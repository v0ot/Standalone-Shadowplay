@echo off
echo === Standalone ShadowPlay Fix ===

echo [1] Setting ShadowPlay enable registry keys...
reg add "HKLM\SOFTWARE\NVIDIA Corporation\Global\ShadowPlay" /v ShadowPlayOnSystemStart /t REG_DWORD /d 1 /f
reg add "HKLM\SOFTWARE\NVIDIA Corporation\Global\ShadowPlay" /v EnableShadowPlay /t REG_DWORD /d 1 /f

echo [2] Restarting NvContainerLocalSystem service...
net stop NvContainerLocalSystem 2>nul
timeout /t 3 /nobreak >nul
net start NvContainerLocalSystem
timeout /t 5 /nobreak >nul

echo [3] Checking service + containers...
sc query NvContainerLocalSystem | findstr STATE
echo.
tasklist /fi "imagename eq NvContainer.exe" /fo list | findstr PID

echo.
echo Done. Now run ShadowPlay.exe
pause
