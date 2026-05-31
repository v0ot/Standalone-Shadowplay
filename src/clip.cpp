// clip.cpp — true standalone NVENC clipper. No NVIDIA App, no NvContainer,
// no overlay. Public APIs only: D3D11 + DXGI Output Duplication for capture,
// NVENC (nvEncodeAPI64.dll, shipped with the NVIDIA display driver) for
// hardware H.264 encoding. Output is a raw H.264 Annex B stream that VLC
// or `ffmpeg -i clip.h264 -c copy clip.mp4` will accept.
//
// Hotkey:
//   Alt+F10  -> toggle record start/stop
//   Alt+End  -> quit
//
// Build: src\build.bat (re-uses the same vcvars64 + cl.exe path as probe).
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include "nvenc_min.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

static const wchar_t* kNvEncDll = L"nvEncodeAPI64.dll";

#define HRBAIL(call) do { HRESULT _hr = (call); if (FAILED(_hr)) { \
    fprintf(stderr, "[!] %s failed hr=0x%08lX (line %d)\n", #call, (unsigned long)_hr, __LINE__); \
    return false; } } while (0)

#define NVBAIL(call) do { NVENCSTATUS _s = (call); if (_s != NV_ENC_SUCCESS) { \
    fprintf(stderr, "[!] %s failed status=%d (line %d)\n", #call, _s, __LINE__); \
    if (g_enc.fns.nvEncGetLastErrorString && g_enc.session) \
        fprintf(stderr, "    nvenc last: %s\n", g_enc.fns.nvEncGetLastErrorString(g_enc.session)); \
    return false; } } while (0)

// ----- Global encoder/capture state -----
struct EncState {
    HMODULE                        hDll = nullptr;
    NV_ENCODE_API_FUNCTION_LIST    fns{};
    void*                          session = nullptr;
    void*                          bitstream = nullptr;
    void*                          registeredRes = nullptr;
    ID3D11Texture2D*               stagingTex = nullptr;   // R8G8B8A8 / BGRA, the texture we feed NVENC
    uint32_t                       width = 0, height = 0;
    uint32_t                       fpsNum = 60, fpsDen = 1;
    uint32_t                       frameIdx = 0;
} g_enc;

struct CapState {
    ComPtr<ID3D11Device>           dev;
    ComPtr<ID3D11DeviceContext>    ctx;
    ComPtr<IDXGIOutputDuplication> dupl;
    DXGI_OUTPUT_DESC               outDesc{};
} g_cap;

static std::atomic_bool g_recording{false};
static std::atomic_bool g_quitting{false};
static FILE*            g_out = nullptr;
static std::string      g_outPath;

// ----- Capture init -----
static bool InitCapture() {
    UINT createFlags = 0;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL featureLevel;
    HRBAIL(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
        nullptr, 0, D3D11_SDK_VERSION, g_cap.dev.GetAddressOf(),
        &featureLevel, g_cap.ctx.GetAddressOf()));

    ComPtr<IDXGIDevice> dxgiDev;
    HRBAIL(g_cap.dev.As(&dxgiDev));
    ComPtr<IDXGIAdapter> adapter;
    HRBAIL(dxgiDev->GetAdapter(adapter.GetAddressOf()));

    DXGI_ADAPTER_DESC adesc{};
    adapter->GetDesc(&adesc);
    wprintf(L"[i] adapter: %s\n", adesc.Description);

    ComPtr<IDXGIOutput> output0;
    HRBAIL(adapter->EnumOutputs(0, output0.GetAddressOf()));
    output0->GetDesc(&g_cap.outDesc);
    LONG W = g_cap.outDesc.DesktopCoordinates.right  - g_cap.outDesc.DesktopCoordinates.left;
    LONG H = g_cap.outDesc.DesktopCoordinates.bottom - g_cap.outDesc.DesktopCoordinates.top;
    wprintf(L"[i] output[0]: %s  %ldx%ld\n", g_cap.outDesc.DeviceName, W, H);
    g_enc.width = (uint32_t)W;
    g_enc.height = (uint32_t)H;

    ComPtr<IDXGIOutput1> output1;
    HRBAIL(output0.As(&output1));
    HRBAIL(output1->DuplicateOutput(g_cap.dev.Get(), g_cap.dupl.GetAddressOf()));

    // BGRA staging texture for NVENC. ARGB on Windows-side is BGRA byte order;
    // NVENC's NV_ENC_BUFFER_FORMAT_ARGB matches DXGI_FORMAT_B8G8R8A8_UNORM.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = g_enc.width; td.Height = g_enc.height;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRBAIL(g_cap.dev->CreateTexture2D(&td, nullptr, &g_enc.stagingTex));
    return true;
}

