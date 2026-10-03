@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files\CMake\bin;C:\Program Files\LLVM\bin;%PATH%"
%*
