@echo off
rem Builds the standalone Rush 2049 audio test (out\audio_test.exe) and runs it.
rem   tools\rush2049\cpp_test\audio_build.bat <out_dir> --races
rem   tools\rush2049\cpp_test\audio_build.bat <out_dir> --song N [--seconds S] [--loop] [--sfx ID] [--rate R]
rem From Git Bash: cmd //c "tools\rush2049\cpp_test\audio_build.bat C:\temp\audio --races"
setlocal
set "HERE=%~dp0"
pushd "%HERE%..\..\.."
if not exist "%HERE%out\audio" mkdir "%HERE%out\audio"
set "MINIZ=lib\N64ModernRuntime\thirdparty\miniz"

call tools\vsenv.bat clang-cl /nologo /O2 /std:c++20 /EHsc -march=nehalem -Wno-everything ^
    /I include /I "%HERE%shim" /I %MINIZ% /I lib\N64ModernRuntime\N64Recomp\include ^
    "%HERE%audio_test.cpp" "%HERE%inflate_shim.cpp" src\rush2049\audio2049.cpp src\rush2049dc\audio2049_dc.cpp ^
    src\rush2049dc\rush2049_dc.cpp src\rush2049dc\rush2049_dc_convert.cpp src\rush2049dc\rush2049_dc_model.cpp src\rush2049dc\rush2049_dc_tables.cpp ^
    src\rush2049\rush2049_rom.cpp src\rush2049\wings_rom.cpp ^
    %MINIZ%\miniz.c %MINIZ%\miniz_tinfl.c %MINIZ%\miniz_tdef.c %MINIZ%\miniz_zip.c ^
    /Fo"%HERE%out\audio\\" /Fe"%HERE%out\audio_test.exe"
if errorlevel 1 (
    popd
    exit /b 1
)
if "%~1"=="" (
    popd
    exit /b 0
)
"%HERE%out\audio_test.exe" %*
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
