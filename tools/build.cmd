@echo off
rem One-click build of Rush2Recompiled.exe. Double-click or run from any shell.
rem Usage: tools\build.cmd [regen]
rem   regen  re-run N64Recomp on us.toml first (needed after us.toml hook changes)
rem Output is also written to tmp\build.log.
setlocal
cd /d "%~dp0.."
if not exist tmp mkdir tmp
set "PATH=C:\Program Files\CMake\bin;C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo vcvars64 failed & pause & exit /b 1)
if /i "%~1"=="regen" (
    .tools\N64Recomp.exe us.toml
    if errorlevel 1 (echo N64Recomp failed & pause & exit /b 1)
)
cmake --build build 2>&1 | powershell -NoProfile -Command "$input | Tee-Object -FilePath tmp\build.log"
echo.
echo Build finished. Log: tmp\build.log
pause
