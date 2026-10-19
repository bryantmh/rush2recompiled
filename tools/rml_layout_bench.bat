@echo off
rem Builds tmp\rml_bench\rml_layout_bench.exe (tools\rml_layout_bench.cpp) against the game's RmlUi build in build\.
rem From Git Bash: cmd //c "tools\rml_layout_bench.bat"
setlocal
set "HERE=%~dp0"
pushd "%HERE%.."
set "OUT=tmp\rml_bench"
set "RML=lib\RecompFrontend\recompui\lib"
set "BLIB=build\lib\RecompFrontend\recompui\lib"
if not exist "%OUT%" mkdir "%OUT%"
call tools\vsenv.bat clang-cl /nologo /O2 /std:c++20 /EHsc /MD -Wno-everything /DNOMINMAX /DRMLUI_STATIC_LIB ^
    /I %RML%\RmlUi\Include tools\rml_layout_bench.cpp /Fo"%OUT%\\" /Fe"%OUT%\rml_layout_bench.exe" ^
    /link %BLIB%\RmlUi\Source\Core\rmlui.lib "%RML%\freetype-windows-binaries\release static\vs2015-2022\win64\freetype.lib" ^
    %BLIB%\lunasvg\lunasvg.lib %BLIB%\lunasvg\plutovg\plutovg.lib user32.lib
popd
