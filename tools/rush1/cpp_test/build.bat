@echo off
rem Builds the standalone Rush 1 track converter test (out\track1_test.exe) and runs it against the Python output.
rem   tools\rush1\cpp_test\build.bat           build, regenerate the Python references, run
rem   tools\rush1\cpp_test\build.bat noref     build and run with the existing references
rem From Git Bash: cmd //c "tools\rush1\cpp_test\build.bat"
setlocal
set "HERE=%~dp0"
pushd "%HERE%..\..\.."
set "REPO=%CD%"
if not exist "%HERE%out" mkdir "%HERE%out"

call tools\vsenv.bat clang-cl /nologo /O2 /std:c++20 /EHsc -Wno-everything /I include ^
    "%HERE%main.cpp" src\track1_convert.cpp /Fo"%HERE%out\\" /Fe"%HERE%out\track1_test.exe"
if errorlevel 1 (
    popd
    exit /b 1
)
if /i not "%1"=="noref" (
    python tools\rush1\cpp_test\make_ref.py || echo make_ref.py reported problems
)
"%HERE%out\track1_test.exe" "%REPO%"
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
