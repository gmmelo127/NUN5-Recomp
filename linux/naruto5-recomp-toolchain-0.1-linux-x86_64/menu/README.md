# Practice Settings → menu de configurações do recomp

Este diretório guarda o menu **Practice Settings** do jogo (Master Mode →
Practice → VS → Square), capturado e transformado no menu nativo de
configurações do recomp.

## Como foi capturado

Estado `master_mode.ps2` (frame 15500), depois:

1. `START` (abre o menu do Master Mode)
2. `Left` (vai para **Quit**), `Cross`, `Up` (Yes), `Cross` → Mode Select
3. `Down` 2× → **Practice**, `Cross`
4. Character Select: `Cross` (personagem), `Cross` (suporte), `Cross`
5. Linked Mode: `Square` para **Auto**
6. `Cross` (oponente/personagem), `Cross` (suporte), `Cross` (stage)
7. Na tela **VS / Practice Settings**, `Square` abre o menu

A sequência exata está em `capture.sh` (headless, com `--press`).

## Arquivos

| arquivo | conteúdo |
|---|---|
| `practice_settings_full.png` | frame original 18400 (1024×1024) |
| `practice_settings_panel.png` | recorte do painel original |
| `practice_settings_player.png` | Player Settings original |
| `practice_settings_opponent.png` | Opponent Settings original (scroll) |
| `vs_practice_settings_screen.png` | tela VS com o prompt Practice Settings |
| `free_battle_settings.png` | Battle Settings do Free Battle (referência) |
| `008F4000_8C45F35E.bin` | overlay original do menu (base 0x8F4000, 0x38000 bytes) |
| `recomp_code/` | units C++ geradas do overlay (0x8F4000/0x8F5000) |
| `overlay_strings.txt` | todas as strings ASCII do overlay, com offsets |
| `settings_strings.txt` | strings do menu/descrições originais |
| `practice_settings.json` | **menu editável**: título, seção Display, itens, opções e descrições |
| `generate_header.py` | gera `runtime/src/menu/practice_settings.hpp` a partir do JSON |
| `generate_font.py` | gera `runtime/src/menu/menu_font.hpp` (ASCII + Latin-1) |
| `capture.sh` | reproduz a sequência de captura original |
| `states/mode_select.ps2` | estado já no Mode Select (teste do menu) |
| `recomp_menu.png` | menu novo aplicado (1600×900) |
| `recomp_menu_unsaved.png` | menu com **NÃO SALVO** e linha modificada |
| `recomp_menu_hint.png` | prompt **OPÇÕES [□]** no pause |
| `recomp_menu_21x9.png` | menu do recomp aberto em 21:9 |
| `recomp_menu_14items.png` | menu de 14 itens (1920×1080) |
| `recomp_menu_pause_21x9.png` | menu aberto no pause em 21:9 (sem 4:3) |
| `recomp_menu_mode_select_hint.png` | prompt OPÇÕES [□] no Mode Select (topo direito) |
| `recomp_menu_pause_hint.png` | prompt OPÇÕES [□] no pause (inferior esquerdo) |
| `texture_filter_compare.png` | Auto × Nearest (720p) |
| `hud_aspect_compare.png` | HUD Aspect Auto × Off (720p) |
| `hud_aspect_16x9_compare.png` | HUD 16:9 × Off (moldura/retrato/minimapa) |
| `anti_aliasing_compare.png` | Off × FXAA × FXAA HQ × FXAA Ultra |
| `object_walk_hud_fix.png` | caminhada 21:9 com HUD corrigido, sem triângulo preto |

## Onde está o código/texto no jogo

- O menu de Practice vive no overlay de código **0x8F4000** (hash `8C45F35E`),
  carregado em RAM pelo jogo. O arquivo `008F4000_8C45F35E.bin` é a imagem usada
  pelo recompilador; as units `m_008F4000_..._unit_0000/0001.cpp` são o código
  recompilado (2 páginas de 4 KB).
