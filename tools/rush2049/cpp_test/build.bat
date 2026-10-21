@echo off
rem Builds the standalone track converter test (out\track2049_test.exe), converts every 2049 track and checks each
rem output with the Rush 2 format validators (tools\track_mod_check.py --stock).
rem From Git Bash: cmd //c "tools\rush2049\cpp_test\build.bat"
setlocal
set "HERE=%~dp0"
pushd "%HERE%..\..\.."
set "REPO=%CD%"
if not exist "%HERE%out" mkdir "%HERE%out"
set "MINIZ=lib\N64ModernRuntime\thirdparty\miniz"

call tools\vsenv.bat clang-cl /nologo /O2 /std:c++20 /EHsc -march=nehalem -Wno-everything ^
    /I include /I "%HERE%shim" /I %MINIZ% /I lib\N64ModernRuntime\N64Recomp\include ^
    "%HERE%main.cpp" "%HERE%inflate_shim.cpp" src\rush2049\track2049_convert.cpp src\rush2049\rush2049_rom.cpp src\rush2049\wings_rom.cpp ^
    %MINIZ%\miniz.c %MINIZ%\miniz_tinfl.c %MINIZ%\miniz_tdef.c %MINIZ%\miniz_zip.c ^
    /Fo"%HERE%out\\" /Fe"%HERE%out\track2049_test.exe"
if errorlevel 1 (
    popd
    exit /b 1
)

"%HERE%out\track2049_test.exe" "%REPO%"
set "RESULT=%ERRORLEVEL%"
for /d %%P in ("%HERE%out\static\*" "%HERE%out\nostatic\*") do (
    python -I tools\track_mod_check.py "%%P" --stock > "%%P\check.txt" 2>&1
    if errorlevel 1 (
        echo %%P: PROBLEMS, see %%P\check.txt
        set "RESULT=1"
    ) else echo %%P: ALL OK
)
popd
exit /b %RESULT%
