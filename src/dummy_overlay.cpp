// dummy_overlay.cpp — stub NVIDIA Overlay.exe
// The ShadowPlay server (nvspcaps64.dll) tries to start the overlay
// process during EnableShadowPlay. It just needs to launch successfully;
// the server doesn't communicate with it further for capture to work.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    Sleep(500);
    return 0;
}
