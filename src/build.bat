@echo off
setlocal
rem build.bat — compile probe.exe using the VS18 toolchain into ..\bin\
rem MSBuild isn't on PATH on this box; we shell out to vcvars64 to set up cl/link.

set "VS_ROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
set "VCVARS=%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [!] vcvars64.bat not found at "%VCVARS%"
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 (
    echo [!] vcvars64.bat failed
    exit /b 1
)

set "SRC=%~dp0"
set "OUT=%SRC%..\bin"
pushd "%OUT%" || exit /b 1

cl /nologo /std:c++17 /EHsc /W3 /O2 /DUNICODE /D_UNICODE ^
   "%SRC%probe.cpp" /Fe:probe.exe /Fo:probe.obj ^
   /link /SUBSYSTEM:CONSOLE shlwapi.lib

set RC=%ERRORLEVEL%
del /q probe.obj 2>nul
popd
exit /b %RC%
