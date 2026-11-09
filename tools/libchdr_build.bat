@echo off
rem Builds tmp\libchdr\chdr.lib (lib\libchdr: CHD disc images, src\chd_image.cpp) for the offline test builds
rem (tools\sfrush_ps1\cpp_test\build.bat, tools\rush2049\cpp_test\dc_build.bat, audio_build.bat). Its own LZMA and
rem zstd are in it; miniz is the caller's (they all link N64ModernRuntime's miniz, as the game does). The game itself
rem builds libchdr through CMake. Run from the repo root (the test builds call it): tools\libchdr_build.bat
setlocal
set "OUT=tmp\libchdr"
if exist "%OUT%\chdr.lib" exit /b 0
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
set "L=lib\libchdr"
call tools\vsenv.bat clang-cl /nologo /c /O2 -march=nehalem -Wno-everything ^
    /DWANT_RAW_DATA_SECTOR=1 /DWANT_SUBCODE=1 /DVERIFY_BLOCK_CRC=1 /DLOWRAM_TARGET=0 /DZSTD_DISABLE_ASM ^
    /I %L%\include /I %L%\deps\lzma-26.02\include ^
    %L%\src\libchdr_bitstream.c %L%\src\libchdr_cdrom.c %L%\src\libchdr_chd.c %L%\src\libchdr_codec_avhuff.c ^
    %L%\src\libchdr_codec_cdfl.c %L%\src\libchdr_codec_cdlz.c %L%\src\libchdr_codec_cdzl.c %L%\src\libchdr_codec_cdzs.c ^
    %L%\src\libchdr_codec_flac.c %L%\src\libchdr_codec_huff.c %L%\src\libchdr_codec_lzma.c %L%\src\libchdr_codec_zlib.c ^
    %L%\src\libchdr_codec_zstd.c %L%\src\libchdr_flac.c %L%\src\libchdr_huffman.c ^
    %L%\deps\lzma-26.02\src\LzmaDec.c %L%\deps\zstd-1.5.7\zstddeclib.c ^
    /Fo"%OUT%\obj\\"
if errorlevel 1 exit /b 1
call tools\vsenv.bat lib /nologo /out:"%OUT%\chdr.lib" "%OUT%\obj\*.obj"
exit /b %ERRORLEVEL%
