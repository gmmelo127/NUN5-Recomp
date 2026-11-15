@echo off
setlocal EnableExtensions
cd /d "%~dp0"
echo [launcher] Este pacote usa Vulkan (wgpu-native GNU).
echo [launcher] O backend DirectX 12 nao esta incluido nesta versao; nao e necessario.
echo [launcher] Iniciando com Vulkan...
call "%~dp0jogar.bat" %*
