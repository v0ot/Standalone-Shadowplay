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
#include <commctrl.h>
#include <cstdio>
#include <cstdint>
#include <string>
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

static constexpr UINT IDM_STATUS      = 100;
static constexpr UINT IDM_SAVE_IR     = 101;
static constexpr UINT IDM_RECORD      = 102;
static constexpr UINT IDM_IR_TOGGLE   = 103;
static constexpr UINT IDM_SETTINGS    = 104;
static constexpr UINT IDM_QUIT        = 105;

// ---- ShadowPlay API state ----
static HMODULE              g_hApiDll     = nullptr;
static IShadowPlayApi*      g_iface       = nullptr;
static uint64_t             g_hSession    = 0;
static bool                 g_irRunning   = false;
static bool                 g_recording   = false;
static NOTIFYICONDATAW      g_nid         = {};
static HWND                 g_hwnd        = nullptr;

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
    if (g_iface && g_irRunning && g_recording)
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
    return true;
}

static bool SpCreateSession() {
    if (!g_iface) return false;
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
    cs.sessionType   = 1;   // video capture
    cs.capController = 3;   // OSCUI (required for IR commands to be accepted)
    HRESULT hr = g_iface->vt->CreateCaptureSession(g_iface, &cs);
    if (FAILED(hr)) { Log("CreateCaptureSession failed: 0x%08X", hr); return false; }
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
    return g_iface->vt->CaptureSessionControl(g_iface, &ctrl);
}

static void SpStartIR() {
    if (g_irRunning) return;
    HRESULT hr = SpSessionControl(3); // IR_START
    if (SUCCEEDED(hr)) {
        g_irRunning = true;
        Log("Instant Replay started");
        ShowBalloon(L"ShadowPlay", L"Instant Replay started");
    } else {
        Log("IR_START failed: 0x%08X", hr);
        ShowBalloon(L"ShadowPlay", L"Failed to start Instant Replay", NIIF_ERROR);
    }
    UpdateTip();
}

static void SpStopIR() {
    if (!g_irRunning) return;
    SpSessionControl(4); // IR_STOP
    g_irRunning = false;
    Log("Instant Replay stopped");
    ShowBalloon(L"ShadowPlay", L"Instant Replay stopped");
    UpdateTip();
}

static void SpSaveIR() {
    if (!g_irRunning) {
        ShowBalloon(L"ShadowPlay", L"Instant Replay is not active", NIIF_WARNING);
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

static void PushSettingsToServer() {
    if (!g_iface) return;
    // SetCaptureSessionParam(hSession=0xFFFFFFFF, cmd=20) tells the server to re-read registry
    struct { uint32_t ver; uint64_t p[8]; } pa = {};
    pa.ver = SpVersion::SessionParam;
    // hSession at +8 = 0xFFFFFFFF (all sessions)
    *reinterpret_cast<uint64_t*>(((char*)&pa) + 8) = 0xFFFFFFFFull;
    // cmd at some offset — from the log it's cmd=20
    // For SetCaptureSessionParam the layout needs more RE; for now the registry
    // write is sufficient since the server reads on RefreshSettings
    g_iface->vt->SetCaptureSessionParam(g_iface, &pa);
}

static INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        uint32_t w = RegReadDword(L"VideoWidth", 1920);
        uint32_t h = RegReadDword(L"VideoHeight", 1080);
        float fps = RegReadFloat(L"RecordingFPS", 60.0f);
        uint32_t bufLen = RegReadDword(L"DVRBufferLen", 80);
        uint32_t enc = RegReadDword(L"EncoderProfile", 2);

        wchar_t buf[64];
        swprintf(buf, 64, L"%u", w); SetDlgItemTextW(hDlg, 201, buf);
        swprintf(buf, 64, L"%u", h); SetDlgItemTextW(hDlg, 202, buf);
        swprintf(buf, 64, L"%.0f", fps); SetDlgItemTextW(hDlg, 203, buf);
        swprintf(buf, 64, L"%u", bufLen); SetDlgItemTextW(hDlg, 204, buf);

        // Encoder profile: 0=Base, 1=Main, 2=High
        HWND hCombo = GetDlgItem(hDlg, 205);
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Base");
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Main");
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"High");
        SendMessageW(hCombo, CB_SETCURSEL, enc, 0);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            wchar_t buf[64];
            GetDlgItemTextW(hDlg, 201, buf, 64); RegWriteDword(L"VideoWidth", _wtoi(buf));
            GetDlgItemTextW(hDlg, 202, buf, 64); RegWriteDword(L"VideoHeight", _wtoi(buf));
            GetDlgItemTextW(hDlg, 203, buf, 64); RegWriteFloat(L"RecordingFPS", (float)_wtof(buf));
            GetDlgItemTextW(hDlg, 204, buf, 64); RegWriteDword(L"DVRBufferLen", _wtoi(buf));
            uint32_t enc = (uint32_t)SendDlgItemMessageW(hDlg, 205, CB_GETCURSEL, 0, 0);
            RegWriteDword(L"EncoderProfile", enc);
            PushSettingsToServer();
            ShowBalloon(L"ShadowPlay", L"Settings saved. Restart IR to apply.");
            EndDialog(hDlg, IDOK);
        } else if (LOWORD(wp) == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
        }
        return TRUE;
    }
    return FALSE;
}

