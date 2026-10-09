@echo off
rem Builds out\car_dump.exe (a 2049 car converted to a Rush 2 car asset). From Git Bash: cmd //c "tools\rush2049\cpp_test\car_build.bat"
setlocal
set "HERE=%~dp0"
pushd "%HERE%..\..\.."
if not exist "%HERE%out\car" mkdir "%HERE%out\car"
set "MINIZ=lib\N64ModernRuntime\thirdparty\miniz"
call tools\vsenv.bat clang-cl /nologo /O2 /std:c++20 /EHsc -march=nehalem -Wno-everything ^
    /I include /I "%HERE%shim" /I %MINIZ% /I lib\N64ModernRuntime\N64Recomp\include ^
    "%HERE%car_dump.cpp" "%HERE%inflate_shim.cpp" src\rush2049\track2049_convert.cpp src\rush2049\rush2049_rom.cpp src\rush2049\wings_rom.cpp ^
    %MINIZ%\miniz.c %MINIZ%\miniz_tinfl.c %MINIZ%\miniz_tdef.c %MINIZ%\miniz_zip.c ^
    /Fo"%HERE%out\car\\" /Fe"%HERE%out\car_dump.exe"
popd
