// dummy_overlay.cpp — stub NVIDIA Overlay.exe
// The ShadowPlay server (nvspcaps64.dll) tries to start the overlay
// process during EnableShadowPlay. It just needs to launch successfully;
// the server doesn't communicate with it further for capture to work.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    // Stay alive long enough for the ShadowPlay server to finish init.
    // The server starts nvosc, checks it's alive, then completes EnableShadowPlay.
    // After that we're no longer needed. 30 seconds is plenty.
    Sleep(30000);
    return 0;
}
