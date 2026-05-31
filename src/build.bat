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

echo Building ShadowPlay.exe...
cl /nologo /std:c++17 /EHsc /W3 /O2 /DUNICODE /D_UNICODE ^
   "%SRC%shadowplay.cpp" /Fe:ShadowPlay.exe /Fo:shadowplay.obj ^
   /link /SUBSYSTEM:WINDOWS shell32.lib shlwapi.lib user32.lib advapi32.lib comctl32.lib gdi32.lib

set RC=%ERRORLEVEL%
del /q shadowplay.obj 2>nul
if %RC% neq 0 goto :done

echo Building probe.exe...
cl /nologo /std:c++17 /EHsc /W3 /O2 /DUNICODE /D_UNICODE ^
   "%SRC%probe.cpp" /Fe:probe.exe /Fo:probe.obj ^
   /link /SUBSYSTEM:CONSOLE shlwapi.lib
del /q probe.obj 2>nul

:done
popd
exit /b %RC%
