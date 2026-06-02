// shadowplay.cpp — Standalone ShadowPlay Controller
// Drives NVIDIA's ShadowPlay capture modules without the NVIDIA App or overlay.
// Sits in the system tray, registers global hotkeys, zero CPU when idle.
//
// Build: cl /nologo /std:c++17 /EHsc /O2 /DUNICODE /D_UNICODE shadowplay.cpp
//        /Fe:ShadowPlay.exe /link /SUBSYSTEM:WINDOWS shell32.lib shlwapi.lib user32.lib advapi32.lib
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include "IShadowPlayApi.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---- Constants ----
static constexpr UINT WM_TRAYICON     = WM_APP + 1;
static constexpr UINT HOTKEY_SAVE_IR  = 1;   // Alt+F10: save instant replay
static constexpr UINT HOTKEY_RECORD   = 2;   // Alt+F9: toggle manual record
static constexpr UINT HOTKEY_IR_TOGGLE = 3;  // Alt+Shift+F10: toggle instant replay on/off

static constexpr UINT TIMER_IR_RETRY  = 1;   // periodic auto-resume when capture was blocked
// CCaptureSession::IsCaptureAllowed returns this when DRM/protected content
// (Netflix, Spotify, protected YouTube in a browser) is detected on screen.
// The capture engine refuses to record in that case — by design, not a bug.
static constexpr HRESULT SP_E_PROTECTED_CONTENT = (HRESULT)0x80040237;

static constexpr UINT IDM_STATUS      = 100;
static constexpr UINT IDM_SAVE_IR     = 101;
static constexpr UINT IDM_RECORD      = 102;
static constexpr UINT IDM_IR_TOGGLE   = 103;
static constexpr UINT IDM_MIC_TOGGLE  = 104;
static constexpr UINT IDM_SETTINGS    = 105;
static constexpr UINT IDM_QUIT        = 106;

// ---- ShadowPlay API state ----
static HMODULE              g_hApiDll     = nullptr;
static IShadowPlayApi*      g_iface       = nullptr;
static uint64_t             g_hSession    = 0;
static bool                 g_irRunning   = false;
static bool                 g_irDesired   = false;  // user/init wants IR on; drives auto-resume
static bool                 g_drmNotified = false;  // suppress repeat DRM-blocked balloons
static bool                 g_recording   = false;
static bool                 g_micEnabled  = true;
static NOTIFYICONDATAW      g_nid         = {};
static HWND                 g_hwnd        = nullptr;

// Concurrency: capture init/restart runs on a worker thread so the tray stays
// responsive. g_spLock serializes all access to g_iface/g_hSession; g_ready
// gates user actions until the first session exists; g_restarting prevents
// overlapping service restarts.
static CRITICAL_SECTION     g_spLock;
static volatile LONG        g_ready       = 0;
static volatile LONG        g_restarting  = 0;

// ---- Helpers ----
static std::wstring ExeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    PathRemoveFileSpecW(p);
    return p;
}

