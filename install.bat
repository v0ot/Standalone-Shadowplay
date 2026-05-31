@echo off
setlocal EnableDelayedExpansion
title Standalone ShadowPlay Installer
color 0A

echo.
echo   ==========================================
echo    Standalone ShadowPlay Installer
echo   ==========================================
echo.
echo   Instant replay ^& clipping without the
echo   NVIDIA App, overlay, or bloat.
echo.
echo   Requires: NVIDIA GPU + display driver
echo.

:: Handle /uninstall
if /i "%~1"=="/uninstall" goto :uninstall
if /i "%~1"=="--uninstall" goto :uninstall
goto :install

:uninstall
echo   Uninstalling Standalone ShadowPlay...
net session >nul 2>&1
if %errorlevel% neq 0 (echo   [!] Run as Administrator. & pause & exit /b 1)
taskkill /f /im ShadowPlay.exe 2>nul
taskkill /f /im nvsphelper64.exe 2>nul
taskkill /f /im "NVIDIA Overlay.exe" 2>nul
net stop NvContainerLocalSystem 2>nul
taskkill /f /im NvContainer.exe 2>nul
timeout /t 2 /nobreak >nul
sc delete NvContainerLocalSystem >nul 2>&1
rmdir /S /Q "C:\Program Files\NVIDIA Corporation\NvContainer" 2>nul
rmdir /S /Q "C:\Program Files\NVIDIA Corporation\NVIDIA App" 2>nul
rmdir /S /Q "%LOCALAPPDATA%\NVIDIA Corporation\NVIDIA App" 2>nul
reg delete "HKLM\SOFTWARE\NVIDIA Corporation\Global\NvApp" /f 2>nul
reg delete "HKLM\SOFTWARE\NVIDIA Corporation\Global\ShadowPlay" /f 2>nul
reg delete "HKLM\SOFTWARE\NVIDIA Corporation\NvContainer" /f 2>nul
reg delete "HKCU\SOFTWARE\NVIDIA Corporation\Global\ShadowPlay" /f 2>nul
del "C:\Windows\System32\nvspcap64.dll" 2>nul
del "C:\Windows\SysWOW64\nvspcap.dll" 2>nul
echo.
echo   Uninstall complete.
pause
exit /b 0

:install
:: Check admin
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo   [!] Run this as Administrator.
    echo       Right-click ^> Run as administrator
    pause
    exit /b 1
)

set "ROOT=%~dp0"
set "RT=%ROOT%runtime"
set "NVC=C:\Program Files\NVIDIA Corporation\NvContainer"
set "APP=C:\Program Files\NVIDIA Corporation\NVIDIA App"
set "SP=%APP%\ShadowPlay"
set "MB=%APP%\MessageBus"
set "CEF=%APP%\CEF"
set "PLUGINS=%NVC%\plugins"
set "SVC=NvContainerLocalSystem"

:: Verify runtime exists
if not exist "%RT%\ShadowPlay\nvspapi64.dll" (
    echo   [!] runtime\ folder not found. Re-clone the repo.
    pause
    exit /b 1
)

echo   [1/9] Stopping existing NVIDIA processes...
taskkill /f /im ShadowPlay.exe 2>nul
taskkill /f /im nvsphelper64.exe 2>nul
taskkill /f /im NvContainer.exe 2>nul
taskkill /f /im "NVIDIA Overlay.exe" 2>nul
net stop %SVC% 2>nul
timeout /t 2 /nobreak >nul

echo   [2/9] Deploying NvContainer...
if not exist "%NVC%" mkdir "%NVC%"
copy /Y "%RT%\NvContainer\NvContainer.exe" "%NVC%\NvContainer.exe" >nul
copy /Y "%RT%\NvContainer\NvPluginWatchdog.dll" "%NVC%\NvPluginWatchdog.dll" >nul

echo   [3/9] Deploying ShadowPlay runtime...
for %%D in ("%SP%" "%SP%\NVSPCAPS" "%SP%\Plugins\LocalSystem" "%MB%" "%CEF%") do (
    if not exist %%D mkdir %%D
)
for %%F in (nvspapi64.dll ipccommon64.dll nvsphelper64.exe nvsphelperplugin64.dll nvspscreenshot64.dll nvspcap64.dll nvspcap.dll capcore64.dll nvfcapi64.dll nvfp64.dll nvmf64.dll) do (
    copy /Y "%RT%\ShadowPlay\%%F" "%SP%\%%F" >nul
)
copy /Y "%RT%\ShadowPlay\_nvspcaps64.dll" "%SP%\NVSPCAPS\_nvspcaps64.dll" >nul
copy /Y "%RT%\ShadowPlay\_nvspserviceplugin64.dll" "%SP%\Plugins\LocalSystem\_nvspserviceplugin64.dll" >nul
for %%F in (MessageBus.dll NvMessageBus.dll NvMessageBusBroadcast.dll messagebus.conf) do (
    copy /Y "%RT%\MessageBus\%%F" "%MB%\%%F" >nul
)

