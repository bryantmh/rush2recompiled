@echo off
rem Builds tmp\scenery_test\scenery_test.exe (tools\calspeed\scenery_test.cpp: California Speed's extended scenery
rem outside the game) and runs it with the arguments. Driven by tools\calspeed\scenery.py.
setlocal
set "HERE=%~dp0"
pushd "%HERE%..\.."
set "OUT=tmp\scenery_test"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
call tools\vsenv.bat clang-cl /nologo /O2 /std:c++20 /EHsc -march=nehalem -Wno-everything /DNOMINMAX ^
    /I include tools\calspeed\scenery_test.cpp src\calspeed\calspeed_scenery.cpp ^
    /Fo"%OUT%\obj\\" /Fe"%OUT%\scenery_test.exe"
if errorlevel 1 (
    popd
    exit /b 1
)
popd
if "%~1"=="" exit /b 0
"%HERE%..\..\%OUT%\scenery_test.exe" %*
exit /b %ERRORLEVEL%
