@echo off
echo === Initializing ShadowPlay Settings ===

echo [1] HKLM Global ShadowPlay enable flags...
reg add "HKLM\SOFTWARE\NVIDIA Corporation\Global\ShadowPlay" /v EnableShadowPlay /t REG_DWORD /d 1 /f
reg add "HKLM\SOFTWARE\NVIDIA Corporation\Global\ShadowPlay" /v ShadowPlayOnSystemStart /t REG_DWORD /d 1 /f

echo [2] HKCU NVSPCAPS settings (same values NVIDIA overlay creates)...
set "K=HKCU\SOFTWARE\NVIDIA Corporation\Global\ShadowPlay\NVSPCAPS"
reg add "%K%" /f >nul

REM IsShadowPlayEnabled = 1 (REG_BINARY, 4 bytes LE)
reg add "%K%" /v IsShadowPlayEnabled /t REG_BINARY /d 01000000 /f
reg add "%K%" /v IsShadowPlayEnabledUser /t REG_BINARY /d 01000000 /f
reg add "%K%" /v RecEnabled /t REG_BINARY /d 01000000 /f
reg add "%K%" /v DwmEnabled /t REG_BINARY /d 01000000 /f
reg add "%K%" /v DwmDvrEnabledV1 /t REG_BINARY /d 01000000 /f
reg add "%K%" /v DwmEnabledUser /t REG_BINARY /d 01000000 /f
reg add "%K%" /v EnableMicrophone /t REG_BINARY /d 01000000 /f

REM VideoWidth=1920 (0x780), VideoHeight=1080 (0x438)
reg add "%K%" /v VideoWidth /t REG_BINARY /d 80070000 /f
reg add "%K%" /v VideoHeight /t REG_BINARY /d 38040000 /f

REM DVRBufferLen=120 seconds
reg add "%K%" /v DVRBufferLen /t REG_BINARY /d 78000000 /f

REM RecordingFPS=60.0 (float 0x42700000)
reg add "%K%" /v RecordingFPS /t REG_BINARY /d 00007042 /f

REM EncoderProfile=2 (High)
reg add "%K%" /v EncoderProfile /t REG_BINARY /d 02000000 /f

REM AudioMode=3, MicMode=2
reg add "%K%" /v AudioMode /t REG_BINARY /d 03000000 /f
reg add "%K%" /v MicMode /t REG_BINARY /d 02000000 /f

REM Default output path (Videos folder)
REM DefaultPathW and TempFilePath as UTF-16LE for C:\Users\admin\Videos
reg add "%K%" /v DefaultPathW /t REG_BINARY /d 43003a005c00550073006500720073005c00610064006d0069006e005c0056006900640065006f007300 /f
reg add "%K%" /v TempFilePath /t REG_BINARY /d 43003a005c00550073006500720073005c00610064006d0069006e005c004100700070004400610074006100 /f

echo [3] Restarting NvContainerLocalSystem...
net stop NvContainerLocalSystem 2>nul
timeout /t 3 /nobreak >nul
net start NvContainerLocalSystem
timeout /t 5 /nobreak >nul

echo.
sc query NvContainerLocalSystem | findstr STATE
echo.
echo Done. Now run ShadowPlay.exe
pause