// ----- NVENC init -----
static bool InitEncoder() {
    g_enc.hDll = LoadLibraryW(kNvEncDll);
    if (!g_enc.hDll) { fprintf(stderr, "[!] LoadLibrary(%ls) failed\n", kNvEncDll); return false; }
    auto pCreate = (PFN_NvEncodeAPICreateInstance)GetProcAddress(g_enc.hDll, "NvEncodeAPICreateInstance");
    if (!pCreate) { fprintf(stderr, "[!] NvEncodeAPICreateInstance not found\n"); return false; }
    g_enc.fns.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVBAIL(pCreate(&g_enc.fns));

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sp{};
    sp.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sp.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    sp.device = g_cap.dev.Get();
    sp.apiVersion = NVENCAPI_VERSION;
    NVBAIL(g_enc.fns.nvEncOpenEncodeSessionEx(&sp, &g_enc.session));

    NV_ENC_PRESET_CONFIG presetCfg{};
    presetCfg.version = NV_ENC_PRESET_CONFIG_VER;
    presetCfg.presetCfg.version = NV_ENC_CONFIG_VER;
    NVBAIL(g_enc.fns.nvEncGetEncodePresetConfigEx(g_enc.session,
        NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P4_GUID, NV_ENC_TUNING_INFO_HIGH_QUALITY, &presetCfg));

    NV_ENC_CONFIG cfg = presetCfg.presetCfg;
    cfg.version = NV_ENC_CONFIG_VER;
    cfg.gopLength = g_enc.fpsNum * 2;       // keyframe every ~2 sec
    cfg.frameIntervalP = 1;                  // no B-frames; faster to mux raw
    cfg.rcParams.version = NVENCAPI_STRUCT_VERSION(1);
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    cfg.rcParams.averageBitRate = 30 * 1000 * 1000;  // 30 Mbps
    cfg.rcParams.maxBitRate = 50 * 1000 * 1000;
    cfg.encodeCodecConfig.h264Config.idrPeriod = cfg.gopLength;

    NV_ENC_INITIALIZE_PARAMS ip{};
    ip.version = NV_ENC_INITIALIZE_PARAMS_VER;
    ip.encodeGUID = NV_ENC_CODEC_H264_GUID;
    ip.presetGUID = NV_ENC_PRESET_P4_GUID;
    ip.encodeWidth = g_enc.width; ip.encodeHeight = g_enc.height;
    ip.darWidth = g_enc.width;    ip.darHeight = g_enc.height;
    ip.frameRateNum = g_enc.fpsNum; ip.frameRateDen = g_enc.fpsDen;
    ip.enablePTD = 1;
    ip.encodeConfig = &cfg;
    ip.tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    ip.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    NVBAIL(g_enc.fns.nvEncInitializeEncoder(g_enc.session, &ip));

    // One output bitstream buffer is enough for synchronous encode (we lock+unlock per frame).
    NV_ENC_CREATE_BITSTREAM_BUFFER bb{};
    bb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    NVBAIL(g_enc.fns.nvEncCreateBitstreamBuffer(g_enc.session, &bb));
    g_enc.bitstream = bb.bitstreamBuffer;

    // Register the staging texture as a persistent input resource.
    NV_ENC_REGISTER_RESOURCE reg{};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.width = g_enc.width; reg.height = g_enc.height;
    reg.resourceToRegister = g_enc.stagingTex;
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    NVBAIL(g_enc.fns.nvEncRegisterResource(g_enc.session, &reg));
    g_enc.registeredRes = reg.registeredResource;

    printf("[i] NVENC initialized: %ux%u @ %u/%u fps, ~30 Mbps VBR\n",
           g_enc.width, g_enc.height, g_enc.fpsNum, g_enc.fpsDen);
    return true;
}

