// dummy_overlay.cpp — stub NVIDIA Overlay replacement
// Creates the shared memory + window that the game-side proxy (nvspcap64.dll)
// needs to fully initialize game-specific capture sessions.
// Without this, games fall back to desktop capture and clips go to Desktop\ folder.
// With this, each game gets its own folder (Deadlock\, Overwatch\, etc.)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // Create the shared memory the game proxy checks for.
    // Name: {1ABAC973-1361-4C7B-B9CE-6A084DB70189}_v2
    // Size: 4096 bytes (just needs to exist and be mappable)
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE hMap = CreateFileMappingA(
        INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, 4096,
        "{1ABAC973-1361-4C7B-B9CE-6A084DB70189}_v2");
    if (hMap) MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, 4096);

    // Create the window the proxy looks for: "NVIDIA GeForce Overlay DT"
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"NVIDIA GeForce Overlay DT";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"NVIDIA GeForce Overlay",
        0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);

    // Stay alive while NvContainer needs us (message loop so the window is findable)
    MSG msg;
    SetTimer(hwnd, 1, 60000, nullptr); // exit after 60s
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER) break;
        DispatchMessageW(&msg);
    }

    return 0;
}
