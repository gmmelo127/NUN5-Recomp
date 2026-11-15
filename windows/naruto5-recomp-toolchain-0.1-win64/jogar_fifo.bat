@echo off
setlocal EnableExtensions
cd /d "%~dp0"
if exist "game_settings.ini" (
    powershell -NoProfile -ExecutionPolicy Bypass -Command "(Get-Content 'game_settings.ini') -replace '^vsync = .*','vsync = On' | Set-Content 'game_settings.ini' -Encoding ASCII"
)
echo [launcher] vsync = On (Fifo)
call "%~dp0jogar.bat" %*