static void Log(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

static void ShowBalloon(const wchar_t* title, const wchar_t* msg, DWORD icon = NIIF_INFO) {
    g_nid.uFlags = NIF_INFO;
    wcsncpy_s(g_nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(g_nid.szInfo, msg, _TRUNCATE);
    g_nid.dwInfoFlags = icon;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
}

static void UpdateTip() {
    const wchar_t* status = L"ShadowPlay: Disconnected";
    if (g_restarting)
        status = L"ShadowPlay: Restarting…";
    else if (!g_ready)
        status = L"ShadowPlay: Starting…";
    else if (g_iface && g_irRunning && g_recording)
        status = L"ShadowPlay: IR Active + Recording";
    else if (g_iface && g_irRunning)
        status = L"ShadowPlay: Instant Replay Active";
    else if (g_iface && g_recording)
        status = L"ShadowPlay: Recording";
    else if (g_iface)
        status = L"ShadowPlay: Ready (IR Off)";
    wcsncpy_s(g_nid.szTip, status, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static bool IsProcessRunning(const wchar_t* name) {
    bool found = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{ sizeof(pe) };
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            if (_wcsicmp(pe.szExeFile, name) == 0) { found = true; break; }
        }
        CloseHandle(snap);
    }
    return found;
}

// The capture engine's DVR/Instant-Replay path needs the "NVIDIA GeForce
// Overlay" window + shared memory to exist before EnableShadowPlay, otherwise
// StartServer times out (0x800705b4) and IR_START fails (0x80040237). The
// server is supposed to spawn the overlay itself but does so unreliably, so we
// ensure our lightweight stub is running first. Idempotent (stub is
// single-instance), so it's safe to call on every init/restart.
static void EnsureOverlayRunning() {
    if (IsProcessRunning(L"NVIDIA Overlay.exe")) return;
    std::wstring local = ExeDir() + L"\\NVIDIA Overlay.exe";
    const wchar_t* candidates[] = {
        L"C:\\Program Files\\NVIDIA Corporation\\NVIDIA App\\CEF\\NVIDIA Overlay.exe",
        local.c_str()
    };
    for (const wchar_t* p : candidates) {
        if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) continue;
        STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
        std::wstring cmd = p;                       // CreateProcess may write to arg
        if (CreateProcessW(p, &cmd[0], nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            Sleep(600); // let it register the window + map the shared memory
            Log("Launched dummy overlay: %ls", p);
            return;
        }
    }
    Log("Could not launch dummy overlay (not found)");
}

// ---- ShadowPlay API wrappers ----
static bool SpInit() {
    SetDllDirectoryW(ExeDir().c_str());
    LoadLibraryW(L"nvapi64.dll");

    g_hApiDll = LoadLibraryW(L"nvspapi64.dll");
    if (!g_hApiDll) {
        // Try the canonical install path
        g_hApiDll = LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVIDIA App\\ShadowPlay\\nvspapi64.dll");
    }
    if (!g_hApiDll) { Log("Failed to load nvspapi64.dll"); return false; }

    auto pCreate = (PFN_CreateShadowPlayApiInterface)
        GetProcAddress(g_hApiDll, "CreateShadowPlayApiInterface");
    if (!pCreate) { Log("CreateShadowPlayApiInterface not found"); return false; }

    SP_CREATE_INTERFACE_ARGS args = {};
    args.version      = SpVersion::Create;
    args.ifaceVersion = SpVersion::IfaceMin;
    args.clientId     = 10; // ShadowPlayApi_TestingTool
    args.ppvInterface = reinterpret_cast<void**>(&g_iface);

    HRESULT hr = pCreate(&args);
    if (FAILED(hr) || !g_iface) { Log("CreateShadowPlayApiInterface failed: 0x%08X", hr); return false; }
    Log("ShadowPlay API initialized, iface=%p", g_iface);
    // g_micEnabled set later after RegReadDword is defined
    return true;
}

static bool SpEnable() {
    if (!g_iface) return false;
    struct { uint32_t ver; uint32_t f4; uint32_t flags; uint32_t origin; uint64_t pad[4]; } ea = {};
    ea.ver = SpVersion::EnableDisable;
    ea.origin = 1; // ServiceRestart
    HRESULT hr = g_iface->vt->EnableShadowPlay(g_iface, &ea);
    Log("EnableShadowPlay: 0x%08X", hr);
    return true; // non-fatal if it fails — CreateCaptureSession might still work
}

static bool SpCreateSession() {
    if (!g_iface) return false;

    // The overlay window/MMF must exist before EnableShadowPlay or StartServer
    // times out and Instant Replay can't start.
    EnsureOverlayRunning();

    // Enable first — triggers the server to create m_pSPServer
    SpEnable();
    Sleep(2000);

    // Push settings via SetCaptureSessionSettings (slot 12)
    // From IDA: version=0x10100, +8=hSession, +56..+187=settings payload
    {
        struct alignas(8) {
            uint32_t version;        // +0   = 0x10100
            uint32_t pad4;           // +4
            uint64_t hSession;       // +8   = 0xFFFFFFFF (all)
            uint8_t  pad16[40];      // +16..+55
            // Settings payload starts at +56:
            uint32_t width;          // +56
            uint32_t height;         // +60
            uint32_t fpsNum;         // +64
            uint32_t fpsDen;         // +68
            uint32_t bitrate;        // +72  (bps)
            uint32_t maxBitrate;     // +76
            uint32_t gopLength;      // +80
            uint32_t encoderProfile; // +84  (0=Base,1=Main,2=High)
            uint32_t audioMode;      // +88  (3=all)
            uint32_t micMode;        // +92
            uint32_t bufferLenSec;   // +96
            uint32_t captureMode;    // +100 (1=game, 2=desktop)
            uint8_t  rest[88];       // +104..+191
        } ss = {};
        ss.version        = 0x10100;
        ss.hSession       = 0xFFFFFFFFull;
        ss.width          = 1920;
        ss.height         = 1080;
        ss.fpsNum         = 60;
        ss.fpsDen         = 1;
        ss.bitrate        = 30000000;
        ss.maxBitrate     = 50000000;
        ss.gopLength      = 120;
        ss.encoderProfile = 2;
        ss.audioMode      = 3;
        ss.micMode         = 2;
        ss.bufferLenSec   = 120;
        ss.captureMode    = 2;
        HRESULT hr = g_iface->vt->SetCaptureSessionSettings(g_iface, &ss);
        Log("SetCaptureSessionSettings: 0x%08X", hr);
    }
    Sleep(1000);

    struct {
        uint32_t version;
        uint32_t field4;
        uint8_t  pad8[8];
        uint32_t sessionType;
        uint32_t capController;
        uint32_t field24;
        void*    pad28;
    } cs = {};
    cs.version       = 0x10020;
    cs.sessionType   = 1;
    cs.capController = 3;

    // Retry a few times — server may need a moment after EnableShadowPlay
    HRESULT hr = E_FAIL;
    for (int attempt = 0; attempt < 5; attempt++) {
        hr = g_iface->vt->CreateCaptureSession(g_iface, &cs);
        if (SUCCEEDED(hr)) break;
        Log("CreateCaptureSession attempt %d: 0x%08X", attempt + 1, hr);
        Sleep(2000);
    }
    if (FAILED(hr)) { Log("CreateCaptureSession failed after retries: 0x%08X", hr); return false; }
    g_hSession = *reinterpret_cast<uint64_t*>(((char*)&cs) + 8);
    Log("Capture session created: 0x%llX", g_hSession);
    return true;
}

static HRESULT SpSessionControl(uint32_t cmd) {
    if (!g_iface || !g_hSession) return E_FAIL;
    struct {
        uint32_t version;
        uint32_t pad4;
        uint64_t hSession;
        uint8_t  pad16[24];
        uint32_t eControlCmd;
        uint8_t  pad44[44];
    } ctrl = {};
    ctrl.version     = 0x10058;
    ctrl.hSession    = g_hSession;
    ctrl.eControlCmd = cmd;
    HRESULT hr = g_iface->vt->CaptureSessionControl(g_iface, &ctrl);
    if (hr == 0x80070006) { // ERROR_INVALID_HANDLE — session was stolen by overlay
        Log("Session handle invalid, recreating...");
        g_hSession = 0;
        if (SpCreateSession()) {
            ctrl.hSession = g_hSession;
            hr = g_iface->vt->CaptureSessionControl(g_iface, &ctrl);
        }
    }
    return hr;
}

static void SpStartIR() {
    if (g_irRunning) return;
    g_irDesired = true;
    HRESULT hr = SpSessionControl(3); // IR_START
    if (SUCCEEDED(hr)) {
        g_irRunning = true;
        g_drmNotified = false;
        Log("Instant Replay started");
        ShowBalloon(L"ShadowPlay", L"Instant Replay started");
    } else if (hr == SP_E_PROTECTED_CONTENT) {
        // DRM content on screen — the periodic retry timer will auto-resume IR
        // once it clears. Tell the user once, not every retry.
        if (!g_drmNotified) {
            g_drmNotified = true;
            ShowBalloon(L"ShadowPlay",
                L"Instant Replay paused: DRM-protected video (Netflix/Spotify/etc.) "
                L"is on screen. It'll start automatically once that closes.", NIIF_WARNING);
        }
        Log("IR_START blocked by protected content (0x80040237)");
    } else {
        Log("IR_START failed: 0x%08X", hr);
        ShowBalloon(L"ShadowPlay", L"Failed to start Instant Replay", NIIF_ERROR);
    }
    UpdateTip();
}

static void SpStopIR() {
    g_irDesired = false;
    if (!g_irRunning) return;
    SpSessionControl(4); // IR_STOP
    g_irRunning = false;
    Log("Instant Replay stopped");
    ShowBalloon(L"ShadowPlay", L"Instant Replay stopped");
    UpdateTip();
}

// Called from the retry timer (UI thread, under lock) — quietly re-attempt IR
// when it was previously blocked by DRM/protected content. No balloon spam.
static void SpTryResumeIR() {
    if (g_irRunning || !g_irDesired) return;
    if (SUCCEEDED(SpSessionControl(3))) {
        g_irRunning = true;
        g_drmNotified = false;
        Log("Instant Replay auto-resumed");
        ShowBalloon(L"ShadowPlay", L"Instant Replay resumed");
        UpdateTip();
    }
}

static void SpSaveIR() {
    if (!g_irRunning) {
        ShowBalloon(L"ShadowPlay",
            g_irDesired ? L"Instant Replay paused (DRM-protected content on screen)"
                        : L"Instant Replay is not active", NIIF_WARNING);
        return;
    }
    HRESULT hr = SpSessionControl(5); // IR_SAVE
    if (SUCCEEDED(hr)) {
        Log("Instant Replay saved");
        ShowBalloon(L"ShadowPlay", L"Replay saved!");
    } else {
        Log("IR_SAVE (cmd=5) returned 0x%08X, trying cmd=7", hr);
        hr = SpSessionControl(7);
        if (SUCCEEDED(hr)) {
            ShowBalloon(L"ShadowPlay", L"Replay saved!");
        } else {
            Log("IR_SAVE alt failed: 0x%08X", hr);
            ShowBalloon(L"ShadowPlay", L"Failed to save replay", NIIF_ERROR);
        }
    }
}

static void SpStartRecord() {
    if (g_recording) return;
    HRESULT hr = SpSessionControl(1); // START_MANUAL
    if (SUCCEEDED(hr)) {
        g_recording = true;
        Log("Recording started");
        ShowBalloon(L"ShadowPlay", L"Recording started");
    } else if (hr == SP_E_PROTECTED_CONTENT) {
        ShowBalloon(L"ShadowPlay",
            L"Can't record: DRM-protected video (Netflix/Spotify/etc.) is on screen.", NIIF_WARNING);
        Log("START_MANUAL blocked by protected content (0x80040237)");
    } else {
        Log("START_MANUAL failed: 0x%08X", hr);
        ShowBalloon(L"ShadowPlay", L"Failed to start recording", NIIF_ERROR);
    }
    UpdateTip();
}

static void SpStopRecord() {
    if (!g_recording) return;
    SpSessionControl(2); // STOP_MANUAL
    g_recording = false;
    Log("Recording stopped");
    ShowBalloon(L"ShadowPlay", L"Recording stopped and saved");
    UpdateTip();
}

static void SpShutdown() {
    if (g_recording) SpStopRecord();
    if (g_irRunning) SpStopIR();
    if (g_iface && g_hSession) {
        struct { uint32_t ver; uint32_t pad; uint64_t h; uint64_t p[4]; } da = {};
        da.ver = 0x10010; da.h = g_hSession;
        g_iface->vt->DestroyCaptureSession(g_iface, &da);
        g_hSession = 0;
    }
    if (g_iface) {
        struct { uint32_t ver; uint64_t p[8]; } ra = {}; ra.ver = 0x10008;
        g_iface->vt->ReleaseInterface(g_iface, &ra);
        g_iface = nullptr;
    }
}

// ---- Settings dialog (reads/writes NVSPCAPS registry) ----
static const wchar_t* kRegPath = L"SOFTWARE\\NVIDIA Corporation\\Global\\ShadowPlay\\NVSPCAPS";

static uint32_t RegReadDword(const wchar_t* name, uint32_t def) {
    HKEY hk; DWORD val, sz = 4;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        BYTE buf[4];
        if (RegQueryValueExW(hk, name, nullptr, nullptr, buf, &sz) == ERROR_SUCCESS && sz >= 4)
            val = *(uint32_t*)buf;
        else
            val = def;
        RegCloseKey(hk);
        return val;
    }
    return def;
}

static float RegReadFloat(const wchar_t* name, float def) {
    HKEY hk; DWORD sz = 4;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        float val;
        if (RegQueryValueExW(hk, name, nullptr, nullptr, (BYTE*)&val, &sz) == ERROR_SUCCESS && sz >= 4) {
            RegCloseKey(hk); return val;
        }
        RegCloseKey(hk);
    }
    return def;
}

