@echo off
rem Builds tmp\billboards_test\billboards_test.exe (tools\calspeed\billboards_test.cpp: California Speed's billboard
rem trees outside the game) and runs it with the arguments. Driven by tools\calspeed\billboards.py.
setlocal
set "HERE=%~dp0"
pushd "%HERE%..\.."
set "OUT=tmp\billboards_test"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
call tools\vsenv.bat clang-cl /nologo /O2 /std:c++20 /EHsc -march=nehalem -Wno-everything /DNOMINMAX ^
    /I include tools\calspeed\billboards_test.cpp src\calspeed\calspeed_billboards.cpp ^
    /Fo"%OUT%\obj\\" /Fe"%OUT%\billboards_test.exe"
if errorlevel 1 (
    popd
    exit /b 1
)
popd
if "%~1"=="" exit /b 0
"%HERE%..\..\%OUT%\billboards_test.exe" %*
exit /b %ERRORLEVEL%
