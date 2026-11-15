# Windows x64

Pacote autocontido do Naruto 5 Recomp para Windows 10/11 x64.

```bat
instalar.bat
```

Abre uma janela Windows Forms para escolher a ISO e a pasta de instalação. O
instalador copia o toolchain, gera o código a partir da sua ISO, compila
`build\native-naruto\ps2run.exe` e cria os atalhos.

Não baixa nada: `toolchain\` já contém Zig 0.14.0, Python 3.12, SDL2/3,
wgpu-native, `eerecomp.exe` e `vurecomp.exe`.

```bat
jogar.bat
```

Usa `iso_path.txt`, o ELF patcheado em `games\naruto\patched\` e os patches de
widescreen. O VU1 é gravado durante o jogo e compilado ao sair
(`scripts\vu_refresh_win.py`).

Requisitos: Windows 10/11 x64, driver Vulkan e uma ISO legal do jogo.


## Desempenho

- `otimizar_vu.bat` grava/compila VU1 nas cenas lentas; a próxima sessão usa AOT.
- `jogar_dx12.bat` força DirectX 12 (a screenshot usa Vulkan); `jogar_vulkan.bat`
  volta para Vulkan.
- `diagnostico.bat` gera `games\naruto\diagnostico.log`; `jogar.bat` grava
  `games\naruto\launcher.log`.
