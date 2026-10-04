@echo off
rem Builds the standalone animated-texture test (out\texanim_test.exe) and runs it.
rem   tools\rush2049\cpp_test\texanim_build.bat [png dir] [seconds]
rem From Git Bash: cmd //c "tools\rush2049\cpp_test\texanim_build.bat"
setlocal
set "HERE=%~dp0"
pushd "%HERE%..\..\.."
if not exist "%HERE%out\texanim" mkdir "%HERE%out\texanim"
set "MINIZ=lib\N64ModernRuntime\thirdparty\miniz"

call tools\vsenv.bat clang-cl /nologo /O2 /std:c++20 /EHsc -march=nehalem -Wno-everything ^
    /I include /I "%HERE%shim" /I %MINIZ% /I lib\N64ModernRuntime\N64Recomp\include ^
    "%HERE%texanim_test.cpp" "%HERE%inflate_shim.cpp" src\track2049_texanim.cpp src\track2049_convert.cpp ^
    src\rush2049_rom.cpp src\wings_rom.cpp %MINIZ%\miniz.c %MINIZ%\miniz_tinfl.c %MINIZ%\miniz_tdef.c %MINIZ%\miniz_zip.c ^
    /Fo"%HERE%out\texanim\\" /Fe"%HERE%out\texanim_test.exe"
if errorlevel 1 (
    popd
    exit /b 1
)
"%HERE%out\texanim_test.exe" %*
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
