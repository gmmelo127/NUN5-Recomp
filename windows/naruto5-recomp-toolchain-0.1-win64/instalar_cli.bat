@echo off
setlocal EnableExtensions
if "%~2"=="" (
    echo Uso: instalar_cli.bat "C:\caminho\jogo.iso" "C:\Games\Naruto5"
    exit /b 1
)
set "ISO=%~1"
set "DEST=%~2"
if not exist "%ISO%" ( echo ISO nao encontrada: %ISO% & exit /b 1 )
if /I not "%CD%"=="%DEST%" (
    if not exist "%DEST%" mkdir "%DEST%"
    robocopy "%CD%" "%DEST%" /E /XD "%CD%\games" "%CD%\generated" "%CD%\build" "%CD%\states" "%CD%\.git" /XF iso_path.txt *.log /NFL /NDL /NJH /NJS /NP | Out-Null
    if errorlevel 8 ( echo Falha ao copiar a ferramenta. & exit /b 1 )
)
> "%DEST%\iso_path.txt" echo %ISO%
pushd "%DEST%"
set "PY=toolchain\python\python.exe"
if not exist "%PY%" set "PY=python"
"%PY%" scripts\port_win.py naruto "%ISO%" --frames 12000 --rounds 4 --pnach "patches\PALtoNTSC_fixed.pnach" --pnach-section "NTSC Video Mode"
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
