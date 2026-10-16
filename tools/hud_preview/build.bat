@echo off
rem Builds tmp\hud_preview\hud_catalog_gen.exe (tools\hud_preview\gen_catalog.cpp: writes src\hud_catalog_rom.inc from
rem the ROM), generates src\hud_catalog_rom.inc if it is missing, then builds tmp\hud_preview\hud_preview.exe (the
rem previewer, with the game's src\hud_layout.cpp and src\hud_catalog.cpp). Both read the ROM's images, so they link
rem the asset catalog reader (as tools\asset_catalog_test.bat does).
rem From Git Bash: cmd //c "tools\hud_preview\build.bat"
setlocal
set "HERE=%~dp0"
pushd "%HERE%..\.."
set "OUT=tmp\hud_preview"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
set "MINIZ=lib\N64ModernRuntime\thirdparty\miniz"
set "SHIM=tools\rush2049\cpp_test"
set FLAGS=/nologo /O2 /std:c++20 /EHsc -march=nehalem -Wno-everything /DNOMINMAX ^
    /I include /I tools\hud_preview /I "%SHIM%\shim" /I %MINIZ% /I lib\N64ModernRuntime\N64Recomp\include /I lib\hqx ^
    /I lib\rt64\src /I lib\rt64\src\contrib
set DEPS=tools\hud_preview\hud_rom.cpp src\asset_catalog_walk.cpp src\texture_upscale_images.cpp ^
    "%SHIM%\inflate_shim.cpp" src\rush2049\track2049_convert.cpp src\rush2049\rush2049_rom.cpp src\rush2049\wings_rom.cpp ^
    lib\hqx\hq2x.c lib\hqx\hq4x.c %MINIZ%\miniz.c %MINIZ%\miniz_tinfl.c %MINIZ%\miniz_tdef.c %MINIZ%\miniz_zip.c
call tools\vsenv.bat clang-cl %FLAGS% tools\hud_preview\gen_catalog.cpp %DEPS% /Fo"%OUT%\obj\\" /Fe"%OUT%\hud_catalog_gen.exe"
if errorlevel 1 goto done
if not exist src\hud_catalog_rom.inc "%OUT%\hud_catalog_gen.exe" rush2.us.recomp.z64 src\hud_catalog_rom.inc
if errorlevel 1 goto done
call tools\vsenv.bat clang-cl %FLAGS% tools\hud_preview\hud_preview.cpp src\hud_layout.cpp src\hud_catalog.cpp %DEPS% ^
    /Fo"%OUT%\obj\\" /Fe"%OUT%\hud_preview.exe"
:done
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