static void RegWriteDword(const wchar_t* name, uint32_t val) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_WRITE, &hk) == ERROR_SUCCESS) {
        RegSetValueExW(hk, name, 0, REG_BINARY, (BYTE*)&val, 4);
        RegCloseKey(hk);
    }
}

static void RegWriteFloat(const wchar_t* name, float val) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_WRITE, &hk) == ERROR_SUCCESS) {
        RegSetValueExW(hk, name, 0, REG_BINARY, (BYTE*)&val, 4);
        RegCloseKey(hk);
    }
}

// Stop+start the NvContainer service and rebuild the capture session.
// Must run under g_spLock (touches g_iface/g_hSession). Blocks ~20s, so it is
// only ever called from a worker thread, never the UI thread.
static void SpRestartServiceCore() {
    SpShutdown();
    g_irRunning = false;
    g_recording = false;
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    wchar_t cmd[] = L"net stop NvContainerLocalSystem";
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 8000);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    Sleep(1000);
    wchar_t cmd2[] = L"net start NvContainerLocalSystem";
    if (CreateProcessW(nullptr, cmd2, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 12000);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    Sleep(4000);
    SpInit();
    SpCreateSession();
    if (g_hSession) SpStartIR();
}

// Worker: first-time capture init. Keeps wWinMain's message loop responsive.
static DWORD WINAPI InitWorker(LPVOID) {
    EnterCriticalSection(&g_spLock);
    bool ok = SpInit() && SpCreateSession();
    if (ok) SpStartIR();
    LeaveCriticalSection(&g_spLock);
    if (ok) {
        InterlockedExchange(&g_ready, 1);
    } else if (!g_iface) {
        ShowBalloon(L"ShadowPlay", L"Failed to connect to ShadowPlay. Is the NVIDIA driver installed?", NIIF_ERROR);
    } else {
        ShowBalloon(L"ShadowPlay", L"Failed to create capture session. Is the ShadowPlay service running?", NIIF_ERROR);
    }
    UpdateTip();
    return 0;
}

