@echo off
rem Builds the standalone mover test (out\movers_test.exe) and runs it.
rem   tools\rush2049\cpp_test\movers_build.bat [seconds] [track]
rem From Git Bash: cmd //c "tools\rush2049\cpp_test\movers_build.bat"
setlocal
set "HERE=%~dp0"
pushd "%HERE%..\..\.."
if not exist "%HERE%out\movers" mkdir "%HERE%out\movers"
set "MINIZ=lib\N64ModernRuntime\thirdparty\miniz"

call tools\vsenv.bat clang-cl /nologo /O2 /std:c++20 /EHsc -march=nehalem -Wno-everything ^
    /I include /I "%HERE%shim" /I %MINIZ% /I lib\N64ModernRuntime\N64Recomp\include ^
    "%HERE%movers_test.cpp" "%HERE%inflate_shim.cpp" src\rush2049\track2049_movers_logic.cpp src\rush2049\rush2049_rom.cpp ^
    src\rush2049\wings_rom.cpp %MINIZ%\miniz.c %MINIZ%\miniz_tinfl.c %MINIZ%\miniz_tdef.c %MINIZ%\miniz_zip.c ^
    /Fo"%HERE%out\movers\\" /Fe"%HERE%out\movers_test.exe"
if errorlevel 1 (
    popd
    exit /b 1
)
"%HERE%out\movers_test.exe" %*
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
