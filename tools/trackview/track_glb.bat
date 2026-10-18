@echo off
rem Builds tmp\track_glb\track_glb.exe (tools\trackview\track_glb.cpp: a track as a placed .glb outside the game) and runs
rem it with the arguments. From Git Bash: cmd //c "tools\trackview\track_glb.bat GEOMETRY PLACEMENT OUT.glb [TITLE]"
setlocal
set "HERE=%~dp0"
pushd "%HERE%..\.."
set "OUT=tmp\track_glb"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
set "MINIZ=lib\N64ModernRuntime\thirdparty\miniz"
set "SHIM=tools\rush2049\cpp_test"
call tools\vsenv.bat clang-cl /nologo /O2 /std:c++20 /EHsc -march=nehalem -Wno-everything /DNOMINMAX ^
    /I include /I "%SHIM%\shim" /I %MINIZ% /I lib\N64ModernRuntime\N64Recomp\include /I lib\hqx ^
    /I lib\rt64\src /I lib\rt64\src\contrib ^
    tools\trackview\track_glb.cpp src\asset_catalog_walk.cpp src\texture_upscale_images.cpp ^
    "%SHIM%\inflate_shim.cpp" lib\hqx\hq2x.c lib\hqx\hq4x.c ^
    %MINIZ%\miniz.c %MINIZ%\miniz_tinfl.c %MINIZ%\miniz_tdef.c %MINIZ%\miniz_zip.c ^
    /Fo"%OUT%\obj\\" /Fe"%OUT%\track_glb.exe"
if errorlevel 1 (
    popd
    exit /b 1
)
popd
"%HERE%..\..\%OUT%\track_glb.exe" %*
exit /b %ERRORLEVEL%