- Os textos/descrições originais são ASCII dentro do overlay:
  - labels/descrições de Health/Chakra/Linked/Ultimate/Items/Commands etc.:
    offsets `0xEF00–0xF700` e `0x10C00–0x10F00` (`settings_strings.txt`);
  - a tabela completa de strings do overlay está em `overlay_strings.txt`.
- Alguns **labels visuais** originais são texturas; as descrições/valores são
  strings ASCII. O menu novo não usa essas texturas: ele desenha o painel e usa
  uma fonte bitmap própria.
- O ponteiro do menu/tabela de itens fica no overlay em `0x8F4A00/0x8F4BD0`
  (pointers para `0x8F48B0`, `0x8F48C0`, …), visíveis no `checks` do
  `generated/naruto/table.cpp`.

## Editar o menu

O **JSON** é a fonte de verdade:

```bash
# edite menu/practice_settings.json
python3 menu/generate_header.py
cmake --build build/native-naruto -j8
```

Cada seção tem `id`, `title` e `items`. Cada item tem `id`, `label`, `options`,
`value` e `description`. O título/subtítulo/rodapé também ficam no JSON
(`title`, `subtitle`, `footer`). O header gerado é
`runtime/src/menu/practice_settings.hpp`, com `Section{id, title, items}` e
`Item{id, label, options, value, description}`.

Hoje há **uma única seção**, `Display Settings`, com 14 itens:

| item | opções |
|---|---|
| Resolution | 960x720 … 3840x2160 (tamanho da janela, teto da resolução interna) |
| Screen Mode | Windowed, Fullscreen, Borderless |
| Aspect Ratio | 4:3, 16:9, 21:9, 32:9, Fit |
| FPS | 30, 60, 120 |
| Anti-Aliasing | Off, FXAA, FXAA HQ, FXAA Ultra |
| Graphic API | Auto, Vulkan, DirectX 12 (**só Windows**) |
| Texture Filtering | Auto, Nearest, Bilinear |
| Anisotropic Filtering | Off, 2x, 4x, 8x, 16x (**reservado**) |
| Draw Distance | Default, Far, Very Far (**reservado**) |
| MTVU | On, Off |
| HUD Aspect Ratio | Auto, 4:3, 16:9, 21:9, 32:9, Off |
| HUD Scale | Auto, 0.57, 0.67, 0.75, 1.00 |
| V-Sync | Off, On |
| Exit Game | Press X (pede confirmação antes de fechar) |

`FPS` é o **limite de apresentação**, não a taxa da lógica. A lógica continua
nativa (60 Hz NTSC / 50 Hz PAL) para o jogo não ficar lento; o acumulador de
presents mostra 30/60/120 imagens por segundo. Com **V-Sync On** o limite é a
taxa do monitor; com **V-Sync Off** pode passar dela (ex.: 120 em um monitor de
60 Hz). O título mostra `FPS Render`, `FPS Logica`, `Frame Time` e `Hz`.
Medido: 30 → `29,9 render / 59,8 logica / 33,4 ms`; 120 + V-Sync Off →
`121,2 / 60,6 / 8,3 ms` em um monitor de 60 Hz.

`Anti-Aliasing` aplica FXAA no passe de apresentação (o overlay do menu é
composto depois, então o texto continua nítido): `FXAA` = 4 taps, `FXAA HQ` =
8 taps (diagonais), `FXAA Ultra` = 12 taps (kernel mais largo). `Texture
Filtering` em `Auto` segue o bit TEX1 do jogo; `Nearest`/`Bilinear` forçam o
filtro.
`Anisotropic Filtering` e `Draw Distance` ficam salvos no ini mas **não têm
efeito** neste jogo: o GS do PS2 não tem filtro anisotrópico e o cenário é
streamado pelo jogo, sem uma variável de draw distance exposta.
`Graphic API`/`MTVU` são aplicados no próximo start. `Software` foi removido
porque desligava o overlay WebGPU (o menu ficava invisível); no Linux ficam
`Auto`/`Vulkan`, no Windows também `DirectX 12`. `HUD Aspect Ratio` corrige a
moldura/retrato/barra de vida e o minimapa do Master Mode; `Auto` segue o
aspecto da apresentação. Quando `HUD Aspect Ratio` está em um valor específico,
ele **vence** o `HUD Scale` manual (um `0.57` salvo não faz mais todas as
opções parecerem iguais). Só texturas de HUD são tocadas (nunca
15616/15872/16224/15360/15424, que também aparecem no cenário 3D): isso elimina
o triângulo preto/objetos deslocados ao caminhar.

