@echo off
rem Builds out\dc_test.exe (the Dreamcast source: disc import and file conversion). From Git Bash:
rem   cmd //c "tools\rush2049\cpp_test\dc_build.bat"
rem Run: out\dc_test.exe IMAGE tmp\dc_test.pak tmp\dc_out [indices]
rem      out\dc_test.exe track tmp\dc_test.pak rush2.us.recomp.z64 1 2 3   (track conversion over the disc)
rem      out\dc_test.exe audio tmp\dc_test.pak OUTDIR                      (the disc's sound as WAVs)
setlocal
set "HERE=%~dp0"
pushd "%HERE%..\..\.."
if not exist "%HERE%out\dc" mkdir "%HERE%out\dc"
set "MINIZ=lib\N64ModernRuntime\thirdparty\miniz"
call tools\vsenv.bat clang-cl /nologo /O2 /std:c++20 /EHsc -march=nehalem -Wno-everything ^
    /I include /I "%HERE%shim" /I %MINIZ% /I lib\N64ModernRuntime\N64Recomp\include ^
    "%HERE%dc_test.cpp" "%HERE%inflate_shim.cpp" src\rush2049_dc.cpp src\rush2049_dc_convert.cpp ^
    src\rush2049_dc_model.cpp src\rush2049_dc_tables.cpp src\rush2049_rom.cpp src\wings_rom.cpp ^
    src\track2049_convert.cpp src\audio2049.cpp src\audio2049_dc.cpp ^
    %MINIZ%\miniz.c %MINIZ%\miniz_tinfl.c %MINIZ%\miniz_tdef.c %MINIZ%\miniz_zip.c ^
    /Fo"%HERE%out\dc\\" /Fe"%HERE%out\dc_test.exe"
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
