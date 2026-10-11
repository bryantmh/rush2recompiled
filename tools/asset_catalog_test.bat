@echo off
rem Builds tmp\catalog_test\asset_catalog_test.exe (tools\asset_catalog_test.cpp). From Git Bash: cmd //c "tools\asset_catalog_test.bat"
rem Extra compiler flags go in CATALOG_DEFS, e.g. -DCATALOG_LOAD_FLAGS=0x14 walks the 2-player mirrored branches.
setlocal
set "HERE=%~dp0"
pushd "%HERE%.."
set "OUT=tmp\catalog_test"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
set "MINIZ=lib\N64ModernRuntime\thirdparty\miniz"
set "SHIM=tools\rush2049\cpp_test"
call tools\vsenv.bat clang-cl /nologo /O2 /std:c++20 /EHsc -march=nehalem -Wno-everything /DNOMINMAX %CATALOG_DEFS% ^
    /I include /I "%SHIM%\shim" /I %MINIZ% /I lib\N64ModernRuntime\N64Recomp\include /I lib\hqx ^
    /I lib\rt64\src /I lib\rt64\src\contrib ^
    tools\asset_catalog_test.cpp src\asset_catalog_walk.cpp src\texture_upscale_images.cpp ^
    "%SHIM%\inflate_shim.cpp" src\rush2049\track2049_convert.cpp src\rush2049\rush2049_rom.cpp src\rush2049\wings_rom.cpp ^
    lib\hqx\hq2x.c lib\hqx\hq4x.c ^
    %MINIZ%\miniz.c %MINIZ%\miniz_tinfl.c %MINIZ%\miniz_tdef.c %MINIZ%\miniz_zip.c ^
    /Fo"%OUT%\obj\\" /Fe"%OUT%\asset_catalog_test.exe"
popd