A fonte do menu é gerada por `menu/generate_font.py` e cobre ASCII 32–126 mais
Latin-1 (160–255), então acentos como **Ç**, **Õ** e **Ã** funcionam. Rode o
script se trocar a fonte/tamanho.

## Renderização (WebGPU)

O menu é desenhado **dentro da janela do jogo**, no passe de apresentação:

- `runtime/src/menu/menu_overlay.cpp` desenha um canvas fixo de **1920×1080**
  (painel com cantos arredondados, laranja claro, fonte bitmap).
- `runtime/src/gs/gpu/gpu.cpp` gera uma textura RGBA8 **1920×1080** e faz o
  upload só quando o menu muda (menu aberto) ou entra no Master Mode (hint).
- `runtime/src/gs/gpu/blit.wgsl` encaixa a textura na viewport preservando 16:9,
  centralizada: **não há distorção** em 4:3/16:9/21:9/32:9.
- O shader escurece o jogo atrás do menu; o hint do Master Mode não escurece.
- Não há passe de escala na CPU. Em 720p o render do menu fica em **~1,6–3 ms**
  (antes eram ~30 ms com a escala na CPU); o upload leva ~0,8 ms.
- A escala interna é **`min(janela, Resolution)`**: uma janela de 720p fica em
  `1024x1024 / 2x` mesmo que a Resolution salva seja 2560x1440; maximizar até
  1920x1080 sobe para `1536x1536 / 3x`, mas nunca passa da Resolution. O título
  mostra `Resolucao janela`, `Resolucao configurada`, `Resolucao interna` e
  `Supersampling` (assim dá para ver o custo real).
- `--window-size 1280x720` permite abrir uma janela menor sem mudar a
  Resolution (supersampling), útil para testar o custo real.
- Mudar **Resolution** aplica a nova escala interna **na hora** (com
  preservação da VRAM/texturas via readback), sem precisar reiniciar.
- **Telas 2D nativas 4:3**: title/loading, Mode Select e character
  select/VS/Practice são apresentados com **pillarbox 4:3** em vez de esticados.
  O **pause do Master Mode usa o aspecto escolhido** (16:9/21:9/32:9), assim
  como o battle/mundo 3D e o FOV; o HUD pequeno tem correção dinâmica. Quando o
  menu do recomp está aberto, a viewport usa o aspecto escolhido para o menu
  ocupar a janela.

### Frame time medido

Com VSync off/unlocked (custo real por frame, RX 7600):

| tela | escala antiga | escala atual |
|---|---|---|
| Mode Select, 2194×1160 | ~10,9 ms (5x) | ~36% menos pixels (4x) |
| Master Mode (mundo) | ~13,4 ms (5x) | ~36% menos pixels (4x) |
| 4K | ~19,3 ms (6x) | ~30% menos pixels (5x) |
| Battle (fogo), 720p | ~7,2 ms (2x) | igual (2x) |

A escala automática agora é **2 (≤1280), 3 (≤1920), 4 (≤2560) e 5 (4K)**,
aplicada sobre `min(janela, Resolution)`. Mudar 4K→720p no menu reduz 5→2 na
hora (6,25× menos pixels internos). Em 720p com Resolution 2560 o interno cai
de 2560² para 1024² (~6,25× menos), que é a maior fonte do consumo excessivo
relatado.

A 60 Hz o orçamento é 16,7 ms. Em 4K a cena do Master Mode passa do orçamento;
para 60 FPS estáveis use 1600×900/1920×1080/2560×1440 (ou Fullscreen com um
modo menor). O menu em si custa ~2 ms e só é redesenhado quando muda.