:: Dummy overlay (satisfies server init check, exits in 30s)
if exist "%ROOT%bin\NVIDIA Overlay.exe" (
    copy /Y "%ROOT%bin\NVIDIA Overlay.exe" "%CEF%\NVIDIA Overlay.exe" >nul
) else (
    echo   [!] bin\NVIDIA Overlay.exe not found - build first: src\build.bat
)

echo   [4/9] Deploying D3D proxies...
if not exist "C:\Windows\System32\nvspcap64.dll" (
    copy /Y "%RT%\ShadowPlay\nvspcap64.dll" "C:\Windows\System32\nvspcap64.dll" >nul
    echo        nvspcap64.dll deployed to System32
) else (
    echo        nvspcap64.dll already in System32
)
if not exist "C:\Windows\SysWOW64\nvspcap.dll" (
    copy /Y "%RT%\ShadowPlay\nvspcap.dll" "C:\Windows\SysWOW64\nvspcap.dll" >nul
)

echo   [5/9] Installing Virtual Audio driver...
if exist "%RT%\NvVAD\nvvad.inf" (
    pnputil /add-driver "%RT%\NvVAD\nvvad.inf" /install >nul 2>&1
    echo        NvVAD driver installed
)

echo   [6/9] Creating plugin directories...
for %%D in (LocalSystem SPUser User Session AIUser) do (
    if not exist "%PLUGINS%\%%D" mkdir "%PLUGINS%\%%D"
)
:: Watchdog
if not exist "%NVC%\Watchdog" mkdir "%NVC%\Watchdog"
copy /Y "%NVC%\NvPluginWatchdog.dll" "%NVC%\Watchdog\NvPluginWatchdog.dll" >nul

:: Directory symlinks
for %%L in (
    "%PLUGINS%\LocalSystem\ShadowPlay"
    "%PLUGINS%\SPUser\nvspcaps"
    "%PLUGINS%\LocalSystem\Watchdog"
) do (
    if not exist %%L (
        if "%%~nxL"=="ShadowPlay" mklink /D %%L "%SP%\Plugins\LocalSystem" >nul 2>nul
        if "%%~nxL"=="nvspcaps" mklink /D %%L "%SP%\NVSPCAPS" >nul 2>nul
        if "%%~nxL"=="Watchdog" mklink /D %%L "%NVC%\Watchdog" >nul 2>nul
    )
)

:: Broadcaster files MUST be real copies (not symlinks — NvContainer crashes on file symlinks)
del "%PLUGINS%\LocalSystem\NvMessageBusBroadcast.dll" 2>nul
del "%PLUGINS%\LocalSystem\messagebus.conf" 2>nul
rmdir "%PLUGINS%\LocalSystem\NvMessageBusBroadcast.dll" 2>nul
rmdir "%PLUGINS%\LocalSystem\messagebus.conf" 2>nul
copy /Y "%MB%\NvMessageBusBroadcast.dll" "%PLUGINS%\LocalSystem\NvMessageBusBroadcast.dll" >nul
copy /Y "%MB%\messagebus.conf" "%PLUGINS%\LocalSystem\messagebus.conf" >nul

echo   [7/9] Deploying IPC to LocalAppData...
set "LOCALSP=%LOCALAPPDATA%\NVIDIA Corporation\NVIDIA App\ShadowPlay"
if not exist "%LOCALSP%" mkdir "%LOCALSP%"
copy /Y "%SP%\ipccommon64.dll" "%LOCALSP%\ipccommon64.dll" >nul
copy /Y "%MB%\messagebus.conf" "%LOCALSP%\messagebus.conf" >nul

echo   [8/9] Configuring registry...
:: ShadowPlay Cfg2 (XOR-validated magic) + Cfg3
reg add "HKLM\SOFTWARE\NVIDIA Corporation\Global\NvApp\ShadowPlay" /v ShadowPlayCfg2 /t REG_BINARY /d AB496FBD04905508145F519C9C2E31F8 /f >nul
reg add "HKLM\SOFTWARE\NVIDIA Corporation\Global\NvApp\ShadowPlay" /v ShadowPlayCfg3 /t REG_DWORD /d 3 /f >nul
:: FTS feature toggle
reg add "HKLM\SOFTWARE\NVIDIA Corporation\Global\NvApp\ShadowPlay\FTS" /v {11E72C12-FE4E-4BC5-AC47-7F4BD7C948D6} /t REG_DWORD /d 1 /f >nul
:: FullPath
reg add "HKLM\SOFTWARE\NVIDIA Corporation\Global\NvApp" /v FullPath /t REG_SZ /d "%SP%\nvsphelper64.exe" /f >nul
:: ShadowPlay enable flags
reg add "HKLM\SOFTWARE\NVIDIA Corporation\Global\ShadowPlay" /v EnableShadowPlay /t REG_DWORD /d 1 /f >nul
reg add "HKLM\SOFTWARE\NVIDIA Corporation\Global\ShadowPlay" /v ShadowPlayOnSystemStart /t REG_DWORD /d 1 /f >nul

