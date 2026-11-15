@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "PY=%~dp0toolchain\python\python.exe"
if not exist "%PY%" set "PY=python"
set "LOG=games\naruto\diagnostico.log"
> "%LOG%" echo === Naruto 5 Recomp - diagnostico ===
>> "%LOG%" echo VU1 arquivos:
dir /b games\naruto\vu1\*.bin 2>nul | find /c /v "" >> "%LOG%"
>> "%LOG%" echo VU1 compilados:
findstr "register_programs" generated\naruto\vu1_table.cpp 2>nul >> "%LOG%"
>> "%LOG%" echo.
set "ISO="
if exist "iso_path.txt" set /p ISO=<"iso_path.txt"
if not exist "%ISO%" ( echo ISO ausente & pause & exit /b 1 )
set "ELF=games\naruto\disc\SLES_556.05"
set "NTSC="
if exist "games\naruto\patched\SLES_556.05" ( set "ELF=games\naruto\patched\SLES_556.05" & set "NTSC=--ntsc" )
set "PATCHES="
if exist "patches\widescreen_recomp.txt" set "PATCHES=--patches patches\widescreen_recomp.txt"
if exist "games\naruto\patches.txt" set "PATCHES=%PATCHES% --patches games\naruto\patches.txt"
echo Rodando 300 frames...
set "PS2_BENCH_TL=0,300"
"build\native-naruto\ps2run.exe" "%ELF%" --disc "%ISO%" --memcard "games\naruto\memcard.ps2" --frames 300 %NTSC% %PATCHES% >> "%LOG%" 2>&1
findstr /i /c:"[gpu]" /c:"[window]" /c:"missing" /c:"halted" /c:"stopped" /c:"[vu]" /c:"[tl]" /c:"no translated" /c:"missing code" "%LOG%"
echo.
echo Log completo: games\naruto\diagnostico.log
echo Launcher log: games\naruto\launcher.log
pause
