@echo off
rem Builds tmp\track_mod_test\track_mod_test.exe (tools\track_mod_test.cpp: the custom track converter outside the game)
rem and runs it. From Git Bash: cmd //c "tools\track_mod_test.bat <track folder> <output folder>"
rem Check the output with: python tools\track_mod_check.py <output folder>
setlocal
set "HERE=%~dp0"
pushd "%HERE%.."
set "OUT=tmp\track_mod_test"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
set "MINIZ=lib\N64ModernRuntime\thirdparty\miniz"
set "SHIM=tools\rush2049\cpp_test"
call tools\vsenv.bat clang-cl /nologo /O2 /std:c++20 /EHsc -march=nehalem -Wno-everything /DNOMINMAX ^
    /I include /I "%SHIM%\shim" /I %MINIZ% /I lib\N64ModernRuntime\N64Recomp\include /I lib\hqx ^
    /I lib\N64ModernRuntime\N64Recomp\lib\tomlplusplus /I lib\rt64\src /I lib\rt64\src\contrib ^
    tools\track_mod_test.cpp src\track_mod\track_mod_convert.cpp src\gltf_scene.cpp src\texture_upscale_images.cpp ^
    "%SHIM%\inflate_shim.cpp" lib\hqx\hq2x.c lib\hqx\hq4x.c ^
    %MINIZ%\miniz.c %MINIZ%\miniz_tinfl.c %MINIZ%\miniz_tdef.c %MINIZ%\miniz_zip.c ^
    /Fo"%OUT%\obj\\" /Fe"%OUT%\track_mod_test.exe"
if errorlevel 1 (
    popd
    exit /b 1
)
popd
"%HERE%..\%OUT%\track_mod_test.exe" %*
exit /b %ERRORLEVEL%