// ----- Per-frame encode -----
static bool EncodeFrame(ID3D11Texture2D* src, bool eos) {
    // Copy the captured desktop texture into our registered staging texture.
    if (src) g_cap.ctx->CopyResource(g_enc.stagingTex, src);

    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = g_enc.registeredRes;
    if (!eos) NVBAIL(g_enc.fns.nvEncMapInputResource(g_enc.session, &map));

    NV_ENC_PIC_PARAMS pp{};
    pp.version = NV_ENC_PIC_PARAMS_VER;
    pp.inputWidth = g_enc.width;
    pp.inputHeight = g_enc.height;
    pp.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
    pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pp.inputBuffer = eos ? nullptr : map.mappedResource;
    pp.outputBitstream = eos ? nullptr : g_enc.bitstream;
    pp.inputTimeStamp = g_enc.frameIdx;
    pp.encodePicFlags = eos ? 0x4 /*NV_ENC_PIC_FLAG_EOS*/ : 0;
    if (eos) pp.outputBitstream = g_enc.bitstream;
    NVENCSTATUS s = g_enc.fns.nvEncEncodePicture(g_enc.session, &pp);
    if (s != NV_ENC_SUCCESS && s != 11 /*NV_ENC_ERR_NEED_MORE_INPUT*/) {
        fprintf(stderr, "[!] nvEncEncodePicture status=%d\n", s);
        if (g_enc.fns.nvEncGetLastErrorString && g_enc.session)
            fprintf(stderr, "    %s\n", g_enc.fns.nvEncGetLastErrorString(g_enc.session));
        return false;
    }

    // Drain ALL available bitstream output. With no B-frames this is usually 1 NAL per call.
    if (s == NV_ENC_SUCCESS) {
        NV_ENC_LOCK_BITSTREAM lock{};
        lock.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock.outputBitstream = g_enc.bitstream;
        NVENCSTATUS ls = g_enc.fns.nvEncLockBitstream(g_enc.session, &lock);
        if (ls == NV_ENC_SUCCESS) {
            if (g_out && lock.bitstreamSizeInBytes) {
                fwrite(lock.bitstreamBufferPtr, 1, lock.bitstreamSizeInBytes, g_out);
            }
            g_enc.fns.nvEncUnlockBitstream(g_enc.session, g_enc.bitstream);
        }
    }

    if (!eos) g_enc.fns.nvEncUnmapInputResource(g_enc.session, map.mappedResource);
    g_enc.frameIdx++;
    return true;
}

// ----- Capture loop -----
static void CaptureLoop() {
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    const auto period = std::chrono::microseconds(1000000 * g_enc.fpsDen / g_enc.fpsNum);

    while (!g_quitting.load()) {
        if (!g_recording.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            next = clock::now();
            continue;
        }

        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> res;
        HRESULT hr = g_cap.dupl->AcquireNextFrame(15, &info, res.GetAddressOf());
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // No new desktop update — re-encode previous staging tex so the timeline doesn't gap.
            EncodeFrame(nullptr, false);
        } else if (SUCCEEDED(hr)) {
            ComPtr<ID3D11Texture2D> srcTex;
            res.As(&srcTex);
            EncodeFrame(srcTex.Get(), false);
            g_cap.dupl->ReleaseFrame();
        } else {
            fprintf(stderr, "[!] AcquireNextFrame hr=0x%08lX\n", (unsigned long)hr);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        next += period;
        std::this_thread::sleep_until(next);
    }

    // Drain encoder on shutdown.
    if (g_out) {
        EncodeFrame(nullptr, true);
        fclose(g_out);
        g_out = nullptr;
        printf("[i] finalized %s\n", g_outPath.c_str());
    }
}

static void StartRecording() {
    if (g_recording.load()) return;
    SYSTEMTIME st; GetLocalTime(&st);
    char fn[260];
    snprintf(fn, sizeof(fn), "clip_%04u%02u%02u_%02u%02u%02u.h264",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_outPath = fn;
    g_out = fopen(fn, "wb");
    if (!g_out) { fprintf(stderr, "[!] cannot open %s\n", fn); return; }
    g_enc.frameIdx = 0;
    g_recording.store(true);
    printf("[+] recording -> %s\n", fn);
}

static void StopRecording() {
    if (!g_recording.load()) return;
    g_recording.store(false);
    // The capture thread will drain + close g_out on next loop iteration.
    // Wait briefly for it.
    for (int i = 0; i < 50 && g_out; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    printf("[+] stopped\n");
}

int wmain() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    printf("Standalone-Shadowplay clip\n");
    printf("  Alt+F10  toggle record\n");
    printf("  Alt+End  quit\n\n");

    if (!InitCapture()) return 1;
    if (!InitEncoder()) return 2;

    if (!RegisterHotKey(nullptr, 1, MOD_ALT | MOD_NOREPEAT, VK_F10)) {
        fprintf(stderr, "[!] RegisterHotKey(Alt+F10) failed (GLE=%lu) — already taken?\n", GetLastError());
    }
    if (!RegisterHotKey(nullptr, 2, MOD_ALT | MOD_NOREPEAT, VK_END)) {
        fprintf(stderr, "[!] RegisterHotKey(Alt+End) failed (GLE=%lu)\n", GetLastError());
    }

    std::thread capThread(CaptureLoop);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY) {
            if (msg.wParam == 1) {
                if (g_recording.load()) StopRecording(); else StartRecording();
            } else if (msg.wParam == 2) {
                printf("[i] quitting\n");
                if (g_recording.load()) StopRecording();
                g_quitting.store(true);
                break;
            }
        }
    }

    UnregisterHotKey(nullptr, 1);
    UnregisterHotKey(nullptr, 2);
    capThread.join();
    return 0;
}
