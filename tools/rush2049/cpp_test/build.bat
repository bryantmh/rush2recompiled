@echo off
rem Builds the standalone track converter test (out\track2049_test.exe) and runs it against the Python output.
rem   tools\rush2049\cpp_test\build.bat           build, regenerate the Python references, run
rem   tools\rush2049\cpp_test\build.bat noref     build and run with the existing references
rem From Git Bash: cmd //c "tools\rush2049\cpp_test\build.bat"
setlocal
set "HERE=%~dp0"
pushd "%HERE%..\..\.."
set "REPO=%CD%"
if not exist "%HERE%out" mkdir "%HERE%out"
set "MINIZ=lib\N64ModernRuntime\thirdparty\miniz"

call tools\vsenv.bat clang-cl /nologo /O2 /std:c++20 /EHsc -march=nehalem -Wno-everything ^
    /I include /I "%HERE%shim" /I %MINIZ% /I lib\N64ModernRuntime\N64Recomp\include ^
    "%HERE%main.cpp" "%HERE%inflate_shim.cpp" src\track2049_convert.cpp src\rush2049_rom.cpp src\wings_rom.cpp ^
    %MINIZ%\miniz.c %MINIZ%\miniz_tinfl.c %MINIZ%\miniz_tdef.c %MINIZ%\miniz_zip.c ^
    /Fo"%HERE%out\\" /Fe"%HERE%out\track2049_test.exe"
if errorlevel 1 (
    popd
    exit /b 1
)

if /i not "%1"=="noref" (
    pushd tools\rush2049
    python track.py >nul || echo track.py reported problems
    popd
    python tools\rush2049\cpp_test\make_ref.py >nul || echo make_ref.py reported problems
)
"%HERE%out\track2049_test.exe" "%REPO%"
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
