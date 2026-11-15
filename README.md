# NUN5-Recomp

Recompilação estática de **Naruto Shippuden: Ultimate Ninja 5** (PlayStation 2)
para PC. O código MIPS do jogo é traduzido para C++ e compilado nativamente; o
resto do console (VU1, GS, IOP, SPU2) é reimplementado no runtime.

**Nenhum código do jogo é distribuído.** Cada usuário gera a build na própria
máquina a partir da sua ISO. O download traz só a ferramenta.

## Como funciona

- `eerecomp` traduz o executável MIPS (EE) para C++ unidade por unidade.
- `vurecomp` compila os microprogramas da VU1. Os 63 programas usados pelo jogo
  são recuperados **estaticamente** do ELF pelo manifesto
  `scripts/vu1_manifest.txt` (192 programas com os alvos de salto), então nada
  é interpretado em jogo.
- O runtime emula o resto do console: VU1, GS (wgpu/Vulkan), IOP, SPU2, IPU e
  memory card. A renderização é feita em compute shaders na GPU.
- A build é gerada e compilada na máquina do usuário; a ISO nunca é enviada
  para lugar nenhum.

## O que tem no repositório

```text
windows/   ferramenta de dev Windows (fonte + scripts + instalador.nsi)
linux/     ferramenta de dev Linux   (fonte + scripts)
```

As duas pastas contêm a **mesma fonte** (`runtime/`, `recomp/`, `host/`,
`menu/`, `scripts/`, `CMakeLists.txt`, patches). O que muda é só a camada do
sistema: `.bat`/`.ps1` no Windows e `.sh` no Linux.

O `toolchain/` (Zig, Python, SDL2, wgpu) **não** está no repositório por causa
do limite de 100 MB do GitHub. Ele vai embutido nos pacotes da aba
[**Releases**](https://github.com/gmmelo127/NUN5-Recomp/releases).

## Instalação

### Windows 10/11 x64

Baixe `naruto5-recomp-setup-0.1-win64.exe` na aba Releases e execute.

O wizard pede a pasta e a ISO, gera o C++, compila e cria os atalhos. O pacote
já inclui Zig, Python, SDL2 e wgpu: **não é preciso instalar mais nada**.
Também há o `naruto5-recomp-toolchain-0.1-win64.7z` (portátil, extrair e rodar
`instalar.bat`).

### Linux x86_64

Baixe `naruto5-recomp-toolchain-0.1-linux-x86_64.tar.xz` na aba Releases.

```bash
tar -xJf naruto5-recomp-toolchain-0.1-linux-x86_64.tar.xz
cd naruto5-recomp-toolchain-0.1-linux-x86_64
./instalar.sh "/caminho/para/Naruto5.iso"
./jogar.sh
```

Requisitos:

- driver **Vulkan** funcionando (Mesa/RADV, NVIDIA ou Intel);
- `g++` 13+ **ou** `clang++` 14+ (C++20 `<format>`);
- `cmake` 3.20+, `ninja` e `python3`;
- SDL2 já vem no pacote.

```bash
# Debian / Ubuntu / Mint
sudo apt install g++ cmake ninja-build python3 libvulkan1 mesa-vulkan-drivers

# Fedora
sudo dnf install gcc-c++ cmake ninja-build python3 vulkan-loader mesa-vulkan-drivers

# Arch / Manjaro
sudo pacman -S gcc cmake ninja python vulkan-radeon

# openSUSE
sudo zypper install gcc-c++ cmake ninja python3 libvulkan1 Mesa-libvulkan1
```

O `instalar.sh` confere tudo e diz o que falta antes de começar. O passo a
passo completo está em `LEIA-ME.txt` dentro do pacote.

### Compilar a ferramenta a partir do código

O pacote de release já traz o toolchain pronto. Para usar esta árvore
diretamente:

- **Windows:** copie `toolchain/` do pacote `.7z` para
  `windows/naruto5-recomp-toolchain-0.1-win64/` e rode `instalar.bat`.
- **Linux:** rode `linux/naruto5-recomp-toolchain-0.1-linux-x86_64/scripts/setup.sh`
  (baixa o wgpu-native e compila `eerecomp`/`vurecomp`) e depois
  `./instalar.sh`.

## Jogar

- **Windows:** atalho criado pelo instalador, ou `jogar.bat`.
- **Linux:** `./jogar.sh` (ou `./jogar.sh /caminho/para/outra.iso`).

O log de cada sessão fica em `games/naruto/last_run.log`; a linha `[perf]`
mostra o tempo de frame (`ms/frame`), o tempo de CPU (`cpu`), o tempo da EE
(`ee`) e se algum programa VU1 ficou fora da tabela AOT (`aot hits/misses`).

## Solução de problemas

- **Tela preta / "no Vulkan device"** — o driver Vulkan não está funcionando.
  Teste com `vulkaninfo`. Em notebook com GPU híbrida, force a dedicada com
  `DRI_PRIME=1 ./jogar.sh` (Linux) ou pelo painel da AMD/NVIDIA (Windows).
- **Erro de `<format>` ou g++ antigo** — instale GCC 13+ ou use
  `CXX=clang++ ./instalar.sh`.
- **`cmake`/`ninja` não encontrado** — instale os pacotes da seção Linux.
- **Lentidão** — veja `[perf]` no log. Se `aot` tiver misses, rode de novo (os
  programas que faltaram são compilados ao sair). Se `cpu` estiver baixo e
  `ms/frame` alto, o limite é a GPU: reduza `anti_aliasing` em
  `game_settings.ini`.
- **Remover a build** — `./desinstalar.sh` (Linux) ou o desinstalador
  (Windows). A ISO e os saves não são tocados.

## Legal

A ferramenta é MIT. O jogo, a ISO e qualquer código gerado a partir dela
pertencem aos seus donos. Este repositório não distribui o jogo nem material
derivado dele.
