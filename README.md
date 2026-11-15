# NUN5-Recomp

Recompilação estática de **Naruto Shippuden: Ultimate Ninja 5** (PlayStation 2)
para PC. O código MIPS do jogo é traduzido para C++ e compilado nativamente; o
resto do console (VU1, GS, IOP, SPU2) é reimplementado no runtime. Nenhum código
do jogo é distribuído: cada usuário gera a build a partir da própria ISO.

- `windows/` — ferramenta de dev Windows (fonte + scripts + toolchain local)
- `linux/` — ferramenta de dev Linux (fonte + scripts)
- instalação e requisitos: `LEIA-ME.txt` dentro de cada pacote
- builds prontas: aba **Releases**

---

# dist — repositório x releases

Duas coisas apenas:

```text
dist/
├── windows/    FERRAMENTA DE DEV Windows  -> vai para o REPOSITÓRIO
├── linux/      FERRAMENTA DE DEV Linux    -> vai para o REPOSITÓRIO
├── releases/   BUILDS PRONTOS             -> vai para as RELEASES
└── nao-enviar/ builds locais com código do jogo (NUNCA publicar)
```

## O que vai para o REPOSITÓRIO

`windows/` e `linux/`. São a mesma ferramenta de dev, uma por sistema:

```text
runtime/     emulador PS2
recomp/      eerecomp + vurecomp
host/        main + janela
menu/        menu de configuração
scripts/     port_win.py / build_win.py  (Windows)
             port.sh (Linux)  +  vu1_manifest.txt
patches/     pnach e widescreen
CMakeLists.txt, docs, LICENSE
```

Cada uma tem o seu `.gitignore` que **exclui `toolchain/`**: o Zig (339 MB),
o Python (22 MB) e as bibliotecas grandes (`wgpu-native` no Linux) não cabem
no GitHub (limite de 100 MB por arquivo). Eles ficam só na sua máquina e vão
**embutidos nos instaladores das releases**.

Para usar a ferramenta de dev basta ter o `toolchain/` na pasta:

- Windows: `toolchain/` com Zig, Python, SDL2, wgpu (é o que o instalador traz).
- Linux: `toolchain/` com SDL2, wgpu e libstdc++ (ou rode `scripts/setup.sh`
  para baixar; o compilador é o `g++` do sistema).

## O que vai para as RELEASES

Somente `releases/` (3 arquivos, todos < 100 MB):

```text
naruto5-recomp-setup-0.1-win64.exe                 62 MB  instalador Windows (recomendado)
naruto5-recomp-toolchain-0.1-win64.7z              56 MB  Windows portátil (extrair e rodar)
naruto5-recomp-toolchain-0.1-linux-x86_64.tar.xz   12 MB  instalador Linux
```

- **Windows**: o `.exe` já traz Zig, Python, SDL2 e wgpu. O usuário escolhe a
  ISO na instalação e o jogo é gerado/compilado localmente. Não precisa
  instalar mais nada.
- **Linux**: o `.tar.xz` traz SDL2, wgpu e libstdc++. O `./instalar.sh` usa o
  compilador do sistema (`g++`/`clang++`) e `cmake`/`ninja`/`python3`, que já
  vêm em qualquer distro de desenvolvimento. O passo a passo completo, com os
  comandos por distro, está em `LEIA-ME.txt` dentro do pacote:
  `./instalar.sh "/caminho/Naruto5.iso"` e depois `./jogar.sh`.
- Nenhum release contém código do jogo: a build é sempre feita a partir da
  ISO do próprio usuário.

## Regra de ouro

1. Repositório = `windows/` + `linux/` (sem `toolchain/`). ~5 MB.
2. Releases = `releases/` (3 arquivos). Nenhum passa de 100 MB.
3. `nao-enviar/` nunca é publicado.

Atualizou o código? Edite **as duas** pastas (`windows/` e `linux/`) — ou
copie uma para a outra antes de mexer — e regenere os 3 arquivos de
`releases/`, atualizando `releases/SHA256SUMS`.
