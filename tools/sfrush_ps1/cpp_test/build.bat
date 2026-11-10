@echo off
rem Builds tmp\ps1_cpp\ps1_test.exe (tools\sfrush_ps1\cpp_test\main.cpp: the game's PS1 SF Rush converter,
rem src\sfrush_ps1, outside the game) and runs it. From Git Bash:
rem   cmd //c "tools\sfrush_ps1\cpp_test\build.bat [OUT_DIR] [DISC .cue or .bin]"
rem Check the track with: cmd //c "tools\track_mod_test.bat OUT_DIR\sfrush_ps1\tracks\chinatown tmp\ps1\check"
setlocal
set "HERE=%~dp0"
pushd "%HERE%..\..\.."
set "OUT=tmp\ps1_cpp"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
set "MINIZ=lib\N64ModernRuntime\thirdparty\miniz"
set "SHIM=tools\rush2049\cpp_test"
set "CLIP=lib\Clipper2\CPP\Clipper2Lib"
call tools\libchdr_build.bat
if errorlevel 1 exit /b 1
call tools\vsenv.bat clang-cl /I lib\libchdr\include /nologo /O2 /std:c++20 /EHsc -march=nehalem -Wno-everything /DNOMINMAX ^
    /I include /I "%SHIM%\shim" /I %MINIZ% /I lib\N64ModernRuntime\thirdparty /I lib\hqx /I lib\rt64\src ^
    /I lib\rt64\src\contrib /I "%CLIP%\include" /I lib\earcut\include ^
    tools\sfrush_ps1\cpp_test\main.cpp src\sfrush_ps1\ps1_disc.cpp src\chd_image.cpp src\sfrush_ps1\ps1_track.cpp src\sfrush_ps1\ps1_diorama.cpp ^
    src\sfrush_ps1\ps1_cars.cpp src\arcade2049\arcade_glb.cpp src\arcade2049\arcade_data.cpp src\arcade2049\arcade_track.cpp ^
    src\arcade2049\arcade_cars.cpp src\texture_upscale_images.cpp ^
    "%SHIM%\inflate_shim.cpp" "%CLIP%\src\clipper.engine.cpp" "%CLIP%\src\clipper.offset.cpp" "%CLIP%\src\clipper.rectclip.cpp" lib\hqx\hq2x.c lib\hqx\hq4x.c ^
    %MINIZ%\miniz.c %MINIZ%\miniz_tinfl.c %MINIZ%\miniz_tdef.c %MINIZ%\miniz_zip.c ^
    tmp\libchdr\chdr.lib /Fo"%OUT%\obj\\" /Fe"%OUT%\ps1_test.exe"
if errorlevel 1 (
    popd
    exit /b 1
)
"%OUT%\ps1_test.exe" %*
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