// Worker: full service restart (settings change / mic toggle).
static DWORD WINAPI RestartWorker(LPVOID) {
    if (InterlockedExchange(&g_restarting, 1)) return 0; // already restarting
    InterlockedExchange(&g_ready, 0);
    UpdateTip();
    EnterCriticalSection(&g_spLock);
    SpRestartServiceCore();
    LeaveCriticalSection(&g_spLock);
    InterlockedExchange(&g_ready, g_hSession ? 1 : 0);
    InterlockedExchange(&g_restarting, 0);
    ShowBalloon(L"ShadowPlay", L"Settings applied");
    UpdateTip();
    return 0;
}

static void SpAsyncRestart() {
    ShowBalloon(L"ShadowPlay", L"Applying settings… (a few seconds)");
    HANDLE h = CreateThread(nullptr, 0, RestartWorker, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

static void SpToggleMic() {
    g_micEnabled = !g_micEnabled;
    RegWriteDword(L"EnableMicrophone", g_micEnabled ? 1 : 0);
    RegWriteDword(L"MicMode", g_micEnabled ? 2 : 0);
    RegWriteDword(L"AudioMode", g_micEnabled ? 3 : 1);
    Log("Microphone %s — restarting service", g_micEnabled ? "ON" : "OFF");
    SpAsyncRestart();
}

// ---- Audio endpoint selection (undocumented IPolicyConfig) ----
// The capture engine records the Windows *default* render/capture endpoints
// ("Assigning Default Mic"). To let the user pick a device, we set their choice
// as the default endpoint — which the engine then honors. Reversible via the
// normal Windows Sound control panel.
static const CLSID kCLSID_PolicyConfig =
    { 0x870af99c, 0x171d, 0x4f9e, { 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9 } };
static const IID kIID_PolicyConfig =
    { 0xf8679f50, 0x850a, 0x41cf, { 0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8 } };

struct IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, INT, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, INT, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR wszDeviceId, ERole eRole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

static void SetDefaultAudioEndpoint(const wchar_t* devId) {
    if (!devId || !*devId) return;
    IPolicyConfig* pc = nullptr;
    if (SUCCEEDED(CoCreateInstance(kCLSID_PolicyConfig, nullptr, CLSCTX_ALL,
            kIID_PolicyConfig, (void**)&pc)) && pc) {
        pc->SetDefaultEndpoint(devId, eConsole);
        pc->SetDefaultEndpoint(devId, eMultimedia);
        pc->SetDefaultEndpoint(devId, eCommunications);
        pc->Release();
    }
}

// Device IDs parallel to the combo entries, captured at dialog-build time so
// the IDOK handler can map the selected index back to an endpoint ID.
static std::vector<std::wstring> g_renderIds;
static std::vector<std::wstring> g_captureIds;

static void EnumAudioDevices(HWND hCombo, EDataFlow flow, std::vector<std::wstring>& ids) {
    ids.clear();
    bool comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    IMMDeviceEnumerator* pEnum = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), (void**)&pEnum)) && pEnum) {
        LPWSTR defaultId = nullptr;
        IMMDevice* pDefault = nullptr;
        if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(flow, eConsole, &pDefault)) && pDefault) {
            pDefault->GetId(&defaultId);
            pDefault->Release();
        }
        IMMDeviceCollection* pCol = nullptr;
        if (SUCCEEDED(pEnum->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &pCol)) && pCol) {
            UINT count = 0;
            pCol->GetCount(&count);
            int defaultIdx = 0;
            for (UINT i = 0; i < count; i++) {
                IMMDevice* pDev = nullptr;
                if (SUCCEEDED(pCol->Item(i, &pDev)) && pDev) {
                    LPWSTR devId = nullptr;
                    pDev->GetId(&devId);
                    IPropertyStore* pProps = nullptr;
                    if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
                        PROPVARIANT name;
                        PropVariantInit(&name);
                        if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &name)) && name.pwszVal) {
                            SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)name.pwszVal);
                            ids.push_back(devId ? devId : L"");
                            if (defaultId && devId && wcscmp(devId, defaultId) == 0)
                                defaultIdx = (int)ids.size() - 1;
                        }
                        PropVariantClear(&name);
                        pProps->Release();
                    }
                    if (devId) CoTaskMemFree(devId);
                    pDev->Release();
                }
            }
            SendMessageW(hCombo, CB_SETCURSEL, defaultIdx, 0);
            pCol->Release();
        }
        if (defaultId) CoTaskMemFree(defaultId);
        pEnum->Release();
    }
    if (comInit) CoUninitialize();
}

