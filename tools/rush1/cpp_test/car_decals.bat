@echo off
rem Builds the standalone Rush 1 car decal test (out\car_decals_test.exe) and runs it against the Python output.
rem From Git Bash: cmd //c "tools\rush1\cpp_test\car_decals.bat"
setlocal
set "HERE=%~dp0"
pushd "%HERE%..\..\.."
set "REPO=%CD%"
if not exist "%HERE%out" mkdir "%HERE%out"

call tools\vsenv.bat clang-cl /nologo /O2 /std:c++20 /EHsc -Wno-everything /I include ^
    "%HERE%car_decals_main.cpp" src\rush1\car1_decals.cpp /Fo"%HERE%out\\" /Fe"%HERE%out\car_decals_test.exe"
if errorlevel 1 (
    popd
    exit /b 1
)
python tools\rush1\cpp_test\make_car_ref.py || echo make_car_ref.py reported problems
"%HERE%out\car_decals_test.exe" "%REPO%"
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