## Entrada

O menu abre com **Square**:

- no **Mode Select**, onde aparece o prompt **OPÇÕES [□]** no canto superior
  direito;
- no **Master Mode**, **somente no menu de pause** (a tela com **Item, Quit,
  Status** e os outros ícones), onde aparece o prompt **OPÇÕES [□]** no canto
  inferior esquerdo. No mundo (a tela com “START button : Open Menu”) o Square
  continua sendo do jogo e o pad **não** é mascarado.
- No pause, o flag de campo (bit `0x800` de `0x617EB8`) precisa ficar limpo por
  **30 frames**, o ícone **Map** do pause (`tbp 15360/PSMT8`, faixa
  `x 200–310, y 15–130`) precisa ter sido desenhado **e** esse desenho precisa
  estar no **último quarto** do frame (os ícones 2D do pause são desenhados
  depois do cenário 3D). Um objeto do mundo que use a mesma textura é desenhado
  antes e não dispara o prompt; isso elimina os falsos positivos de
  transições/cutscenes que faziam o Square abrir o menu no meio do mundo e
  deixar o joystick “morto”.

| tecla | controle | ação |
|---|---|---|
| `A` (Square) | botão X | abre/fecha |
| `↑`/`↓` (ou `W`/`S`) | d-pad | navega nas linhas |
| `←`/`→` | d-pad | muda o valor (fica pendente) |
| `X` / `Z` / `Enter` | Cross / Start | **aplica e grava** |
| `Backspace` / `Esc` | Select | fecha e **descarta** o que não foi aplicado |

Enquanto o menu está aberto, o input não alimenta o jogo. Ao sair do Mode
Select/Master Mode, o menu fecha e descarta alterações pendentes.

Para diagnosticar o controle, o host imprime:

```text
[input] controller 0: <nome do controle>
[input] controller 0 disconnected
[input] pad 0 masked by recomp menu
[input] pad 0 released
```

O host reexamina os controles a cada segundo e reabre um pad que o driver
desconectou sem enviar `SDL_CONTROLLERDEVICEREMOVED`, então o joystick não fica
permanentemente morto. Se o controle não aparecer em `[input] controller`, o SDL
não o reconheceu (isso não é causado pelo menu); verifique o
mapeamento/Steam Input.

## Alterações não salvas

Mudar um valor com ←/→ **não** altera o jogo nem grava o ini. O menu mostra:

- o selo **NÃO SALVO** no topo;
- um `*` na linha modificada;
- o rodapé em destaque.

Ao apertar **X** (Cross/Start), todos os valores pendentes são aplicados de uma
vez e `game_settings.ini` é gravado. Ao fechar com **A/Esc/Backspace**, os
valores voltam aos últimos aplicados.

## `game_settings.ini`

O arquivo é criado na primeira execução na pasta do jogo/projeto (diretório de
trabalho do `ps2run`; `PS2_SETTINGS` permite outro caminho). Ele é recarregado
na inicialização **antes** do parsing da linha de comando; opções explícitas de
CLI vencem. A cada **X** o arquivo é regravado.

```ini
[recomp]
resolution = 1920x1080
screen_mode = Windowed
aspect = 16:9
fps = 60
anti_aliasing = Off
graphic_api = Auto
texture_filter = Auto
aniso = Off
draw_distance = Default
mtvu = On
hud_aspect = Auto
hud_scale = Auto
vsync = On
exit_game = Press X
```

- `resolution`: tamanho inicial da janela / modo exclusivo (`WxH`) e **teto da
  resolução interna**. A escala efetiva é `min(janela, resolution)`: uma janela
  de 720p roda em 2x mesmo com `resolution = 2560x1440`; aumentar a janela sobe
  a escala só até o teto.
