@echo off
rem One-click build of Rush2Recompiled.exe. Double-click or run from any shell.
rem Usage: tools\build.cmd [regen]
rem   regen  force N64Recomp to rerun (it already reruns by itself when us.toml or the syms file changed)
rem Runs tools\build.py, which shares one build lock with every Claude session and renames a running exe
rem instead of failing the link. Output is also written to tmp\build.log.
setlocal
cd /d "%~dp0.."
set "ARGS="
if /i "%~1"=="regen" set "ARGS=--regen"
python tools\build.py %ARGS%
echo.
echo Build finished. Log: tmp\build.log
pause