static HWND g_hSettings = nullptr;

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
            0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
            HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                x, y, w, h, hwnd, (HMENU)(intptr_t)id, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)hFont, TRUE);
            return c;
        };

        int y = 15;
        mk(L"STATIC", L"Resolution:", 0, 15, y+3, 80, 20, -1);
        mk(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_NUMBER, 100, y, 60, 24, 201);
        mk(L"STATIC", L"x", SS_CENTER, 162, y+3, 16, 20, -1);
        mk(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_NUMBER, 180, y, 60, 24, 202);

        y += 35;
        mk(L"STATIC", L"FPS:", 0, 15, y+3, 80, 20, -1);
        mk(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_NUMBER, 100, y, 60, 24, 203);

        y += 35;
        mk(L"STATIC", L"Buffer (sec):", 0, 15, y+3, 80, 20, -1);
        mk(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_NUMBER, 100, y, 60, 24, 204);

        y += 35;
        mk(L"STATIC", L"Encoder:", 0, 15, y+3, 80, 20, -1);
        HWND hCombo = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 100, y, 140, 120, 205);
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Base");
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Main");
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"High");

        y += 35;
        mk(L"STATIC", L"Output:", 0, 15, y+3, 80, 20, -1);
        HWND hOut = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 100, y, 190, 200, 207);
        EnumAudioDevices(hOut, eRender, g_renderIds);

        y += 35;
        mk(L"STATIC", L"Input:", 0, 15, y+3, 80, 20, -1);
        HWND hIn = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 100, y, 190, 200, 208);
        EnumAudioDevices(hIn, eCapture, g_captureIds);

        y += 35;
        mk(L"BUTTON", L"Microphone in clips", BS_AUTOCHECKBOX | WS_TABSTOP, 15, y, 150, 20, 206);
        CheckDlgButton(hwnd, 206, g_micEnabled ? BST_CHECKED : BST_UNCHECKED);

        y += 30;
        mk(L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, 60, y, 80, 30, IDOK);
        mk(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 155, y, 80, 30, IDCANCEL);

        // Populate from registry
        wchar_t buf[64];
        swprintf(buf, 64, L"%u", RegReadDword(L"VideoWidth", 1920));  SetDlgItemTextW(hwnd, 201, buf);
        swprintf(buf, 64, L"%u", RegReadDword(L"VideoHeight", 1080)); SetDlgItemTextW(hwnd, 202, buf);
        swprintf(buf, 64, L"%.0f", RegReadFloat(L"RecordingFPS", 60.0f)); SetDlgItemTextW(hwnd, 203, buf);
        swprintf(buf, 64, L"%u", RegReadDword(L"DVRBufferLen", 80));  SetDlgItemTextW(hwnd, 204, buf);
        SendDlgItemMessageW(hwnd, 205, CB_SETCURSEL, RegReadDword(L"EncoderProfile", 2), 0);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            wchar_t buf[64];
            GetDlgItemTextW(hwnd, 201, buf, 64); RegWriteDword(L"VideoWidth", _wtoi(buf));
            GetDlgItemTextW(hwnd, 202, buf, 64); RegWriteDword(L"VideoHeight", _wtoi(buf));
            GetDlgItemTextW(hwnd, 203, buf, 64);
            int fps = _wtoi(buf);
            if (fps < 1) fps = 30; if (fps > 120) fps = 120;
            RegWriteFloat(L"RecordingFPS", (float)fps);
            GetDlgItemTextW(hwnd, 204, buf, 64); RegWriteDword(L"DVRBufferLen", _wtoi(buf));
            RegWriteDword(L"EncoderProfile", (uint32_t)SendDlgItemMessageW(hwnd, 205, CB_GETCURSEL, 0, 0));
            g_micEnabled = IsDlgButtonChecked(hwnd, 206) == BST_CHECKED;
            RegWriteDword(L"EnableMicrophone", g_micEnabled ? 1 : 0);
            RegWriteDword(L"MicMode", g_micEnabled ? 2 : 0);
            RegWriteDword(L"AudioMode", g_micEnabled ? 3 : 1);

            // Apply chosen audio devices by making them the Windows default,
            // which the capture engine then records from.
            bool comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
            int oi = (int)SendDlgItemMessageW(hwnd, 207, CB_GETCURSEL, 0, 0);
            if (oi >= 0 && oi < (int)g_renderIds.size())
                SetDefaultAudioEndpoint(g_renderIds[oi].c_str());
            int ii = (int)SendDlgItemMessageW(hwnd, 208, CB_GETCURSEL, 0, 0);
            if (ii >= 0 && ii < (int)g_captureIds.size())
                SetDefaultAudioEndpoint(g_captureIds[ii].c_str());
            if (comInit) CoUninitialize();

            DestroyWindow(hwnd);
            Log("Settings saved — restarting service to apply");
            SpAsyncRestart();
        } else if (LOWORD(wp) == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
        g_hSettings = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowSettingsDialog() {
    if (g_hSettings) { SetForegroundWindow(g_hSettings); return; }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = SettingsWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"SPSettings";
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        registered = true;
    }

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int ww = 320, wh = 430;
    g_hSettings = CreateWindowExW(WS_EX_TOOLWINDOW, L"SPSettings", L"ShadowPlay Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        (sw - ww) / 2, (sh - wh) / 2, ww, wh,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(g_hSettings, SW_SHOW);
    SetForegroundWindow(g_hSettings);
}

// Gate a capture action on readiness and take the lock so it can't race the
// restart worker freeing g_iface. Returns false (with feedback) if not ready.
static bool SpEnter() {
    if (!g_ready) {
        ShowBalloon(L"ShadowPlay",
            g_restarting ? L"Restarting — one moment…" : L"Still starting up…", NIIF_WARNING);
        return false;
    }
    if (!TryEnterCriticalSection(&g_spLock)) {
        ShowBalloon(L"ShadowPlay", L"Busy — try again in a moment", NIIF_WARNING);
        return false;
    }
    return true;
}
static void SpLeave() { LeaveCriticalSection(&g_spLock); }

// ---- Tray icon + window ----
static void ShowContextMenu() {
    POINT pt; GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();

    wchar_t status[128];
    swprintf(status, 128, L"Status: %s%s",
        g_irRunning ? L"IR Active" : L"IR Off",
        g_recording ? L" | Recording" : L"");
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED, IDM_STATUS, status);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING | (g_irRunning ? 0 : MF_GRAYED), IDM_SAVE_IR, L"Save Replay\tAlt+F10");
    AppendMenuW(hMenu, MF_STRING, IDM_RECORD, g_recording ? L"Stop Recording\tAlt+F9" : L"Start Recording\tAlt+F9");
    AppendMenuW(hMenu, MF_STRING, IDM_IR_TOGGLE, g_irRunning ? L"Stop Instant Replay" : L"Start Instant Replay");
    AppendMenuW(hMenu, MF_STRING | (g_micEnabled ? MF_CHECKED : 0), IDM_MIC_TOGGLE, L"Microphone");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_SETTINGS, L"Settings...");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_QUIT, L"Quit");

    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(hMenu);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU)
            ShowContextMenu();
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK)
            ShowSettingsDialog();
        return 0;

    case WM_TIMER:
        if (wp == TIMER_IR_RETRY && g_ready && g_irDesired && !g_irRunning && !g_restarting) {
            if (TryEnterCriticalSection(&g_spLock)) {
                SpTryResumeIR();
                LeaveCriticalSection(&g_spLock);
            }
        }
        return 0;

    case WM_HOTKEY:
        if (SpEnter()) {
            if (wp == HOTKEY_SAVE_IR)     SpSaveIR();
            else if (wp == HOTKEY_RECORD) { if (g_recording) SpStopRecord(); else SpStartRecord(); }
            else if (wp == HOTKEY_IR_TOGGLE) { if (g_irRunning) SpStopIR(); else SpStartIR(); }
            SpLeave();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_SAVE_IR:    if (SpEnter()) { SpSaveIR(); SpLeave(); } break;
        case IDM_RECORD:     if (SpEnter()) { if (g_recording) SpStopRecord(); else SpStartRecord(); SpLeave(); } break;
        case IDM_IR_TOGGLE:  if (SpEnter()) { if (g_irRunning) SpStopIR(); else SpStartIR(); SpLeave(); } break;
        case IDM_MIC_TOGGLE: SpToggleMic(); break;
        case IDM_SETTINGS:   ShowSettingsDialog(); break;
        case IDM_QUIT:       DestroyWindow(hwnd); break;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_IR_RETRY);
        InterlockedExchange(&g_ready, 0);
        EnterCriticalSection(&g_spLock);   // wait out any in-flight restart
        SpShutdown();
        LeaveCriticalSection(&g_spLock);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        UnregisterHotKey(hwnd, HOTKEY_SAVE_IR);
        UnregisterHotKey(hwnd, HOTKEY_RECORD);
        UnregisterHotKey(hwnd, HOTKEY_IR_TOGGLE);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // Single instance check
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"StandaloneShadowPlay_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Standalone ShadowPlay is already running.", L"ShadowPlay", MB_OK | MB_ICONINFORMATION);
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    InitializeCriticalSection(&g_spLock);

    // Register window class
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInst;
    wc.lpszClassName  = L"StandaloneShadowPlay";
    wc.hIcon          = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"ShadowPlay", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);

    // Tray icon
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"ShadowPlay: Initializing...");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);

    // Kill the NVIDIA hotkey helper — it steals Alt+F10/F9 before we can register them.
    // The helper is needed during server init (creates shared memory for settings), but
    // after the service has been running for a few seconds we can safely kill it.
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{ sizeof(pe) };
            for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
                if (_wcsicmp(pe.szExeFile, L"nvsphelper64.exe") == 0) {
                    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                    if (h) { TerminateProcess(h, 0); CloseHandle(h); Log("Killed nvsphelper64 PID=%u", pe.th32ProcessID); }
                }
            }
            CloseHandle(snap);
        }
        Sleep(500);
    }

    // Global hotkeys (now uncontested)
    RegisterHotKey(g_hwnd, HOTKEY_SAVE_IR,   MOD_ALT | MOD_NOREPEAT, VK_F10);
    RegisterHotKey(g_hwnd, HOTKEY_RECORD,    MOD_ALT | MOD_NOREPEAT, VK_F9);
    RegisterHotKey(g_hwnd, HOTKEY_IR_TOGGLE, MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F10);

    // Auto-resume Instant Replay if it gets blocked (DRM content) and later clears.
    SetTimer(g_hwnd, TIMER_IR_RETRY, 20000, nullptr);

    // Init ShadowPlay API on a worker thread — connecting + creating the capture
    // session takes several seconds (server warm-up + retries). Doing it here
    // would freeze the tray; instead the UI loop starts immediately and the
    // worker flips g_ready when the session is live.
    g_micEnabled = RegReadDword(L"EnableMicrophone", 1) != 0;
    UpdateTip();
    {
        HANDLE h = CreateThread(nullptr, 0, InitWorker, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }

    // Message loop — blocks on GetMessage, zero CPU when idle
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_hSettings && IsDialogMessageW(g_hSettings, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DeleteCriticalSection(&g_spLock);
    if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
    return 0;
}