- `screen_mode`: `Windowed`, `Fullscreen` ou `Borderless`.
- `aspect`: `4:3`, `16:9`, `21:9`, `32:9` ou `Fit` → `PS2_PRESENT_AR`
  (apresentação na hora; o FOV widescreen é reaplicado no frame seguinte com os
  patches carregados). Mode Select/character select continuam em pillarbox 4:3;
  pause, mundo 3D e battle usam o aspecto escolhido.
- `fps`: limite de apresentação (`30`, `60`, `120`) → `g_options.hz`; a lógica
  continua em 60/50. Com V-Sync On é limitado ao monitor, com V-Sync Off pode
  ultrapassar.
- `anti_aliasing`: `Off`/`FXAA`/`FXAA HQ`/`FXAA Ultra` → `g_options.aa`.
- `graphic_api`: `Auto`/`Vulkan` (+ `DirectX 12` no Windows) →
  `PS2_GPU_BACKEND`; aplicado no próximo start. `Software` foi removido.
- `texture_filter`: `Auto`/`Nearest`/`Bilinear` → `g_options.filter`.
- `aniso`: reservado (o GS não tem filtro anisotrópico).
- `draw_distance`: reservado (o jogo não expõe draw distance).
- `mtvu`: `On`/`Off` → ausência/presença de `PS2_NO_VU_THREAD`; próximo start.
- `hud_aspect`: `Auto`/`4:3`/`16:9`/`21:9`/`32:9`/`Off` → `PS2_HUD_ASPECT`.
- `hud_scale`: `Auto` ou fator → `PS2_HUD_SCALE` (aplica na hora nos próximos
  draws do HUD; o jogo precisa redesenhá-lo).
- `vsync`: `On` (Fifo) ou `Off` (Mailbox/Immediate) aplicado na hora.
- `exit_game`: ao apertar X, abre a confirmação nativa **Sair do jogo /
  Cancelar**. Só `Sair` encerra; `Cancelar`/Esc volta ao menu. O valor é
  apenas informativo.

`--widescreen` apenas liga os patches de widescreen; o **aspecto salvo no ini
vence** (21:9 continua 21:9 no próximo start). Só um `PS2_PRESENT_AR` explícito
no ambiente do launcher tem prioridade sobre o ini. `--window` garante que
exista uma janela; `--window-size 1280x720` abre uma janela menor sem mudar o
teto da resolução interna; o modo salvo decide windowed/fullscreen/borderless.
`--fullscreen` explícito sempre vence.

## Runtime do menu

`runtime/src/menu/recomp_menu.cpp` (incluído em `ps2runtime`) implementa:

```cpp
ps2::menu::menu().open/close/toggle();
ps2::menu::menu().up/down();    // linha
ps2::menu::menu().left/right(); // valor (staged)
ps2::menu::menu().confirm();    // aplica: commit + on_apply + grava ini
ps2::menu::menu().back();       // fecha e descarta
ps2::menu::menu().dirty();      // há alterações não salvas?
```

Hooks usados pelo host (`host/window.cpp`):

```cpp
auto &m = ps2::menu::menu();
m.on_change = nullptr;                 // sem preview/save
m.on_apply  = [](const Item &) { /* aplica tudo + settings_save() */ };
m.on_close  = nullptr;                 // fechar sem X descarta
```

## Teste rápido

```bash
# estado já no Mode Select; tecle A assim que a janela abrir
SDL_VIDEODRIVER=x11 PS2_PRESENT_DUMP=/tmp/menu.ppm \
PS2_PRESENT_DUMP_FRAME=16400 PS2_PRESENT_DUMP_LATE=1 \
./build/native-naruto/ps2run games/naruto/patched/SLES_556.05 \
  --disc ./jogo.iso --memcard memcard.ps2 --widescreen --ntsc \
  --patches games/naruto/patches.txt --patches widescreen_recomp.txt \
  --load-state menu/states/mode_select.ps2 \
  --window --resolution 720p --filter bilinear --frames 16450
```

O dump é gravado pelo próprio passe de apresentação, então inclui o menu.

`PS2_MENU_STATS=1` mostra o tempo de render/upload do overlay:
`[menu] menu 1920x1080 render ... ms upload ... ms`.
