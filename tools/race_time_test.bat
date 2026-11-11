@echo off
rem Builds tmp\race_time_test\race_time_test.exe (tools\race_time_test.cpp: the race time model on converted path files,
rem dry and with a traction) and runs it. From Git Bash: cmd //c "tools\race_time_test.bat PATH.bin [...]"
setlocal
set "HERE=%~dp0"
pushd "%HERE%.."
set "OUT=tmp\race_time_test"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
call tools\vsenv.bat clang-cl /nologo /O2 /std:c++20 /EHsc -march=nehalem -Wno-everything /DNOMINMAX ^
    /I include /I tools\rush2049\cpp_test\shim /I lib\N64ModernRuntime\N64Recomp\include ^
    /I lib\N64ModernRuntime\librecomp\include /I lib\N64ModernRuntime\ultramodern\include /I lib\N64ModernRuntime\thirdparty ^
    tools\race_time_test.cpp src\race_time_model.cpp /Fo"%OUT%\obj\\" /Fe"%OUT%\race_time_test.exe"
if errorlevel 1 (
    popd
    exit /b 1
)
popd
"%HERE%..\%OUT%\race_time_test.exe" %*