:: NVSPCAPS user settings (REG_BINARY, 4-byte LE values)
set "K=HKCU\SOFTWARE\NVIDIA Corporation\Global\ShadowPlay\NVSPCAPS"
reg add "%K%" /v IsShadowPlayEnabled /t REG_BINARY /d 01000000 /f >nul
reg add "%K%" /v IsShadowPlayEnabledUser /t REG_BINARY /d 01000000 /f >nul
reg add "%K%" /v RecEnabled /t REG_BINARY /d 01000000 /f >nul
reg add "%K%" /v DwmEnabled /t REG_BINARY /d 01000000 /f >nul
reg add "%K%" /v DwmDvrEnabledV1 /t REG_BINARY /d 01000000 /f >nul
reg add "%K%" /v DwmEnabledUser /t REG_BINARY /d 01000000 /f >nul
reg add "%K%" /v EnableMicrophone /t REG_BINARY /d 01000000 /f >nul
reg add "%K%" /v VideoWidth /t REG_BINARY /d 80070000 /f >nul
reg add "%K%" /v VideoHeight /t REG_BINARY /d 38040000 /f >nul
reg add "%K%" /v DVRBufferLen /t REG_BINARY /d 78000000 /f >nul
reg add "%K%" /v RecordingFPS /t REG_BINARY /d 00007042 /f >nul
reg add "%K%" /v EncoderProfile /t REG_BINARY /d 02000000 /f >nul
reg add "%K%" /v AudioMode /t REG_BINARY /d 03000000 /f >nul
reg add "%K%" /v MicMode /t REG_BINARY /d 02000000 /f >nul
reg add "%K%" /v HLEnabled /t REG_BINARY /d 01000000 /f >nul
reg add "%K%" /v GameStreamPortal /t REG_BINARY /d 01000000 /f >nul

:: Watchdog entry (auto-launch SPUser container at login)
set "WD=HKLM\SOFTWARE\NVIDIA Corporation\NvContainer\Watchdog\SPUserX64"
reg add "%WD%" /ve /t REG_SZ /d "Temp - SP User Plugins (x64)" /f >nul
reg add "%WD%" /v Container /t REG_SZ /d "%NVC%\NvContainer.exe" /f >nul
reg add "%WD%" /v Folder /t REG_SZ /d "%PLUGINS%\SPUser" /f >nul
reg add "%WD%" /v Parameters /t REG_SZ /d "-f \"C:\ProgramData\NVIDIA Corporation\NVIDIA App\NvContainer\NvContainerUser%%dSPUser.log\" -d \"%PLUGINS%\SPUser\" -r -l 3 -p 30000" /f >nul
reg add "%WD%" /v Policy /t REG_SZ /d "10/300/5" /f >nul

:: Log directories
if not exist "C:\ProgramData\NVIDIA Corporation\NVIDIA App\NvContainer" mkdir "C:\ProgramData\NVIDIA Corporation\NVIDIA App\NvContainer"
if not exist "C:\ProgramData\NVIDIA Corporation\NVIDIA App\MessageBus" mkdir "C:\ProgramData\NVIDIA Corporation\NVIDIA App\MessageBus"

echo   [9/9] Registering + starting service...
sc delete %SVC% >nul 2>&1
timeout /t 1 /nobreak >nul
sc create %SVC% binPath= "\"%NVC%\NvContainer.exe\" -s %SVC% -a -f \"C:\ProgramData\NVIDIA Corporation\NVIDIA App\NvContainer\NvContainerLocalSystem.log\" -l 3 -d \"%PLUGINS%\LocalSystem\" -r -p 30000 -ert" start= auto DisplayName= "NVIDIA LocalSystem Container" >nul
sc description %SVC% "Standalone ShadowPlay capture service" >nul
sc start %SVC% >nul
timeout /t 5 /nobreak >nul

:: Verify
sc query %SVC% | findstr "RUNNING" >nul 2>&1
if %errorlevel% equ 0 (
    echo.
    echo   ==========================================
    echo    Installation complete!
    echo   ==========================================
    echo.
    echo   Run:  bin\ShadowPlay.exe
    echo.
    echo   Hotkeys:
    echo     Alt+F10        Save instant replay
    echo     Alt+F9         Toggle manual recording
    echo     Alt+Shift+F10  Toggle instant replay
    echo     Right-click tray icon for menu
    echo.
    echo   Uninstall:  install.bat /uninstall
    echo.
) else (
    echo.
    echo   [!] Service failed to start.
    echo   Check: C:\ProgramData\NVIDIA Corporation\NVIDIA App\NvContainer\NvContainerLocalSystem.log
    echo.
)
pause
exit /b 0
