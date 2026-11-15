@echo off
setlocal EnableExtensions
cd /d "%~dp0"
echo ============================================================
echo  Otimizacao VU1
echo ============================================================
echo  Jogue normalmente as cenas que estao lentas (Master Mode,
echo  lutas). O launcher grava os microprogramas VU1. Quando voce
echo  fechar o jogo, eles serao compilados automaticamente.
echo  Na proxima vez que abrir, essas cenas usam o codigo compilado.
echo ============================================================
call "%~dp0jogar.bat" %*
echo.
echo Otimizacao concluida. Abra jogar.bat novamente para usar o VU1 compilado.
pause
