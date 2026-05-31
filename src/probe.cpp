// probe.cpp — minimum viable test of the Shadowplay capture stack hosted in our process.
// Goal: LoadLibrary nvspapi64.dll, call CreateShadowPlayApiInterface, report the HRESULT.
// Tells us empirically what dependencies fail, what NvContainer-related state is missing,
// and how far we can get without the rest of the NVIDIA App bloat.

#include <windows.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <io.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <string>
#include "IShadowPlayApi.h"

#pragma comment(lib, "shlwapi.lib")

static std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}

static void PrintLastError(const char* what) {
    DWORD e = GetLastError();
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                   reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    fprintf(stderr, "[!] %s failed (GLE=0x%08lx): %ls\n", what, e, msg ? msg : L"(no msg)");
    if (msg) LocalFree(msg);
}

static HMODULE LoadFromHere(const wchar_t* name) {
    std::wstring p = ExeDir() + L"\\" + name;
    HMODULE h = LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (h) {
        wprintf(L"[+] loaded %s @ %p\n", name, h);
    } else {
        wprintf(L"[-] failed to load %s\n", name);
        PrintLastError("LoadLibraryExW");
    }
    return h;
}

int wmain() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    // Make the bin/ folder (where probe.exe lives) the primary DLL search dir.
    SetDllDirectoryW(ExeDir().c_str());

    // Spawn NvContainer in child mode unless an external one is already running
    // (e.g. spawned by a SYSTEM-elevated launcher for the privilege-escalation test).
    bool externalNvc = false;
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{ sizeof(pe) };
            for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
                if (_wcsicmp(pe.szExeFile, L"NvContainer.exe") == 0) { externalNvc = true; break; }
            }
            CloseHandle(snap);
        }
    }

    PROCESS_INFORMATION pi{};
    if (externalNvc) {
        wprintf(L"[+] external NvContainer detected — skipping our spawn\n");
    } else {
        std::wstring ncDir = ExeDir() + L"\\nvcontainer";
        std::wstring ncExe = ncDir + L"\\NvContainer.exe";
        std::wstring ncCmd = L"\"" + ncExe + L"\" -c -d plugins -f nvc.log -l 7 -r";
        STARTUPINFOW si{ sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        std::wstring mutableCmd = ncCmd;
        if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, ncDir.c_str(), &si, &pi)) {
            PrintLastError("CreateProcessW(NvContainer)");
            return 3;
        }
        wprintf(L"[+] NvContainer spawned (child), PID=%lu\n", pi.dwProcessId);
    }
    Sleep(2500);  // Let the plugin reach 'Started' state before we connect.

    // Preload nvapi64.dll from System32 so CheckShadowPlaySupport's GPU arch queries work.
    LoadLibraryW(L"nvapi64.dll");

    // Client side: load the API.
    HMODULE hApi = LoadFromHere(L"nvspapi64.dll");
    if (!hApi) {
        fprintf(stderr, "[!] nvspapi64.dll did not load; aborting probe.\n");
        return 1;
    }

    auto pCreate = reinterpret_cast<PFN_CreateShadowPlayApiInterface>(
        GetProcAddress(hApi, "CreateShadowPlayApiInterface"));
    if (!pCreate) {
        PrintLastError("GetProcAddress(CreateShadowPlayApiInterface)");
        return 2;
    }
    printf("[+] CreateShadowPlayApiInterface @ %p\n", pCreate);

    IShadowPlayApi* iface = nullptr;
    SP_CREATE_INTERFACE_ARGS args = {};
    args.version      = SpVersion::Create;          // 0x10018
    args.ifaceVersion = SpVersion::IfaceMin;        // 0x10008
    args.clientId     = 10;                         // ShadowPlayApi_TestingTool (see ipccommon64 name table)
    args.pad          = 0;
    args.ppvInterface = reinterpret_cast<void**>(&iface);

    HRESULT hr = pCreate(&args);
    printf("[*] CreateShadowPlayApiInterface returned HRESULT=0x%08lX, iface=%p\n",
           static_cast<unsigned long>(hr), iface);

    if (SUCCEEDED(hr) && iface) {
        printf("[+] vtable @ %p\n", iface->vt);

        // Probe every method with a minimum versioned-args struct.
        // We're looking for the error-signature pattern that tells us whether
        // the bus is joined (== reachable to NvContainer or our future local bus).
        struct VerArg { uint32_t ver; uint64_t pad[8]; } va;

        auto call = [&](const char* name, uint32_t ver, SpMethod fn) {
            memset(&va, 0, sizeof(va));
            va.ver = ver;
            HRESULT r = fn(iface, &va);
            printf("    %-30s ver=0x%05x -> 0x%08lX\n", name, ver,
                   static_cast<unsigned long>(r));
        };

        printf("[*] GetStatus:\n");
        {
            // GetStatus args: +0 version, +4 status flags (OUT)
            // From CaptureCore.log format: "enabled(%d) user(%d) running(%d)"
            // The status flags at +4 are a bitfield: bit0=MF, bit1=GPU, bit2=driver, bit3=OS, bit4=RAM, bit5=allgood
            struct StatusArgs { uint32_t ver; uint32_t flags; uint64_t pad[7]; } sa = {};
            sa.ver = SpVersion::Status;
            HRESULT sr = iface->vt->GetStatus(iface, &sa);
            printf("    HRESULT=0x%08lX flags=0x%08X\n", (unsigned long)sr, sa.flags);
            // Dump all bytes of the response for analysis
            auto* raw = reinterpret_cast<uint32_t*>(&sa);
            printf("    raw dwords: ");
            for (int i = 0; i < 16; ++i) printf("%08X ", raw[i]);
            printf("\n");
        }
        call("RegisterCallback",          SpVersion::RegisterCallback, iface->vt->RegisterCallback);

        // CORRECTED args layout from IDA decompile of EnableShadowPlay @ 0x18007d970:
        //   +0  DWORD version (0x10010 = 65552)
        //   +4  DWORD field (read as `*(a2+4)` and forwarded to bus payload, exact semantics unknown)
        //   +8  DWORD flags  (logged as "flags(%d)")
        //   +12 DWORD origin (logged as "origin(%d)"; binary checks origin==3 for DriverReload path)
        struct EnableArgs {
            uint32_t version;
            uint32_t field4;
            uint32_t flags;
            uint32_t origin;
        };

        // EnableShadowPlay — re-enabled now that we preload nvapi64.
        // The IDA decompile shows it calls CheckShadowPlaySupport first,
        // and only proceeds if flags & 0x20 (all checks pass).
        printf("[*] EnableShadowPlay (origin=1 ServiceRestart):\n");
        {
            struct EnableArgs { uint32_t ver; uint32_t field4; uint32_t flags; uint32_t origin; uint64_t pad[4]; } ea = {};
            ea.ver = SpVersion::EnableDisable;
            ea.flags = 0;
            ea.origin = 1; // ServiceRestart
            HRESULT er = iface->vt->EnableShadowPlay(iface, &ea);
            printf("    HRESULT=0x%08lX\n", (unsigned long)er);
        }

        // CreateCaptureSession — CORRECTED version is 0x10020 (NOT 0x10060).
        // Args from IDA: +0=ver, +4=??, +16=sessiontype, +20=capcontroller, +24=??
        printf("[*] CreateCaptureSession (ver=0x10020):\n");
        struct CreateSessionArgs {
            uint32_t version;       // +0  0x10020
            uint32_t field4;        // +4
            uint8_t  pad8[8];       // +8..+15
            uint32_t sessionType;   // +16
            uint32_t capController; // +20
            uint32_t field24;       // +24
            void*    pad28;
        };
        CreateSessionArgs cs = {};
        cs.version       = 0x10020;
        cs.sessionType   = 1;  // 1 = video capture (confirmed from CaptureCore.log)
        cs.capController = 3;  // 3 = OSCUI controller (confirmed: real overlay uses capcontroller=3)
        HRESULT cr = iface->vt->CreateCaptureSession(iface, &cs);
        printf("    ver=0x10020 sessionType=1 -> 0x%08lX\n", static_cast<unsigned long>(cr));
        if (SUCCEEDED(cr)) {
            printf("    [+] session handle stored at args+8 (out): 0x%08x%08x\n",
                   *(uint32_t*)(((char*)&cs)+12),
                   *(uint32_t*)(((char*)&cs)+8));
        }

        if (SUCCEEDED(cr)) {
            uint64_t hSession = *reinterpret_cast<uint64_t*>(((char*)&cs) + 8);
            printf("    session handle: 0x%llX\n", hSession);

            // CaptureSessionControl — start recording.
            // From IDA: version range [0x10058..0x50058].
            // Args: +0=ver, +8=hSession, +20=eControlCmd
            // eControlCmd: 1=start, 2=stop, 3=save-instant-replay (guesses)
            printf("[*] CaptureSessionControl (start recording, cmd=1):\n");
            struct ControlArgs {
                uint32_t version;       // +0
                uint32_t pad4;          // +4
                uint64_t hSession;      // +8
                uint8_t  pad16[24];     // +16..+39
                uint32_t eControlCmd;   // +40 (verified from IDA: *(DWORD*)(a2+40))
                uint8_t  pad44[44];     // +44..
            } ctrl = {};
            ctrl.version = 0x10058;     // 65592
            ctrl.hSession = hSession;
            ctrl.eControlCmd = 3;  // 3 = IR_START (instant replay start, from real CaptureCore.log)
            HRESULT ctrlR = iface->vt->CaptureSessionControl(iface, &ctrl);
            printf("    HRESULT=0x%08lX\n", (unsigned long)ctrlR);

            if (SUCCEEDED(ctrlR) || ctrlR == 0) {
                printf("[+] INSTANT REPLAY STARTED! Buffer filling for 10 seconds...\n");
                Sleep(10000);

                // Save the instant replay (cmd=4 might be IR_SAVE, or cmd=2 for stop)
                printf("[*] CaptureSessionControl (save replay, cmd=4):\n");
                ctrl.eControlCmd = 4;  // try IR_SAVE
                HRESULT saveR = iface->vt->CaptureSessionControl(iface, &ctrl);
                printf("    HRESULT=0x%08lX\n", (unsigned long)saveR);

                if (saveR != 0) {
                    printf("[*] Trying cmd=5 (alt save):\n");
                    ctrl.eControlCmd = 5;
                    saveR = iface->vt->CaptureSessionControl(iface, &ctrl);
                    printf("    HRESULT=0x%08lX\n", (unsigned long)saveR);
                }
            }

            // Destroy the session
            printf("[*] DestroyCaptureSession:\n");
            struct DestroyArgs {
                uint32_t version;
                uint32_t pad4;
                uint64_t hSession;
                uint64_t pad[4];
            } da = {};
            da.version = 0x10010;
            da.hSession = hSession;
            HRESULT dr = iface->vt->DestroyCaptureSession(iface, &da);
            printf("    HRESULT=0x%08lX\n", (unsigned long)dr);
        }

        // ReleaseInterface
        printf("[*] ReleaseInterface:\n");
        {
            memset(&va, 0, sizeof(va));
            va.ver = 0x10008;
            HRESULT r = iface->vt->ReleaseInterface(iface, &va);
            printf("    HRESULT=0x%08lX\n", (unsigned long)r);
        }
    } else {
        printf("[-] Interface creation failed. Common HRESULT meanings:\n");
        printf("    0x80070057 E_INVALIDARG  — args struct version/clientId rejected\n");
        printf("    0x80004001 E_NOTIMPL\n");
        printf("    0x800706BA RPC_S_SERVER_UNAVAILABLE — NvContainer service not running\n");
    }

    printf("\n[done]\n");

    // Clean shutdown of the spawned container.
    if (pi.hProcess) {
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 2000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return 0;
}
