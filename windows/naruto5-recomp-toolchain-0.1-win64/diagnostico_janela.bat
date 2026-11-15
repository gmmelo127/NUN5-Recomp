@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "PY=%~dp0toolchain\python\python.exe"
if not exist "%PY%" set "PY=python"
if not exist "games\naruto" mkdir "games\naruto"
set "ISO="
if exist "iso_path.txt" set /p ISO=<"iso_path.txt"
if not exist "%ISO%" ( echo ISO ausente & pause & exit /b 1 )
set "ELF=games\naruto\disc\SLES_556.05"
set "NTSC="
if exist "games\naruto\patched\SLES_556.05" ( set "ELF=games\naruto\patched\SLES_556.05" & set "NTSC=--ntsc" )
set "PATCHES="
if exist "patches\widescreen_recomp.txt" set "PATCHES=--patches patches\widescreen_recomp.txt"
if exist "games\naruto\patches.txt" set "PATCHES=%PATCHES% --patches games\naruto\patches.txt"
set "PS2_PRESENT_LOG=%~dp0games\naruto\present.csv"
if exist "%PS2_PRESENT_LOG%" del "%PS2_PRESENT_LOG%"
set "PS2_PRESENT_SOFTWARE="
set "PS2_GPU_FALLBACK="
set "PS2_GPU_LOW_POWER="
set "PS2_SDL_RENDERER="
echo Rodando 600 frames em janela. A janela abre; aguarde fechar...
"build\native-naruto\ps2run.exe" "%ELF%" --disc "%ISO%" --window --widescreen %NTSC% %PATCHES% --memcard "games\naruto\memcard.ps2" --frames 600 %* > "games\naruto\last_run.log" 2>&1
"%PY%" "scripts\present_stats.py" "games\naruto\present.csv"
echo.
findstr /i /c:"[gpu]" /c:"[window]" /c:"software" "games\naruto\last_run.log"
echo.
echo Log: games\naruto\last_run.log
pause
