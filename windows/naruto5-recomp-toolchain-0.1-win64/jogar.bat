@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "PS2_NO_VU_AOT="
set "PS2_PRESENT_SOFTWARE="
set "PS2_GPU_FALLBACK="
set "PS2_GPU_LOW_POWER="
set "PS2_SDL_RENDERER="
set "PS2_PERF=1"
set "PS2_VU_STATS=1"
set "PS2_BENCH_TL=0,300"
set "ISO="
if exist "iso_path.txt" set /p ISO=<"iso_path.txt"
if not exist "%ISO%" (
    echo ISO nao encontrada. Rode instalar.bat e escolha a ISO.
    pause
    exit /b 1
)
if not exist "build\native-naruto\ps2run.exe" (
    echo Build ausente. Rode instalar.bat primeiro.
    pause
    exit /b 1
)

set "PY=%~dp0toolchain\python\python.exe"
if not exist "%PY%" set "PY=python"

set "VU=?"
if exist "generated\naruto\vu1_table.cpp" (
    for /f "usebackq delims=" %%a in (`"%PY%" "scripts\vu_count.py"`) do set "VU=%%a"
)
echo [launcher] VU1 compilado: %VU% programas
if not exist "games\naruto" mkdir "games\naruto"
if not exist "games\naruto\vu1" mkdir "games\naruto\vu1"
>>"games\naruto\launcher.log" echo === %DATE% %TIME% ===
>>"games\naruto\launcher.log" echo args: %*
>>"games\naruto\launcher.log" echo vu1: %VU%
>>"games\naruto\launcher.log" echo PS2_NO_VU_AOT: %PS2_NO_VU_AOT%
if exist "game_settings.ini" >>"games\naruto\launcher.log" findstr /b /c:"resolution =" /c:"anti_aliasing =" /c:"fps =" "game_settings.ini"

if exist "scripts\vu_refresh_win.py" "%PY%" "scripts\vu_refresh_win.py"

set "ELF=games\naruto\disc\SLES_556.05"
set "NTSC="
if exist "games\naruto\patched\SLES_556.05" (
    set "ELF=games\naruto\patched\SLES_556.05"
    set "NTSC=--ntsc"
)
set "PATCHES="
if exist "patches\widescreen_recomp.txt" set "PATCHES=--patches patches\widescreen_recomp.txt"
if exist "games\naruto\patches.txt" set "PATCHES=%PATCHES% --patches games\naruto\patches.txt"

set "PS2_VU_RECORD=%~dp0games\naruto\vu1"
"build\native-naruto\ps2run.exe" "%ELF%" --disc "%ISO%" --window --widescreen %NTSC% %PATCHES% --memcard "games\naruto\memcard.ps2" %* > "games\naruto\last_run.log" 2>&1
set "RC=%ERRORLEVEL%"
findstr /i /c:"[gs]" /c:"[window]" /c:"[gpu]" /c:"software" "games\naruto\last_run.log"
findstr /i /c:"software renderer" /c:"WebGPU unavailable" "games\naruto\last_run.log" >nul
if not errorlevel 1 echo [launcher] AVISO: WebGPU indisponivel, caiu no renderizador software. Veja games\naruto\last_run.log
if exist "scripts\vu_refresh_win.py" "%PY%" "scripts\vu_refresh_win.py"
exit /b %RC%
