@echo off
rem Builds the standalone track select art test (out\art_test.exe) and runs it: builds asset 3 with the 2049
rem dioramas and logos, checks it and writes PNG previews.
rem   tools\rush2049\cpp_test\art_build.bat [output dir]     (default tools\rush2049\cpp_test\out\art)
rem From Git Bash: cmd //c "tools\rush2049\cpp_test\art_build.bat"
setlocal
set "HERE=%~dp0"
pushd "%HERE%..\..\.."
set "REPO=%CD%"
if not exist "%HERE%out\art_obj" mkdir "%HERE%out\art_obj"
set "MINIZ=lib\N64ModernRuntime\thirdparty\miniz"

call tools\vsenv.bat clang-cl /nologo /O2 /std:c++20 /EHsc -march=nehalem -Wno-everything ^
    /I include /I "%HERE%shim" /I %MINIZ% /I lib\N64ModernRuntime\N64Recomp\include ^
    "%HERE%art_main.cpp" "%HERE%inflate_shim.cpp" src\rush2049_rom.cpp src\wings_rom.cpp ^
    %MINIZ%\miniz.c %MINIZ%\miniz_tinfl.c %MINIZ%\miniz_tdef.c %MINIZ%\miniz_zip.c ^
    /Fo"%HERE%out\art_obj\\" /Fe"%HERE%out\art_test.exe"
if errorlevel 1 (
    popd
    exit /b 1
)
"%HERE%out\art_test.exe" "%REPO%" %1
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
