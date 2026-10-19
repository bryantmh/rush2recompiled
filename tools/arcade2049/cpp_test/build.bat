@echo off
rem Builds tmp\arcade_cpp\arcade_test.exe (tools\arcade2049\cpp_test\main.cpp: the game's Rush 2049 arcade converters,
rem src\arcade2049, outside the game) and runs it. From Git Bash:
rem   cmd //c "tools\arcade2049\cpp_test\build.bat [OUT_DIR] [DATA_DIR]"
rem Check the track with: cmd //c "tools\track_mod_test.bat OUT_DIR\rush2049_arcade\tracks\alcatraz tmp\arcade\cpp_check"
setlocal
set "HERE=%~dp0"
pushd "%HERE%..\..\.."
set "OUT=tmp\arcade_cpp"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
set "MINIZ=lib\N64ModernRuntime\thirdparty\miniz"
set "SHIM=tools\rush2049\cpp_test"
set "CLIP=lib\Clipper2\CPP\Clipper2Lib"
call tools\vsenv.bat clang-cl /nologo /O2 /std:c++20 /EHsc -march=nehalem -Wno-everything /DNOMINMAX ^
    /I include /I "%SHIM%\shim" /I %MINIZ% /I lib\N64ModernRuntime\thirdparty /I lib\hqx /I lib\rt64\src ^
    /I lib\rt64\src\contrib /I "%CLIP%\include" /I lib\earcut\include ^
    tools\arcade2049\cpp_test\main.cpp src\arcade2049\arcade_data.cpp src\arcade2049\arcade_glb.cpp ^
    src\arcade2049\arcade_track.cpp src\arcade2049\arcade_cars.cpp src\texture_upscale_images.cpp ^
    "%CLIP%\src\clipper.engine.cpp" "%CLIP%\src\clipper.offset.cpp" "%CLIP%\src\clipper.rectclip.cpp" ^
    "%SHIM%\inflate_shim.cpp" lib\hqx\hq2x.c lib\hqx\hq4x.c ^
    %MINIZ%\miniz.c %MINIZ%\miniz_tinfl.c %MINIZ%\miniz_tdef.c %MINIZ%\miniz_zip.c ^
    /Fo"%OUT%\obj\\" /Fe"%OUT%\arcade_test.exe"
if errorlevel 1 (
    popd
    exit /b 1
)
"%OUT%\arcade_test.exe" %*
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
