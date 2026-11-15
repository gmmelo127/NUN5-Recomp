@echo off
setlocal EnableExtensions
cd /d "%~dp0"
if exist "game_settings.ini" (
    powershell -NoProfile -ExecutionPolicy Bypass -Command "(Get-Content 'game_settings.ini') -replace '^graphic_api = .*','graphic_api = Vulkan' | Set-Content 'game_settings.ini' -Encoding ASCII"
)
echo [launcher] backend forcado: Vulkan
call "%~dp0jogar.bat" %*