static void ShowSettingsDialog() {
    // Build dialog template in memory (no .rc file needed = single-file build)
    alignas(4) BYTE dlgBuf[4096] = {};
    DLGTEMPLATE* dt = (DLGTEMPLATE*)dlgBuf;
    dt->style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT;
    dt->cdit = 13; // number of controls
    dt->cx = 220; dt->cy = 165;

    WORD* p = (WORD*)(dt + 1);
    *p++ = 0; *p++ = 0; // menu, class
    // title
    const wchar_t* title = L"ShadowPlay Settings";
    while (*title) *p++ = *title++;
    *p++ = 0;
    // font
    *p++ = 9; // point size
    const wchar_t* font = L"Segoe UI";
    while (*font) *p++ = *font++;
    *p++ = 0;

    auto addCtrl = [&](DWORD style, short x, short y, short cx, short cy, WORD id, const wchar_t* cls, const wchar_t* text) {
        p = (WORD*)(((uintptr_t)p + 3) & ~3); // align to DWORD
        DLGITEMTEMPLATE* item = (DLGITEMTEMPLATE*)p;
        item->style = style | WS_CHILD | WS_VISIBLE;
        item->x = x; item->y = y; item->cx = cx; item->cy = cy;
        item->id = id;
        p = (WORD*)(item + 1);
        while (*cls) *p++ = *cls++;
        *p++ = 0;
        while (*text) *p++ = *text++;
        *p++ = 0;
        *p++ = 0; // extra data
    };

    short row = 10;
    addCtrl(SS_LEFT, 10, row, 60, 10, -1, L"Static", L"Width:");
    addCtrl(ES_NUMBER | WS_BORDER | WS_TABSTOP, 75, row-2, 50, 14, 201, L"Edit", L"");
    addCtrl(SS_LEFT, 130, row, 40, 10, -1, L"Static", L"Height:");
    addCtrl(ES_NUMBER | WS_BORDER | WS_TABSTOP, 170, row-2, 40, 14, 202, L"Edit", L"");

    row += 22;
    addCtrl(SS_LEFT, 10, row, 60, 10, -1, L"Static", L"FPS:");
    addCtrl(ES_NUMBER | WS_BORDER | WS_TABSTOP, 75, row-2, 50, 14, 203, L"Edit", L"");

    row += 22;
    addCtrl(SS_LEFT, 10, row, 60, 10, -1, L"Static", L"Buffer (sec):");
    addCtrl(ES_NUMBER | WS_BORDER | WS_TABSTOP, 75, row-2, 50, 14, 204, L"Edit", L"");

    row += 22;
    addCtrl(SS_LEFT, 10, row, 60, 10, -1, L"Static", L"Encoder:");
    addCtrl(CBS_DROPDOWNLIST | WS_TABSTOP, 75, row-2, 80, 80, 205, L"ComboBox", L"");

    row += 28;
    addCtrl(BS_DEFPUSHBUTTON | WS_TABSTOP, 60, row, 45, 16, IDOK, L"Button", L"Save");
    addCtrl(BS_PUSHBUTTON | WS_TABSTOP, 115, row, 45, 16, IDCANCEL, L"Button", L"Cancel");

    DialogBoxIndirectW(GetModuleHandleW(nullptr), dt, g_hwnd, SettingsDlgProc);
}

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

    case WM_HOTKEY:
        if (wp == HOTKEY_SAVE_IR)     SpSaveIR();
        else if (wp == HOTKEY_RECORD) { if (g_recording) SpStopRecord(); else SpStartRecord(); }
        else if (wp == HOTKEY_IR_TOGGLE) { if (g_irRunning) SpStopIR(); else SpStartIR(); }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_SAVE_IR:    SpSaveIR(); break;
        case IDM_RECORD:     if (g_recording) SpStopRecord(); else SpStartRecord(); break;
        case IDM_IR_TOGGLE:  if (g_irRunning) SpStopIR(); else SpStartIR(); break;
        case IDM_SETTINGS:   ShowSettingsDialog(); break;
        case IDM_QUIT:       DestroyWindow(hwnd); break;
        }
        return 0;

    case WM_DESTROY:
        SpShutdown();
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
        return 0;
    }

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

    // Global hotkeys
    RegisterHotKey(g_hwnd, HOTKEY_SAVE_IR,   MOD_ALT | MOD_NOREPEAT, VK_F10);
    RegisterHotKey(g_hwnd, HOTKEY_RECORD,    MOD_ALT | MOD_NOREPEAT, VK_F9);
    RegisterHotKey(g_hwnd, HOTKEY_IR_TOGGLE, MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F10);

    // Init ShadowPlay API
    if (!SpInit()) {
        ShowBalloon(L"ShadowPlay", L"Failed to connect to ShadowPlay. Is the NVIDIA driver installed?", NIIF_ERROR);
    } else if (!SpCreateSession()) {
        ShowBalloon(L"ShadowPlay", L"Failed to create capture session. Is the ShadowPlay service running?", NIIF_ERROR);
    } else {
        SpStartIR();
    }

    UpdateTip();

    // Message loop — blocks on GetMessage, zero CPU when idle
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
    return 0;
}
