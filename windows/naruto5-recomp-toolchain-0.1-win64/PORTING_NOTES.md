# Porting Notes - Naruto Shippuden: Ultimate Ninja 5 (PAL SLES_556.05)

## Environment
- Project: ps2-web-recomp (snuri00/ps2-web-recomp), ported with `scripts/port.sh naruto <iso> --native-only`
- Build: Windows (clang-cl + Ninja) via `scripts/winenv.sh`
- Disc: `games/naruto/disc.iso` hardlink to `jogo.iso`
- ISO: PAL SLES_556.05
- Toolchain: CMake 4.5.0-rc1, Ninja 1.13.2, clang 22.1.3 (VS18 LLVM)

## Key Fixes

### SIFCMD #20/#21 (GetNextRequest/ExecRequest) - Pull model
**Files**: `runtime/src/iop/sif.cpp`, `runtime/src/iop/kernel.hpp`, `runtime/src/iop/kernel.cpp`

Implemented guest-server pull model for SIFCMD. Evidence from disassembly/logs:
- SNDBASE imports sifcmd 14/17/19/20/21 (stubs at 0xB0/0xB8/0xC0/0xC8/0xD0). Server loop at module vaddr 0x40F4 does: `r = GetNextRequest(qd); if (r) { ExecRequest(r); } else { SleepThread(); }` (thbase #24). 
- CRI_ADXI uses RpcLoop push model (imports 14/16/17/19/22/24/25), registers server 0x90000200.
- SNDBASE registers servers 0x12346 (func 0xA9C, buf 0x19668) and 0x12347 (func 0x324, buf 0x19628) with queue as 7th arg.

**Changes**:
- Added `#20 sceSifGetNextRequest(qd)`: pops from `g_requests[qd]`; if empty sets `qd+4 = 0` and returns 0. If request pending, sets `qd+4 = 1`, allocates 0x40-byte IOP-RAM descriptor via `iop::kernel::alloc()`, fills real SifRpcServerData offsets (`+0x00` id, `+0x04` func, `+0x08` buffer, `+0x0C` size, `+0x1C` client_data, `+0x24` rpc_number, `+0x28` recv, `+0x2C` recv_size, `+0x30` recv_mode, `+0x34` server_data). Stores pending Request in `g_exec_requests[desc]`.
- Added `#21 sceSifExecRequest(desc)`: looks up in `g_exec_requests`, copies payload into server buffer, sets `t->rpc_server`, `t->rpc_return = c.r[31]`, `g_held_calls[server] = call`, sets `c.r[31] = g_finish_stub`, `c.r[4..6] = rpc_number/buf/size`, sets `c.pc = s.func`. Handler runs and `rpc_finish` sends reply/resumes guest at `rpc_return`.
- `deliver_request()` now wakes threads waiting in `kWaitSleep` (SNDBASE polls GetNextRequest + SleepThread), not just `kWaitRpc`.
- `sifcmd #19 SetRpcQueue` writes thread id/+4/+8/+0xC into guest's queue descriptor; `#17 RegisterRpc` writes `sd+0x40 = queue`.
- Added `Thread::rpc_return` in kernel.hpp, serialized in kernel.cpp.
- Runtime RPC model: EE→IOP Call packet (header 16 + record_id, packet_addr, rpc_id, client_data, rpc_number, send_size, recv, recv_size, recv_mode, server_data) → `g_requests[queue]` → guest handler → `reply_call()` copies reply directly into EE RAM at `call.recv` (works for fileio/loadfile).

**Result**: No more `[iop] missing sifcmd #20 called from 0x00024124`; SNDBASE runs (CCSND create thread, Usedrive(IOP): DVD, SNDBASE:Debug Printf Off); GS primitives increase dramatically (197→9598); frames render (no longer black). CRI_ADXI bind resolves (bind at ~line 321, server registered at ~line 326). Pull-model servers (SNDBASE 0x12346/0x12347) and push-model server (CRI_ADXI 0x90000200) both work.

### IRX Export/Import Layout (verified)
**IRX export record** (36-byte header): magic 0x41C00000, u32 0, u16 version, u16 mode, char[8] name, 4 u32 fields at +0x14. **fptrs[] array read at record+0x24** by `scripts/iop_lib.py`. **Export ordinal == index in array at record+0x14** (the 4 header u32 fields double as ordinals 0-3). Verified against ps2tek: SIFMAN GetSMFlag=0x17(ord?), SYSMEM ord 4 AllocSysMemory, 5 FreeSysMemory, 6 QueryMemSize.

**IOP module import record**: `u32 0x41E00000`, `u32 next`, `u16 version`, `u16 mode`, `char[8] name`, then 8-byte stubs `{u32 0x03E00008, u16 fno, u16 ori_zero}` at record+0x14.

### Other findings
- SIFCMD ordinals from disasm: 4 InitCmd=0x330, 6 GetSreg=0x360, 7 SetSreg=0x3D0, 8 SetCmdBuffer=0x580, 10 AddCmdHandler=0x820, 11 RemoveCmdHandler=0x10F4, 12 SendCmd=0x133C, 14 InitRpc=0x160C, 15 BindRpc=0x151C, 16 CallRpc=0x1564, **20 GetNextRequest=0x1818**, **21 ExecRequest=0x1880**, 22 RpcLoop=0x1A28, 23 GetOtherData=0xDF8, 24 RemoveRpc=0x16EC, 25 RemoveRpcQueue=0x1784, 28 SendCmdIntr=0x5CC.
- Real #17 RegisterRpc writes sd+0=id, sd+4=func, sd+8=buffer, sd+0x10=arg4, sd+0x14=arg5, sd+0x38=0, sd+0x3C=0, sd+0x40=arg6(queue) - 7 args. Runtime arg(c,6) reads sp+0x18 correctly.
- Real #19 SetRpcQueue writes queue+0=thread id, queue+4=0, queue+8=0, queue+0xC=0.
- Real #20 returns qd[0x0C], sets qd[0x04]=1, advances qd[0x0C]=head[0x3C], returns 0 when empty.
- Real #21 calls func(srv+0x24, srv+0x08, srv+0x0C) then replies via sceSifSendCmd(0x80000008, pkt64, 0x40, ...).
- HLE dispatch: handler sets c.pc = r[31] before calling HLE; HLE may redirect execution by setting c.pc/c.npc/c.r[31] (how rpc_loop/#21 work). c.r[31] at HLE entry = guest's return address.
- g_held_calls keyed by server id (one in-flight call per server); g_requests keyed by queue id.

### Desktop window (SDL2) + Windows portability
**Files**: `CMakeLists.txt`, `host/window.cpp`

- `host/window.cpp` was POSIX-only (`sys/resource.h`, `unistd.h`, `getrusage`, `/proc/self/statm`, `sysconf`). Added `_WIN32` branches using `GetProcessTimes` (CPU%) and `GetProcessMemoryInfo` (RSS). No behaviour change on Linux.
- SDL2 lookup: CMake now tries `find_package(SDL2 CONFIG)` against `third_party/sdl2/cmake`, then `${SDL2_DIR}/cmake`, then the system module. A vendored copy lives in `third_party/sdl2/` (SDL2 2.32.10 headers + `lib/x64` import libs).
- If SDL2 is missing, `ps2run` is built headless as before (`PS2_WINDOW` undefined).
- `SDL2.dll` must sit next to `ps2run.exe` (copied from `third_party/sdl2/lib/x64/SDL2.dll`).
- `--window` uses the SDL renderer path (`attach_gpu()` returns false on Windows), so it works with the software GS rasterizer — no Vulkan/wgpu required.

**Verified**: `[window] presenting through SDL` … `[ee] stopped after 240 frames`. Keyboard mapping in `window.cpp`: arrows = d-pad, Z = Cross, X = Circle, A = Square, S = Triangle, Q/W = L1/R1, 1/2 = L2/R2, Enter = Start, Backspace = Select, F11 = fullscreen.

## Current Status
- Boot works: IOP reboot with `cdrom0:\MODULES\IOPRP300.IMG;1`, SNDBASE.IRX + CRI_ADXI.IRX load, CDVDFS finds all data files.
- Sound/RPC paths functional. Frames render (not black). Game progresses past menus - frames change over time with button presses (tested up to frame ~3400).
- GS: 24859 primitives drawn in long run; software rasterizer only.
- Many EE threads sit waiting at 0x0015E9C8 (appears to be some wait/sleep function) - appears normal.

## Build/Run Commands
- Rebuild native: `. scripts/winenv.sh && ninja -C build/native-naruto`
- Rebuild tools: `cmake -S . -B build/tools -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang-cl && ninja -C build/tools eerecomp vurecomp`
- Reconfigure (needed after a CMakeLists change or to enable the window):
  `. scripts/winenv.sh && cmake -S . -B build/native-naruto -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang-cl -DPS2_GENERATED="C:/Users/guilh/naruto5code/ps2-web-recomp/generated/naruto"`
- Headless run: `PS2_VU_RECORD=games/naruto/vu1 ./build/native-naruto/ps2run.exe games/naruto/disc/SLES_556.05 --disc games/naruto/disc.iso --misses games/naruto/misses.txt --memcard games/naruto/explore.ps2 --frames 600 --press 300:start --frames-dir games/naruto/shots --frame-every 100`
- Windowed run: same command with `--window` (add `--fullscreen` for fullscreen). `SDL2.dll` must be next to `ps2run.exe`.

## Code windows: PRG overlays (BTL/ADV/ETC) at 0x006C6D00
**Symptom**: Windowed run reached the real game (intro/menus, ~3069 frames, 564206 GS primitives) then halted:
```
[fileio] open cdrom0:\PRG\BTL.BIN;1 -> 3
[ee] no translated code at 0x008EA080 (ra 0x0011A0EC)
[ee] halted: missing code at 0x008EA080 (ra 0x0011A0EC)
```
**Evidence**:
- ELF has a contentless executable PT_LOAD window at `vaddr=0x006C6D00` with `memsz` 0x22D000 / 0x1F4B00 / 0x29F00 — one per PRG overlay image.
- `PRG/BTL.BIN` (0x226180 bytes) header: `"MWo3"`, ver 1, **load addr 0x006C6D00**, export table `[0x008ECDF0, 0x008ECE14)` (9 entry points). `PRG/ADV.BIN` ver 2, same base, exports `[0x008BB380, 0x008BB3AC)` (11). `PRG/ETC.BIN` ver 3, same base, no exports. Images are uncompressed MIPS (5665 `jr ra` in BTL) and already contain absolute pointers, so the loader copies them verbatim (no relocation).
- `0x008EA080 - 0x006C6D00 = 0x223380` is a valid function prologue (`addiu sp,sp,-0x230`), and `0x008EA080` is one of the 9 pointers in BTL's export table (at file offset 0x2260F0).
- The caller at `0x0011A0EC` is a loop `lw v0,0(s0); jalr ra,v0` over the export table `[0x8ECDF0,0x8ECE14)` — a pointer-table call, so recursive-descent never found it from the static program.
- The old `scripts/windows.py capture` dump only captured heap data at `0x006C4000` (16 KB) and the 0x8F0000 window; BTL was never captured because the crash prevented a dump at that point.
**Fix**:
- Added the disc images directly as code-window overlays (deterministic, no RAM dump needed):
  - `games/naruto/overlays/006C6D00_F68554B6.bin` = `PRG/BTL.BIN`
  - `games/naruto/overlays/006C6D00_7AA46D27.bin` = `PRG/ADV.BIN`
  - `games/naruto/overlays/006C6D00_8368B262.bin` = `PRG/ETC.BIN`
- `games/naruto/entries.txt` seeds each module's export table (BTL: 0x223380, 0x225AF0…0x225FD0; ADV: 0x1E3980…0x1F33E0) as `<module>:<offset>` tokens.
- Removed stale `006C4000_79FE1550.bin` (heap snapshot, not a module) so it cannot serve over the real overlays.
- `recomp/main.cpp` `read_overlays` consumers: when several images share one base, seed the window from the **widest** image (`window_extent[base]`), not from whichever file sorts first.
**Result**: eerecomp now reports `8 distinct code window images of 8 captured, code 885937 instructions`, 498 units, and `0x8EA080` is translated (unit 0x89, entry index 2080 = function 123).
**Validation (state 4500, mid-opening)**: pressing Start during the opening skips it; the load flow then reports
`[fileio] open cdrom0:\PRG\BTL.BIN;1 -> 3` and the run continues for 1300 more frames with no `no translated code` and no halt at `0x008EA080` (previously the halt was immediate). VIF1 now shows real VU1 microcode starts (`01/10/11/13/14/17/20/30`) after the load, i.e. the battle overlay is running.

**Repro inputs (headless)**: at the title, `--press 700:start+cross:60` enters the intro; pressing `start` at ~4600-5000 during the opening skips it and loads BTL. Only `start` does not advance the title — Cross is required.

## Post-BTL "stall": it is the memory-card dialog, not a hang
**Symptom**: after BTL loads, the screen stays static (2305/1644 colours) for thousands of frames; all EE threads appear blocked. It looked like a missing reply/stream, but there were no CD reads, no fileio opens and no missing code.
**Diagnosis**: OCR of the dumped frames shows the game is waiting on input in a message box, not on an I/O event. Two different screens alternate because the message has two pages:
1. `No Naruto Shippuden: Ultimate Ninja 5 data found on memory card (PS2) in MEMORY CARD slot 1. Please insert memory card (PS2) with ... data.` + `[e] Next`
2. `Return to Title Screen?` + `YES/NO`
`Cross` advances page 1→2; `Left`/`Right` move the selection; `Cross` confirms. Choosing **No** returns to the Game Mode Select.
**Root cause of the loop**: `--memcard` points at a new, unformatted card image. The game therefore takes the *load/continue* path, finds no data, and can only return to the title — it never offers "start without saving".
**Fix**: run **without `--memcard`** (no card inserted). The startup check then shows the intended dialog:
`No memory card (PS2) is inserted. ... Start the game anyway?` + `YES/NO`
The default selection is **NO** (German `NEIN` in this PAL image); `Left` selects YES, `Cross` confirms. `jogar-naruto.bat` no longer passes `--memcard`.
**Repro**: boot, `--press 700:cross:10` (title), `--press 1600:cross:10` (skip opening) → dialog at ~1800; `Left`+`Cross` → opening movie. The full opening (`OPENING.PSS`, ~61MB) is long and must finish before the Game Mode Select appears.
**Notes**: the runtime translates the on-screen text according to the disc language (this image uses German). `Up`/`Down` change the Game Mode Select cursor; `Left`/`Right` do not. The BTL crash at `0x008EA080` is fixed (overlay fixes); the game now progresses past it to the menu flow.

## RPC server `0x00012346` is SNDBASE, not CRI (and the "empty reply" workaround was the bug)
**Symptom**: after dismissing the "Start the game anyway?" prompt, the game called server `0x00012346` and appeared to freeze before the opening movie. An earlier log also showed `[sif] call to 0x00012346 fn 37888 is waiting for its reply` plus all EE threads parked at `pc 0x0015E9C8` and `[sema] 2 count 0 waiters 1`.
**Wrong turn**: the server id was assumed to belong to `CRI_ADXI.IRX`, so a special case was added in `runtime/src/iop/sif.cpp` that ignored the call and wrote a synthetic `0,1,0` reply. That workaround *caused* a hang earlier in the flow: it bypassed the real guest handler.
**Evidence that identified the server**:
- `games/naruto/explore_ram.bin` at `0x416170` holds an EE-side module table: `{0x006C4F10, 0x00012346, 0x006C4F38, 0x00012347, "sndbase.irx", "\DATA\SNDDATA.BIN;1"}`.
- `MODULES/SNDBASE.IRX` (from `disc.iso`) imports `sifcmd` funcs 14/17/19/20/21 (RegisterRpc, SetRpcQueue, GetNextRequest, ExecRequest) and contains the `file close error!` string.
- Headless trace (`--trace-iop --trace-iop-hle`) shows:
  `[iop-hle] sifcmd:sceSifRegisterRpc(001D4F94, 00012346, 00020A9C, 00029668)` →
  `[sif] server 0x00012346 registered (func 0x00020A9C buf 0x00029668)` and the same for `0x00012347` (`func 0x00020324`).
- EE calls are then dispatched to the guest normally: `[sif] request 0x... for server 0x00012346 handed to the guest`, `[sif] server 0x00012346 runs fn ...`.
**Fix**: removed the `0x00012346` special case and the 2-second "held HLE call" auto-reply from `sif.cpp`. Unknown-server calls must never be answered with fabricated data; if a guest server is registered, its handler owns the reply.
**Validation (headless, state 1900)**: `--press 1940:left:30 --press 1990:cross:30` dismisses the no-card prompt → title (27661 colours) → `--press 2650:cross:20` → second prompt → `--press 2850:left:30 --press 2950:cross:30` → opening movie plays:
`[ipu] commands: 0:516 2:144657 3:532382 ... out 19788592 qwords` at frame 4200, no missing code, no held SIF calls.
**Input detail**: `Left`+`Cross` **held in the same frame does not work** in the `YES/NO` dialog (confirm is processed before the cursor moves). Press and release `Left`, then press `Cross`.
**Sound init traffic observed after the title (all served by the guest handler)**: fns `33024`, `256`, `784`, `800`, `4928` (path `cdrom0:\DATA\`), `4944` (`snddata.bin`), `37392`, `36944`, `160`, `176` to server `0x00012346`, plus `0x90000200` (CRI_ADXI). fn `37392` is a blocking (mode 0) call and completes normally.

## Sema 12 deadlock fix (2025-10-24)
**Symptom**: The game boots and runs but deadlocks at the language selection / opening movie. Thread 1 (main) waits on sema 12 (`WaitSema` at 0x0015EAE8), but sema 12 is never signaled. The wait loop cycles through semaphores 2, 3, 7, 8, 10, 12 (seen in `WaitSema` syscall trace). The `vblank()` function was only signaling semaphores 2, 3, 7, 8, 10 — sema 12 was never signaled. Thread 2 (priority 0, highest) was READY but never ran because thread 3 (priority 8) was stuck in a tight loop at 0x12E728 (spin on `*(u64*)0x3EE908 == 0`), and the preemption logic wasn't correctly forcing a yield.

**Fixes applied**:
1. **runtime/src/system.cpp:169**: Added `kernel::signal_sema_irq(12)` to the VBlank signaling list.
2. **runtime/src/kernel.cpp**: Added semaphore wait timeout logic in `check_sema_timeouts()` (called from `idle()`) so threads blocked on semaphores for more than ~1 frame (frame_cycles/60) get woken with return value -1 (timeout).
3. **runtime/src/system.cpp**: Added preemption logic in `preempt()` that checks if a higher-priority READY thread exists and yields by setting `c.budget = 0` and calling `kernel::reschedule()`.
4. **runtime/src/kernel.hpp + kernel.cpp**: Exported `best_ready()` and `reschedule(Cpu&)` for use from `system.cpp`.

**Result**: Thread 2 (prio 0) now preempts thread 3 (prio 8), sema 12 gets signaled on VBlank, and the game progresses past the language selection.

## Build/Run Commands
- Rebuild native: `. scripts/winenv.sh && ninja -C build/native-naruto`
- Rebuild tools: `cmake -S . -B build/tools -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang-cl && ninja -C build/tools eerecomp vurecomp`
- Reconfigure (needed after a CMakeLists change or to enable the window):
  `. scripts/winenv.sh && cmake -S . -B build/native-naruto -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang-cl -DPS2_GENERATED="C:/Users/guilh/naruto5code/ps2-web-recomp/generated/naruto"`
- Headless run: `PS2_VU_RECORD=games/naruto/vu1 ./build/native-naruto/ps2run.exe games/naruto/disc/SLES_556.05 --disc games/naruto/disc.iso --misses games/naruto/misses.txt --memcard games/naruto/explore.ps2 --frames 600 --press 300:start --frames-dir games/naruto/shots --frame-every 100`
- Windowed run: same command with `--window` (add `--fullscreen` for fullscreen). `SDL2.dll` must be next to `ps2run.exe`.

## Free Battle reached (2025-10-24)
**Repro (Linux, headless, from the title state)**:
`./build/native-naruto/ps2run games/naruto/disc/SLES_556.05 --disc games/naruto/disc.iso --memcard /tmp/cardF.ps2 --load-state /tmp/title_fmt.ps2 --press 3100:up:10 --press 3300:cross:10 --press 4300:down:10 --press 4500:cross:10 --press 5200:cross:10 --press 5600:cross:10 --press 6000:cross:10 --press 6400:cross:10 --press 6800:cross:10 --press 7000:cross:10 --press 7200:cross:10 --press 7800:cross:10 --frames 20000 --frame-every 2000 --frames-dir /tmp/bFinal`
- Title default is **Load Game**; `Up` then `Cross` is New Game. The presses above go New Game → Game Mode Select → Free Battle (1P vs COM) → Select Character → Linked Mode prompt → VS → battle.
- The battle plays (health bars, combo counter, specials) and ends by KO; the game then returns to Select Character. A 20,000-frame run from the title reaches the frame limit with no halt and no `missing code`.
- The memory card must be formatted; `/tmp/naruto_card.ps2` works. The card is not part of save states, so pass `--memcard` when loading one.

**Code-window identification fix**:
- `recomp/main.cpp` `checks_for_window` now samples 256 code words across the image instead of emitting every translated word (the F68554B6 table had 491,023 checks).
- `runtime/src/dispatch.cpp` tolerates up to 8 mismatched checks for fixed-base images, and `in_place` re-runs the full check before evicting an instance. The game writes data into the window; without this, a data write that landed on a check word made `instance_for` return null and valid F68554B6 code was reported as missing (plain-address misses such as `0x007232E0`).
- After the fix, all misses resolve to `006C6D00_F68554B6:<offset>` tokens.

**New entry points**: 105 `006C6D00_F68554B6:<offset>` seeds were added to `games/naruto/entries.txt` (functions reached only through vtables/function pointers). They were collected with a temporary `PS2_MISS_RETURN=1` mode that records the miss and returns to `ra`; the mode was removed after collecting. `eerecomp` now reports `11 distinct code window images of 14 captured, code 888901 instructions`, 1215 units.

**`close` fix validated**: `SNDBASE.IRX` loads at `0x00020000` and the 20,000-frame battle run shows no `file close error!` and no `[sif] ... fn 37888 ... waiting for its reply`.

**Save state**: `--save-state 8490:/tmp/battle8490.ps2` while the battle is running; resume with `--load-state /tmp/battle8490.ps2`. `--frames` must be greater than the save frame, otherwise the run halts before the save request is handled.

## Battle/Master Mode code-pointer coverage (2025-10-24)

**Symptoms**: after reaching Free Battle, pressing attacks halted at `0x00722E00`, then `0x00756040` (BTL/`F68554B6`). Master Mode halted at `0x00811E10`, then `0x007FAF90` (ADV/`7AA46D27`). All were valid functions reached through RAM function-pointer tables (vtables/sparse arrays), not direct calls.

**Fix**: seeded all pointer-sized values that point into each overlay's code window, using RAM dumps captured at the crashes:
- BTL `F68554B6`: 11,523 unique targets from the battle RAM dump.
- ADV `7AA46D27`: 15,840 unique targets from the Master Mode crash dump.
- 12,995 new tokens appended to `games/naruto/entries.txt`. After removing 125 seeds that were data tables (first four words all pointing into the same overlay window), the file has 90,825 lines. `eerecomp` reports `11 distinct code window images of 14 captured, code 955,780 instructions`, 1,337 units.
- The scan also includes data pointers. `recomp/codegen.cpp` was made robust: a delay-slot fallthrough only emits `goto L_<addr>` when the target is translated code; otherwise it calls `exit_to(addr)`. This avoids undefined labels from data seeds.

**Validation**:
- Free Battle, loaded at `/tmp/freebattle.ps2` (frame 26000), pressed Cross/Circle/Square/Triangle for 6,000 frames: no halt (32,000-frame run).
- Master Mode from the title (`/tmp/title_real.ps2`, frame 19000), New Game -> Master Mode -> periodic Cross for 10,000 frames: no halt (30,000-frame run).
- In the headless Linux boot the title appears at frame ~19000, then the game enters attract/demo mode at ~20000. Save the state at 19000 for a usable title state.
- `games/naruto/memcard.ps2` is 8 MB and not in the runtime's 8,650,752-byte card format; the game shows "No data"/"No memory card" dialogs. For headless runs, omit `--memcard` and choose YES with `Left` then `Cross` on "Start the game anyway?" (the choice is horizontal; `Up` does nothing).

**Overlay selection fix**: the removed data-table seed at ADV `0x1F4680` (RAM `0x8BB380`) was an array of function pointers iterated by `0x0011A0EC`, which calls `0x008AA680`. Seeding the table as code confused overlay selection. `runtime/src/dispatch.cpp` now has `module_has_entry(m, pc, base)`; `instance_for` only returns a cached/fixed/dynamic module when that module actually has a translated entry at `pc`, so an overlay is never picked just because it sits at `0x6C6D00`.

**Shop/Collection validation (2025-10-24)**: from `/tmp/mode_select.ps2` (frame 21000, Game Mode Select, Master Mode selected), the carousel order is Options, Master Mode, Free Battle, Practice, Shop, Collection, then wraps. Navigation takes effect about 400 frames after each press.
- Shop: three `Down` (21200/21600/22000) then `Cross` at 22400; tutorial `Next` at 24000; item submenu and list at 25000/25600. 40,000 frames, no halt.
- Collection: four `Down` then `Cross` at 22800; `Cross` at 23600 opens Characters; `R1` at 24400 pages and `Cross` at 25200 selects Gaara. Movie and Music also open. 34,000-40,000 frames, no halt.
- Master Mode: `Cross` every 400 frames from 21200; reached the intro movie (IPU DMA) by ~36000 frames, no halt.

**Build note**: use `ninja -C build/native-naruto -j6 ps2run` (and `ninja -C build/tools -j6 eerecomp`) to avoid saturating the machine.

## Gameplay lag: VU1 microcode was never compiled (2025-10-25)

**Symptom**: the game runs fine in menus, but during a battle the frame rate drops to ~32 fps
(~31 ms per frame), i.e. visible lag/slow motion. The user suspected missing EE entries or
function pointers.

**Ghidra pass**: the ELF (`games/naruto/disc/SLES_556.05`) was imported with
`ghidra-analyzeHeadless` (no GhidraMCP server was available). It found 17,683 raw pointer tables
in the main ELF, 4,037 in ADV (`7AA46D27`) and 125 in ETC (`8368B262`), but none of the hot
gameplay paths were missing: a missing EE entry halts with `no translated code at ...`, it does
not slow the game down. The pointer tables were therefore not the cause.

**Profiling**: the native build already includes the sampling profiler
(`PS2_PROFILE=file PS2_PROFILE_FROM=frame`). From a mid-battle save state:
```
PS2_PROFILE=/tmp/prof.txt PS2_PROFILE_FROM=12100 ps2run ... --load-state /tmp/battle.ps2 --frames 12400
```
Top self time:
```
57.08%  ps2::vu::run_fast(int, unsigned int)          <- VU1 interpreter loop
12.21%  ps2::vu::(anonymous)::lower(...)
 6.75%  ps2::vu::(anonymous)::set_flags(...)
 1.40%  ps2::vu::(anonymous)::upper(...)
```
That is ~77% of the frame inside the VU1 interpreter.

**Root cause**: `games/naruto/vu1/` was empty and `generated/naruto/` had no `vu1_*.cpp`, so no
VU1 program was pre-compiled. On native builds the runtime JIT is off by design
(`ps2::vu::jit::mode()` returns 0 outside Emscripten), so every VU1 program fell back to
`run_fast()`. Menus barely use VU1; the battle uses it for every vertex/lighting pass.

**Fix** (project's intended AOT VU workflow):
1. Record the microcode while playing the real flow:
   ```
   PS2_VU_RECORD=games/naruto/vu1 ps2run games/naruto/disc/SLES_556.05 \
     --disc games/naruto/disc.iso --memcard memcard.ps2 --load-state /tmp/title.ps2 \
     --press 7100:up:10 --press 7300:cross:10 --press 7600:down:10 --press 7800:cross:10 \
     --press 8300:cross:10 --press 8600:cross:10 --press 8900:cross:10 --press 9200:cross:10 \
     --press 9500:cross:10 --press 9800:cross:10 --press 10100:cross:10 --press 10500:cross:10 \
     --frames 15000 <attacks every 20 frames from 11500 to 14700>
   ```
   This wrote 51 `.bin` files (16 KB each) into `games/naruto/vu1/`.
2. Compile them:
   ```
   ./build/tools/vurecomp games/naruto/vu1 generated/naruto
   # 114 VU1 programs, 63 of them jump targets
   ```
3. Rebuild: `ninja -C build/native-naruto -j6 ps2run`. `register_generated_vu()` registers the
   table at startup and `lookup()` now finds a compiled function for every VU1 entry.

**Validation**:
- `PS2_VU_COUNT=1` over the full title -> Free Battle flow (15,000 frames, attacks) prints no
  `[vu1] compiled ... interpreted` line: zero interpreted VU1 calls.
- `PS2_VU_CHECK=1` (compiled vs interpreter) reports only flag-only differences
  (`vf-1 vi-1 q/p/i/r 0 ... mem -1`), which is expected: the generated code skips MAC/clip flag
  computation when no later instruction reads it. Vector registers, VI and VU memory match.
- Frames dumped from the same battle state with `PS2_VU_INTERPRET=1` and with the compiled
  programs are visually identical (small +/-1..8 shading differences only).

**Performance** (same battle state, 500 frames, windowed, PAL 50 Hz cap):
| | before (VU interpreted) | after (VU compiled) |
|---|---|---|
| headless `PS2_TIME` | 31.32 ms/frame | 16.84 ms/frame |
| windowed fps | 34.6 fps | 49.8 fps (cap) |

**If a new scene is slow**: run it with `PS2_VU_COUNT=1`. If `[vu1] compiled N interpreted M: <pc>:<count>`
appears, record that scene with `PS2_VU_RECORD=games/naruto/vu1`, re-run `vurecomp` and rebuild.
The recordings and the generated code are gitignored (`/games/`, `/generated/`), so they must be
regenerated on a fresh checkout.

## Collection ultimate: partial overlay + SCANMSK ghost

Two bugs showed up when playing Kankuro's ultimate from Collection > Characters >
(second page, R1) > Kankuro > Ultimate Jutsu > first technique > Play.

**1. `missing code at 0x008A0880`.** BTL, ADV and ETC all load at `0x006C6D00`, but ETC only
covers `[0x006C6D00, 0x006F0C00)` (`size 0x29F00`) and leaves the rest of the previous BTL image
byte-for-byte intact. The battle callback at `0x00366050` keeps calling BTL code at `0x008A0880`,
which is above the ETC end. `instance_for()` used to discard the cached BTL instance as soon as
the module header at `0x006C6D00` changed to ETC.

`runtime/src/dispatch.cpp` now models this overlap: `Instance` keeps `tail_start`, and a fixed-base
instance whose header was replaced by a smaller overlay stays valid for offsets
`>= tail_start` (the active overlay size). The tail is proven with `tail_loaded()` (the module's
check words at or above the occupant size) and one tail check word is sampled per call. The same
path is used when the BTL instance was not cached yet (e.g. loading a state directly).

**2. Ghost of the 3D scene over the Collection UI.** The ultimate writes GS `SCANMSK` (reg `0x22`)
with mask 3 at the end of its render pass and never clears it. The Collection layer is drawn
afterwards with that mask still active: only one field is written, and the other field keeps the
last 3D frame. On the interlaced display this looks like a translucent Kankuro "ghost" over the
Collection screen.

The game writes `SCANMSK=3` once, during the ultimate, and never clears it. The same frames with
the mask forced to 0 are visually identical, so the register is not doing anything the game needs
on screen. The port therefore ignores `SCANMSK` and always draws every line:
- `runtime/src/gs/raster.cpp` (`plot`): the `scanmsk` early-outs are gone.
- `runtime/src/gs/gpu/gs.wgsl` (`plot`): same.

The ultimate still renders normally; the Collection layer is no longer half-masked and the ghost
(and the residual scanlines) disappear.

**Validation** (state at the ultimate list, then Cross, Cross):
```
ps2run $ROM --disc jogo.iso --memcard memcard.ps2 --widescreen --ntsc \
  --load-state /tmp/uj_select.ps2 --frames 44200 \
  --frames-dir /tmp/final2 --frame-every 100 \
  --press 41200:cross:8 --press 42500:cross:8
```
- `frame_43200.ppm`: ultimate still renders normally.
- `frame_43800.ppm`: Collection screen with the TV and the Kankuro technique list, no ghost.
- Window capture with `PS2_SCALE=4 ... --window` shows the same clean screen.

## Progressive-scan option

The game calls `SetGsCrt(interlace=1, mode=3, field=0)` and keeps writing `SMODE2=1`. Add
`--progressive` to force a progressive presentation:

- `runtime/src/hw.cpp` (`gs_set_crt`): forces `interlace=0`.
- `runtime/src/gs/gs.cpp` (`write_privileged`, SMODE2): clears bit 0.
- `runtime/src/gs/gs.cpp` (`read_privileged`, CSR): does not report the odd-field bit, so the game
  does not alternate fields.

Usage:
```
PS2_SCALE=4 ./build/native-naruto/ps2run ... --window --progressive --widescreen --ntsc
```

Note: the brief (1-2 frame) black flashes at the end of Kankuro's ultimate are not interlacing;
they come from the game's own flash/fade effect (`Flash!` / `FADE END` strings in the ELF) and are
present with both the GPU and the software rasterizer. `--progressive` removes the interlace
presentation, not the scripted flashes.

## Frame rate, refresh rate and filtering options

These are separate knobs:

- `--fps 30|60|120|unlocked`: target **presentation/animation rate**. The emulated vblank stays
  native (60 NTSC / 50 PAL), so the game runs at normal speed. `30` is the native Naruto scene
  rate; `60`/`120` are produced by temporal interpolation (`--interpolate` is implied for these).
  `unlocked` follows the display refresh.
- `--hz n`: presentation cap. With no value it is queried from the current display
  (`open_window` prints `[window] display N Hz`) and is at least 60. `120` only works on a 120 Hz
  display; a 60 Hz display caps it at 60. The monitor's physical refresh is the hard limit.
- `--interpolate`: generate an intermediate frame between the game's 30 fps updates. The frame
  is a temporal blend of the previous and the new game frame (1-frame delay). It is automatically
  enabled for `--fps 60`, `--fps 120` and `--fps unlocked`.
- `--filter nearest|bilinear`: overrides the GS `TEX1` bilinear bit for every texture.

The window title now shows `game X fps` (unique game/scene updates), `present Y Hz` (frames sent
to the window) and `display Z Hz` (queried monitor refresh). For example, on a 60 Hz monitor:
`game 30.0 fps  present 60.0 Hz  display 60 Hz`.

The game's 30 fps scene update is hard-coded (the main loop waits on the engine semaphore every
two fields). Changing that would also change game speed, so the port uses temporal interpolation
instead. A true 60 fps game-logic patch would need to halve every animation delta as well.

`--progressive` forces `SetGsCrt(interlace=0)`, clears `SMODE2` bit 0 and stops reporting the odd
field in `GS CSR`. It changes the presentation mode, not the game's 30 fps update rate.

The short black flashes at the end of Kankuro's ultimate are scripted (`Flash!` / `FADE END` in the
ELF) and appear in both the GPU and software rasterizers; they are not interlacing.

Mipmaps: the PS2 GS supports mipmaps (`MIPTBP1/2`), but the port samples only the base level. True
trilinear (bilinear inside two mip levels plus a blend between them) is not implemented yet;
`--filter bilinear` is the current texture-filtering control.

## VU1 `vfix` SSE2 optimization (Master Mode, 2026-10-27)

`vfix` (denormal/Inf clamp used on every VU float load/result) was implemented with
arrays/memcpy and scalar min/compare. Replacing it with SSE2 intrinsics
(`pand`/`pcmpgtd`/`por` + sign) is bit-exact and gave a large win in Master Mode:

| version | wall (60 frames) | per frame |
|---|---|---|
| original `vfix` | 1275 ms | 21.26 ms |
| SSE2 `vfix` | 822 ms | 13.70 ms |

VU thread idle went from 18% to 62% at the game's 30 fps. Removing MAC/status flag writes
(`upper(hi, false)`) measured 21.24 ms, i.e. neutral, and is semantically risky because MAC/status
survive between microprograms; it was reverted. A main-loop bypass to a port vblank sync still
freezes (thread resume/PC handling), so the game remains 30 fps; the next step is freeing the
game's 2-vblank frame-skip. A backup of the pre-optimization VU1 files/codegen is in
`/tmp/vu1_backup`. `PS2_BENCH=from,to` prints the deterministic per-frame host time.

## PSS opening slowdown: real cause and fix (2026-10-27)

The opening movies (`LOGO_C.PSS`, `NARUTO10.PSS`, `OPENING.PSS`) were running at ~40 fps while
the 3D scenes already ran at the game's 30 fps. The `vfix` SSE2 change was suspected, but an A/B
test over the same 800 emulated frames showed no regression:

| `vfix` | opening frames 1800..2100 |
|---|---|
| original | 22.1–25.2 ms/frame |
| SSE2 (current) | 20.4–24.4 ms/frame |

The sampler showed the real hot path during video: EE block dispatch (`gen::unit_*`,
`ps2::dispatch`, `ps2::st32/ld32`) and the software IPU decoder (`decode_macroblock`,
`run_command`, `convert`). The VU was 97% idle during playback.

Fixes, in order of impact:

1. **`dispatch()` cached its debug environment lookups.** It called `std::getenv` for
   `PS2_TRACE_SCENE/MODE/MODE_STATE/LOADER/WINDOW` on every dispatched guest block (~20k blocks
   per video frame). Cached as file-scope booleans: 18.3 → 18.0 ms, and Master Mode
   13.5 → 12.4 ms/frame.
2. **IPU bit reader:** `bits_at` built a 64-bit window byte by byte on every `peek` (once per
   coefficient). It now does one unaligned 64-bit load plus `bswap`, with the old loop only for
   the tail: 18.3 → 15.1–15.7 ms/frame.
3. **IPU YUV→RGB `convert` in SSE2:** eight pixels at a time with `madd`/`packus`, same fixed
   point constants and the same alpha thresholds as the scalar path. Verified bit-exact with
   `PS2_IPU_CHECK=1` over 2000 frames (it decodes each macroblock twice and compares):
   15.1–15.7 → 13.8–14.7 ms/frame.
4. Minor: `hw::irq_pending()` hint so `kernel::service` can return immediately when nothing is
   pending (it runs once per guest block), the unit index uses a shift when `g_unit_size` is a
   power of two, and the IOP/CDVD search trace prints the frame number.

Result in the window for the whole 40 s opening: `present 59.2–61.0 Hz` at 105–110% CPU
(was ~40 Hz). Master Mode is unchanged visually at 30 fps scene / 62–65% VU idle, and now
measures 12.55 ms/frame in `PS2_BENCH=30001,30061`.

Diagnostics added: `PS2_STATS=1` prints the window title line once per second,
`PS2_LOOP_STATS=1` splits each frame into events/IOP/SIF/kernel/service/dispatch,
`PS2_BENCH_TL=from,step` prints a per-step ms/frame timeline, and `PS2_IPU_CHECK=1` cross-checks
the SSE2 colour conversion against the scalar reference. Backups of the touched files are in
`/tmp/fps60_backup`, and the pre-`vfix` VU1 files remain in `/tmp/vu1_backup`.

## Colour path: RGB throughout (2026-10-27)

The PSS bitstream is YUV 4:2:0, but it never reaches the GS or the window as YUV. The IPU's CSC
command converts it to RGB (`convert()` in `runtime/src/ipu/ipu.cpp`) and the game chooses the
output width with the CSC `rgb16` bit. During the whole opening the counter shows
`csc out rgb16 0 rgb32 246400`, i.e. the game asks for 32-bit RGB (RGBA8888). The GS framebuffer
is PSMCT32/PSMCT16 as usual and the presentation surface is `BGRA8Unorm`/`RGBA8Unorm`
(`ABGR8888` on the SDL fallback), so the output is RGB end to end.

## Why the game logic stays at 30 fps (60 Hz output)

Dumping consecutive frames in Master Mode shows identical pairs (`frame N` and `N+1` differ in 0
bytes, then `N+2` changes ~23% of the pixels): the renderer presents the same image twice. The
scene hook (`ps2_note_scene_update`, thread 9) also fires every second vblank. The scheduler trace
shows the chain `vblank IRQ (t0) -> t4 -> t1 -> WakeupThread(7) -> t7 wakes t8..t12 -> t9 scene`,
and the `WakeupThread(7)` at `0x001D5778` only happens on even frames. The global at `0x003EDBB8`
is a vblank counter (it advanced exactly +1 per vblank: 29977 at frame 30003, 30034 at 30060), so
the engine counts fields and treats two fields as one logic frame.

This is the game's design: it is a PAL 25 fps title (SLES_556.05) and the 60 Hz mode gives 30 logic
frames per second. The well-known PCSX2 60 fps patch for this game unlocks the limiter and, by its
own description, makes the game "run much faster than it should", because the logic is fixed-step.
The port is not the limit: Master Mode needs 12.55 ms/frame against the 16.67 ms vblank budget and
the VU thread is 62-65% idle.

Real 60 fps logic without speeding up would require a game-specific re-timing mod: update once and
scale every per-frame increment (animation, physics, timers) by 1/2. That is a ROM-hacking effort,
not a generic emulator change, and no such patch exists for this title (the existing ones only
double the speed). Options kept open for a later attempt; the current behaviour is correct-speed
30 fps logic with 60 Hz presentation and a 60 fps opening movie.

Follow-up: an unconditional `[sig35]` print had been left inside `signal_sema`, writing to stderr for
every semaphore signal. Removing it cut the opening from 13.8-14.7 to 10.7-11.5 ms/frame and the
window CPU from ~105% to 68-80%, still at 59.7-60.5 presents/s. Environment-gated traces are now
used for scheduler/semaphore/syscall work (`PS2_SEMA_TRACE`, `PS2_SYSCALL_TRACE`,
`PS2_TRACE_SCENE_UPD`).

## 60 fps unlock experiment (2026-10-27)

An experimental `PS2_FORCE_WAKE=<thread>` hook releases a thread parked in `SleepThread()` at
every vblank. Waking thread 7 (the logic dispatcher) makes the engine update the scene once per
field instead of once per frame: the window reports `scene 59.7-60.7 fps` and Master Mode
processes 1191 frames in 20 s (59.6 vblanks/s), with VU ~70% busy, CPU ~200% and GPU 8%. The
render work overlaps with a thread, so this is real-time feasible.

The engine is fixed-step, though. A deterministic walking test (left stick up for 120 vblanks)
compared three runs: normal 120 vblanks, forced 120 vblanks and normal 240 vblanks. Position-like
floats moved -179.5 / -305.4 / -314.4 respectively: the forced run matches the 240-vblank walk, so
it advances twice as fast. The scene entry (`ps2_note_scene_update`, guest `0x001EF8E0`) receives
no delta-time argument (a0=4, f12-f14=0) and there is no global game-speed cheat for this title.
The community 60 fps patch behaves the same way (it only unlocks the limiter and speeds the game
up). A correct 60 fps would need a per-system re-timing mod (halve every per-frame increment:
animation, physics, timers, camera), which is a ROM-hacking effort; the hook is therefore OFF by
default and exists only for experiments. Default behaviour remains 30 fps logic with 60 Hz output.

### Re-timing investigation (continued)

The frame counter at `0x003EDBB8` is written by the vblank path once per field, but it does not
feed the logic: intercepting its increment so it advanced at half rate changed nothing in the
deterministic walking test (position still matched the 240-vblank walk). The character animation
indices (`+0x468..+0x470` of the player object at `0x00C18140` in the Master Mode state) advance by
one per logic frame, and the player object's speed field (`+0x244`) is a per-frame constant rather
than a delta-time product. The scene entry receives no delta argument. So the engine is fixed-step
throughout: 60 updates per second makes movement, animation and timers run twice as fast.

A correct 60 fps therefore needs a per-system re-timing (halve movement speeds, animation index
increments, physics, timers and camera rates). That is a game-specific ROM hack, not a generic
emulator change; it is not implemented here. `PS2_FORCE_WAKE=7` remains available for experiments
and is OFF by default.

## Opt-in 60 fps re-timing (Master Mode working) - 2026-10-27

`PS2_60FPS=1` enables an experimental 60 fps mode that keeps the game speed correct. It is OFF by
default; the default build remains byte-identical behaviour at 30 fps logic / 60 Hz output.

What the mode does:

1. Wakes the logic thread (7) every vblank instead of every second one (`kernel::force_wake`), so
   the scene updates once per field.
2. Halves the character animation step: the five `dt = 1.0` arguments in the per-object animation
   updater at `0x0076CEB0` (generated file `m_006C6D00_7AA46D27_unit_00A6.cpp`) read
   `gen::g_anim_dt_bits`, which is 0.5 in this mode. The animation frame counters now advance at
   the same rate as 30 fps (measured 59 after 120 vblanks, versus 58 at 30 fps and 118 without
   retiming).
3. Halves the movement/camera speed constants in the character parameter block at `0x0089A0A0`
   (`+0x20` 4.7 -> 2.35, `+0x24` 30.0 -> 15.0). The world position measured after a deterministic
   120-vblank walk becomes -5952 versus -6029 at 30 fps (the un-retimed 60 fps run gives -4202,
   i.e. exactly the 240-vblank/2x position).

Verified in Master Mode: scene/present 59.6-60.4 fps, CPU 180-205%, VU 60-75% busy, no missing or
bad code, and side-by-side screenshots after the same wall-clock walk show the same character
position and animation phase as 30 fps. Default mode still measures animation 58 / position -6029.

Still to retime for the whole game: per-character battle speeds (the params block is the current
character's), battle timers (e.g. the match clock, not reachable without a battle state), AI
decision intervals, UI/effect emitters that use other animation callers, and modules other than
`7AA46D27`. Backups: `/tmp/retime_backup` and `/tmp/fps60_backup2`.

### 60 fps stability fix (loads are not re-entrant)

Forcing the logic thread every vblank during scene loads and transitions made the engine run
non-re-entrant code twice, which corrupted an object pointer (observed as writes to
`0x59D8FEBC` from `0x001B8BFC` and a `break` at `0x00118B3C`). The 60 fps hook now checks
`ps2_scene_last_frame()` and only wakes thread 7 (and patches the character speed block) when the
scene updated within the last 2 vblanks. During loads the game stays at its native rate. Verified
with a 20 000-frame run (battles/attract mode, ~200 s at 60 fps) and a boot-to-menu run: no unmapped
writes, no break, scene 59.8-59.9 fps. The battle match timer still decreases at ~1 s/s.

### Speed reference: NTSC vs original PAL

The disc is the PAL release (SLES-556.05). The PAL build advances its frame delta at 1/25 s; running it with `--ntsc` makes the logic run 30 times per second, i.e. 20% faster than the
original PAL game. `PS2_60FPS=1` matches the existing 30 fps NTSC speed (deterministic walk:
position -4194 vs -4244 at 30 fps, 1.2% difference; a 2x run gives -9044). `PS2_60FPS_PAL=1`
runs 60 updates per second at the original PAL rate (animation step 25/60, speed constants
scaled by 25/60; walk position -4786, i.e. ~85% of the NTSC distance, matching 25/30).
A Master Mode save state is kept at `master_mode.ps2` in the project root.

Update: the parameter block at 0x0089A0A0 is an array of seven 0x64-byte character slots, not one
struct. Patching only the first slot retimed the player but left NPCs and battle characters at
double rate (the audio was always correct because the IOP runs in real time). The 60 fps modes now
patch all seven slots (speeds and the 1/25 frame delta) every vblank. A 10 s idle comparison of the
Master Mode shows NPCs and the player in identical positions at 30 and 60 fps.

Final speed-reference decision: the disc is the PAL release and the audio (IOP/SPU2) always runs at
real time, so forcing NTSC made the EE logic 20% faster than the music. `PS2_60FPS=1` now defaults
to the original PAL speed (25/60 per 60 Hz update, all seven character slots and the animation
advance scaled); `PS2_60FPS_NTSC=1` keeps the old 30 fps NTSC reference. Deterministic 240-vblank
walk: 30 fps NTSC -4244, 60 fps PAL -4786 (about 25/30 of the distance), 60 fps NTSC -4194.

### Headless scale test (important limitation)

Changing PS2_SPEED (the animation/global scale) moves the player but does not change the rendered
world at the same wall time: frames at 10 s are identical with scale 0.30 and 0.4167. The engine
rewrites its 1/25 frame delta from an overlay table during the scene update (no st32/st128 reaches
0x89A0D8, and the value is not in the ELF), so a global retiming of world/AI is not possible with
the data patches used here. `--pal` now overrides the state's PAL flag and runs the game natively
at 25 logic updates/s (scene 24.9, 50 Hz output), which is the original-speed reference.

### Default reverted to 30 fps NTSC

The 60 fps experiments are now OFF by default. Plain startup prints `[retime] mode 30 (scale
1.0000)`; the window title shows `retime 30`, scene runs at 29.9 fps with VU ~65% idle, and the
deterministic walk matches the old 30 fps reference (position -4244, animation 127.98). Opt in with
`PS2_60FPS=1` (PAL-speed 60 fps experiment), `PS2_60FPS_NTSC=1` (NTSC-speed experiment),
`PS2_SPEED=<scale>` (custom movement/animation scale) or `--pal` (native 25 fps PAL).

### Battle regression fixed (2026-10-28)

Battles were ending immediately in "Draw" with both health bars empty. Two changes from the
60 fps experiments were reverted: (1) the generated-code animation scale hook
(`fp::add(c.f[0], fp::mul(c.f[12], g_time_scale))`) in four generated units, which ran even at
scale 1.0, and (2) `--pal/--ntsc` overriding the video mode stored in a loaded save state. Default
is again plain 30 fps NTSC (`[retime] mode 30`, title `retime 30`, scene 29.9, 12.65 ms/frame in
Master Mode). The 60 fps experiments remain opt-in only.

### Shadow comparison (dojo vs village)

The dojo (Practice) shadow is a long soft projected silhouette; the village shadows are short and
hard. Both rasterizers (CPU and GPU) and the VU1 interpreter produce pixel-identical dojo frames,
so this is not a VU/GPU emulation discrepancy. The long shadow is consistent with the low-angle
indoor light and the second (elevated) character's shadow overlapping the area. A reference image
of the original dojo is needed to tell whether the game intends a shorter/harder shadow there.

### Round Sprite (PCSX2 hack) for FST sprites

Ported PCSX2's `RoundSpriteOffset` (used by the Naruto 5 "Round Sprite: Full" fix) to both the CPU
rasterizer and the WebGPU path (`round_sprite_offset` in `gs.hpp`). FST sprites have their texel
coordinates snapped to the GS 1/16 grid (nearest) or shifted by half a texel (bilinear), matching
the hardware for shadow/blob sprites. Enabled by default; `PS2_NO_ROUND_SPRITE=1` disables it.
Verified in the village and with the battle/attract state: no unmapped/break, scene 30 fps NTSC.

### Investigacao da sombra (GS dump do PCSX2)

Foi escrito um parser do GS dump (`dumpgs.gs.zst`): cabecalho, estado, regs privilegiados e o stream GIF
(concatenado por frame, com PACKED/ADLIST/PACKED-AD e imagens), reconstruindo a VRAM e o estado dos registradores.
Descobertas:

- O screenshot 640x480 embutido no dump mostra a cena correta do PCSX2 no mesmo local do bug (sem a sombra grande).
- A paleta das sombras (`CBP 0x3F8B`, `0x3FAD`, `0x3FB4`) e **byte-identica** entre o dump e o runtime.
- As texturas de sombra PSMT4 (TBP `0x3800` 256x128, `0x3A5E` 32x32, `0x3840` 128x128) tambem sao identicas.
- `TEST`, `ALPHA`, `TEX0`, `TEX1` dos draws compartilhados batem; logo o problema nao e decodificacao de CLUT nem alpha test/blend.
- A sombra projetada grande do nosso render vem de ~41 tristrips/frame com
  `TEX0 = 0x2007F165E1413800` (TBP `0x3800`, PSMT4 256x128, CBP `0x3F8B`, TCC=1, CLD=1),
  `TEST = 0x514EB` (ATE/GEQUAL, AREF 0x4E), `ALPHA = 0x44`, ABE=1. Pulando exatamente esses draws
  a sombra grande desaparece e sobram apenas as sombras pequenas (blobs).
- No dump do PCSX2, no local do bug, esses draws nao aparecem; o jogo usa/samples TBP `0x3800` como
  PSMCT32 256x256 (`TEX0 = 0x0000000220013800`, TCC=0) e possui passes que escrevem num framebuffer
  com `FBMSK = 0xFF000000` (somente alpha), tipico de shadow map renderizado para VRAM.

Bug real corrigido no caminho: a mascara do TEX2 incluia os bits 32-36 (TH, TCC, TFX), que nao fazem
parte do TEX2, permitindo que um write de TEX2 corrompesse esses campos. Nao alterou a cena deste jogo
(o jogo nao usa TEX2 nesses materiais), mas fica corrigido.

### Replay dos inputs do PCSX2 (botoes.txt)

`botoes.txt` e o log do pad do PCSX2 (estado por frame). Foi convertido para `--axis`/`--press`
(mapa: `Left: [x,y]` -> `--axis frame:lx:x` / `ly:y`, `L2` -> `--press frame:l2`), com
`frame = 15500 + amostra`. Com esse replay o runtime chega ao mesmo local do dump do GS
(ramen com a roda/banco) e o frame resultante bate com o screenshot embutido no dump
(brilho do chao 170.1 vs 169.3 do PCSX2).

Nessa comparacao foram verificados, material por material, e estao identicos ao PCSX2:
`TEX0`, `TEX1`, `TEST`, `ALPHA`, `TEXA`, `CLUT` (byte a byte: `0x3F8B`, `0x3FAD`, `0x3FB4`),
o conteudo das texturas PSMT4 (`0x3A5E` 32x32, `0x3840` 128x128, `0x3800` 256x128),
`AFAIL` (confirmado contra o renderizador de software do PCSX2: FB_ONLY escreve cor sem Z,
ZB_ONLY nao escreve cor, RGB_ONLY escreve RGB sem alpha), `PABE=0` e `FBMSK`.

Observacao: o dump do GS enviado esta com a camera mais afastada que as capturas
`com_bug.png`/`sem_bug.png` (altura do personagem 0.296 do frame vs 0.42), ou seja, nao e o
mesmo instante das capturas.

Diagnosticos novos (todos por env): `PS2_DRAW_RAW=*|tbp`, `PS2_DRAW_FRAME=n`,
`PS2_DUMP_TEX_AT=tbp,file`, `PS2_SKIP_TEXF=tex0`, `PS2_SKIP_TEST=test`,
`PS2_FRAME_RANGE=from,to`, `PS2_PIXEL=x,y`, `PS2_TEX0_TRACE=1`.

### Comparacao das novas capturas (poco/manhole)

`com_bug.png` e `sem_bug.png` novas (3840x2079) estao na MESMA posicao/camera
(personagem em cx 0.506 vs 0.493, y1 0.705 vs 0.696, altura 35.5% vs 34.6%).
Medicoes na mesma regiao:
- escuridao da sombra: 78.1 (nosso) vs 79.0 (PCSX2)
- fracao de chao abaixo de 110: 62% vs 65%
- areas iluminadas: 154 vs 151
Ou seja, nesta posicao a sombra casa em intensidade e extensao. O residual do diff
concentra-se nas BORDAS de toda a imagem (sharpness/filtragem), nao na sombra.

No PCSX2 a janela mostra "1024x1024 (2x)" de resolucao interna; o nosso `PS2_SCALE=2`
tambem rasteriza em 2x, mas os frame dumps (--frames-dir) saem em 512x512 nativo.

Tambem verificado: `DTHE=0` no dump (dithering desligado) e `DIMX` nunca escrito, logo
implementar dither nao mudaria nada neste jogo. `DTHE`/`DIMX` continuam nao implementados
(lacuna de fidelidade conhecida).

### Replay de GS dump do PCSX2 (`--gs-dump`)

Novo modo: le um dump `.gs` do PCSX2 (formato v2+ nao comprimido) e alimenta o nosso GS
com o stream GIF do dump, sem bootar o jogo:

```
zstd -d dumpgs.gs.zst -o dumpgs.gs
./build/native-naruto/ps2run --gs-dump dumpgs.gs --frames-dir /tmp/gsrep
```

- `runtime/src/gsdump.cpp`: parse do cabecalho (`fake_crc`, `header_size`, `GSDumpHeader`),
  pulo do savestate (state_size) + `GSPrivRegSet` (0x2000) e loop de pacotes:
  - tipo 0: `gif::transfer(path, data, bytes/16)`
  - tipo 1: vblank -> `gs::flush_and_wait()` + `gs::display()` + PPM por campo
  - tipo 2: VRAM->EE (dados nao existem no dump, ignorado)
  - tipo 3: snapshot do `GSPrivRegSet` -> `gs::write_privileged()` (PMODE/SMODE2/DISPFB/DISPLAY/BGCOLOR)
- A VRAM comeca zerada (texturas enviadas antes do inicio do dump faltam); neste jogo quase
  tudo e reenviado por frame, entao a imagem sai completa.

Validacao: o frame replayado bate com o screenshot que o PCSX2 embute no proprio dump
(RMSE ~8%, diferencas so de borda/escala). Em particular a **sombra do personagem e a sombra
do cenario ficam iguais as do PCSX2** com os MESMOS draws -- ou seja, o renderizador (CLUT,
alpha test/blend, TEX0/TEX1, FBMSK) esta fiel. Se a sombra diferir com o jogo rodando, a
diferenca vem do **stream de draws** (estado do jogo), nao do desenho.

### Resolucao de saida (`--resolution`) e filtro do blit

- `--resolution 720p|1080p|4k|WxH`: abre a janela exatamente nesse tamanho e escolhe o
  supersampling interno automaticamente (720p -> 2x, 1080p -> 3x, 4K -> 6x).
  `PS2_SCALE` continua mandando quando definido; o limite subiu de 3 para 6.
- Blit de apresentacao (`blit.wgsl`): a magnificacao continua "sharp bilinear"; a
  **minificacao** agora usa um box filter de 16 taps sobre o footprint (`size/out`), que
  elimina o moire/"scanline" que aparecia ao reduzir 1024/1536/3072 -> 720/1080/2160.
- Medido: 720p/1080p/4K rodam a 30 fps; RAM 244/239/274 MB; GPU 3/5/10%.
- Comparacao objetiva com a captura do PCSX2 no mesmo ponto (ruido horizontal por pixel):
  14.81 (nosso 720p) vs 14.68 (PCSX2) -- mesma textura/dither do jogo.

### Nitidez da apresentacao (720p/1080p/4K)

Tres causas do aspecto "borrado/sobreposto":

1. **Filtro do blit aplicado nos dois eixos**: o box filter de minificacao tambem era usado
   no eixo que estava sendo *ampliado*, borrando a imagem. Agora o filtro e por eixo:
   eixo ampliado = sharp bilinear (pixel nitido), eixo reduzido = media do footprint.
2. **Supersampling acima da saida**: em 4K o runtime rasterizava 3072 e reduzia para 2160
   (SSAA), o que suaviza tudo. Agora a escala interna acompanha a saida:
   720p -> 1x, 1080p -> 2x, 4K -> 4x (ampliacao nítida, como a resolucao interna do PCSX2).
3. **Sprites 1:1 (HUD, texto, icones)**: com escala interna > 1 o bilinear espalhava cada
   texel por SCALE pixels. Agora, quando a textura mapeia 1:1 no framebuffer, a amostragem
   e point (replica o texel), igual ao hardware em resolucao nativa.

Tambem: `sharp bilinear` passou a usar a escala fracionaria (1/footprint) em vez de
`floor`, o que mantem a nitidez em ampliacoes nao inteiras (ex.: 2048 -> 3840).

Observacao: o jogo pede uma area de display **512x512** (DISPLAY DW=DH=511, SMODE2=0,
interlaced). Com `--widescreen` a apresentacao estica para 16:9; sem ele, 4:3. Comparar
nosso print com um do PCSX2 no *mesmo* aspect ratio.

### Nitidez: causa raiz e ajuste final

1. A janela desenhava em **2186x1155** (fullscreen-desktop com escala HiDPI): o compositor ampliava
   nosso buffer para 4K. Agora `--fullscreen --resolution WxH` pede o modo **exclusivo** exato e o
   log mostra `[window] driver ..., drawable 3840x2160`.
2. A escala interna passou a ser derivada da **largura do drawable**: `ceil(out_width/512)` limitada
   a 5 (720p -> 3, 1080p -> 4, 4K -> 5), que mantem 30 fps. `PS2_SCALE` (1..8) sobrescreve.
3. O upscale de apresentacao virou **bicubica Catmull-Rom** (era sharp-bilinear), o que define as
   bordas na ampliacao; a minificacao continua com box filter de 16 taps.

Medicao (gradiente medio na mesma regiao, 4K):
- antes: x 1.55 / y 6.35   (referencia PCSX2: x 1.48 / y 6.33)
- agora: x 2.28 / y 6.06   -> ~54% mais detalhe horizontal que a captura do PCSX2.
Custo: 4K com escala 5 = 30 fps; escala 6 = ~28 fps; escala 8 = ~17 fps (GPU).

### Round Sprite desligado por padrao + apresentacao de campo (bob)

- O hack Round Sprite (PCSX2) estava **piorando** sprites no nosso runtime: o deslocamento de
  meio texel com amostragem bilinear transformava a borda de sprites (HUD, "hub" do mapa) numa
  copia deslocada. Agora e **opt-in**: `PS2_ROUND_SPRITE=1`. Padrao = desligado.
- O jogo e **entrelacado** (SMODE2.INT=0, DISPLAY 512x512). A apresentacao mostrava o framebuffer
  inteiro (os dois campos), o que duplicava tudo que muda por campo (efeitos a 60 Hz, como o hub
  do mapa) -- "imagem sobreposta". Agora, quando INT=0, apresentamos **um campo** (bob:
  `src_line = (y & ~1) + field`), tambem no caminho threaded (`present(regs, mode)`), que era o
  unico ativo. `PS2_NO_DEINTERLACE=1` volta ao comportamento antigo (weave).
- Observacao: bob usa metade das linhas (duplica cada uma), entao perde um pouco de resolucao
  vertical -- e o que um TV entrelacado mostra por campo. Sem ghosting.

### Correcoes da apresentacao (rodada final)

- `sharp bilinear` so afia em **escalas inteiras** (`floor(1/fp)`); com fator fracionario
  (ex.: 2560 -> 3840 = 1.5x) ele mapeava a amostra no texel vizinho e **rasgava detalhes finos**
  (a barra vermelha do hub virava 3 listras). Era um bug meu, agora corrigido.
- Bicubica removida (o overshoot criava bandas em sprites pequenos de alto contraste).
- Desentrelacamento: padrao = **bob com campo fixo** (sem fantasma e sem salto). Opcoes:
  `PS2_DEINT=weave` (framebuffer inteiro, resolucao cheia, pode duplicar efeitos desenhados
  nos dois campos) e `PS2_DEINT=blend` (media dos dois campos).
- Round Sprite continua **desligado por padrao** (`PS2_ROUND_SPRITE=1` liga).

### Validacao com o dump do GS no ponto do "hub" (12:07)

Com o dump novo (`dumpgs.zst`, cena do poco/hub):
1. Replay dos MESMOS draws do PCSX2 no nosso GS -> identico ao screenshot embutido no dump
   (salvo em `hub_replay_vs_pcsx2.png`). Renderer fiel (CLUT/alpha/TEX0/TEX1/FBMSK/bob).
2. Jogo rodando (build atual, PS2_SCALE=5, Round Sprite off, bob) na mesma posicao ->
   tambem identico a referencia: hub unico, barra vermelha solida
   (`hub_live_vs_pcsx2.png`).
RMSE live-vs-dump-screenshot: 0.104 (diferenca so de nitidez/escala; a captura 4K foi
reduzida para 640x360 e a referencia e o screenshot 2x do PCSX2).

### Imagem "duplicada e deslocada" no hub: causa real (29/10)

Reproduzido de forma deterministica com o dump do PCSX2, sem depender de screenshot:

    PS2_SCALE=1 ps2run --gs-dump gs2.gs --frames-dir /tmp/v1
    PS2_SCALE=5 ps2run --gs-dump gs2.gs --frames-dir /tmp/w5

- Em 1x a barra vermelha do painel do mapa ocupa 2 linhas nativas (206,11,31 e 214,8,27),
  identica ao screenshot que o PCSX2 grava no dump.
- Em 5x a MESMA barra aparecia com ~16 sub-linhas, com dois picos e vales escuros
  (padrao com periodo = SCALE): ou seja, cada texel era lido duas vezes, deslocado.
- Causa: a UV da textura era interpolada na posicao do **sub-pixel** (px,py). O GS amostra
  a textura **uma vez por pixel nativo**; variar a UV dentro do pixel nativo faz o filtro
  varrer o par de texels uma vez por pixel, e em SCALE>1 isso aparece como uma segunda
  copia deslocada de cada texel. Os icones do mapa (os "coins") mostravam a linha escura
  no meio exatamente por isso.
- Correcao: textura amostrada na grade nativa, exceto quando a textura esta **ampliada**
  (ai a resolucao interna extra realmente rende). Aplicado nos dois caminhos (triangulos e
  sprites) via o gradiente du/dx, dv/dy.
- Extra: a posicao de amostragem passou a ser o **centro** do sub-pixel
  (px = x + (sub+0.5)/SCALE). Isso aproxima mais do PCSX2: o replay 1x contra o screenshot
  do dump melhorou de RMSE 0.0723 para 0.0666.

### Por que o PCSX2 parecia "perfeito e suave"

O OSD do print do PCSX2 diz `Vulkan 3072x3072 (6x)` -- ele roda com **resolucao interna 6x**,
ou seja, faz upsampling de textura em 6x. Isso deixa os icones do mapa redondos e nitidos.
Nosso 5x agora mantem a textura na resolucao nativa (fiel ao PS2) e usa a resolucao extra
apenas em geometria e em texturas ampliadas.

### A CAUSA REAL do "hub duplicado e deslocado" (fim do dia 29/10)

Medicao deterministica no dump (`gs2.gs`), sem depender de screenshot:

- A barra vermelha do painel do mapa, em 1x, ocupa 2 linhas nativas: (206,11,31) e (214,8,27).
  Esse par e o MESMO elemento desenhado **uma vez por campo** (o jogo escreve em 60 Hz e os
  campos ficam entrelacados **meia linha** um do outro: o GS tem 4 bits fracionarios, entao o
  segundo campo e escrito em y+0.5).
- Em 1x as duas copias caem no mesmo pixel e somem. Em 5x (2500 linhas) elas ficam a 2.5
  sub-linhas uma da outra -> cada icone/barra/borda aparece DUAS vezes, deslocado meia linha.
  Era exatamente o "duplicada e deslocada" do hub.
- PCSX2 nao mostra porque o sinal de video do PS2 (e o proprio PCSX2) nunca resolve abaixo de
  uma linha nativa.

Correcao (em `blit.wgsl`, na apresentacao -- nao no raster):

- Media vertical de ~0.375 de linha nativa (2 amostras + bilinear), **so no eixo vertical**.
  O eixo horizontal continua nitido, entao a resolucao interna extra continua valendo.
- Ajuste por env: `PS2_PRESENT_VBLUR=0.375` volta ao desfoque vertical, `0` desliga, `0.5` suaviza mais.
  O **padrao passou a ser 0** (pedido para preservar a nitidez); o efeito de meia linha volta
  a aparecer no hub se o desfoque for desligado.
- `PS2_SCALE` volta a amostrar textura na posicao do sub-pixel (foi o que deixou a imagem
  "low-res"/blocada quando eu tentei amostrar na grade nativa -- revertido).

### Custo

4K + SCALE=5: 30.0 fps, GPU 6%, CPU ~130-141% (o blit passou de 4 para 8 taps).

### "Tudo pixelado" em 4K: point sampling forcado (30/10)

- O caminho de sprite forcava **point sampling** em duas situacoes, ignorando o TEX1.LCM do
  jogo: (a) sprites cuja textura mapeia 1:1 no framebuffer, (b) qualquer textura em formato
  "direct" (PSMCT32/24/16 = muito comum: HUD, painel do mapa, texto).
- Em 1x isso e' invisivel (bilinear num mapeamento 1:1 quase nao muda nada), mas ao ampliar
  5-6x cada texel vira um bloco de 7-8 px = "tudo pixelado".
- O PCSX2 (6x) respeita o filtro do jogo, por isso e' suave. Agora tambem respeitamos:
  `point = TME && !BILINEAR` e nada mais.

### --resolution como unico parametro

- `--resolution` agora implica `--fullscreen` (sem ele, janela nao pode passar da area util).
- A escala interna segue a resolucao: 720p->3x(1536), 1080p->4x(2048), 1440p->5x(2560),
  4k->6x(3072). Teto 6x porque 7x=23 fps e 8x=17 fps em 4K; PS2_SCALE continua como override.
- 4K medido: 29.6-29.8 fps, GPU 18%, CPU ~131%.
- `PS2_PRESENT_VBLUR` agora tem padrao **0** (nitidez). Com 0 o entrelacamento de meia linha
  dos icones do mapa volta a ficar "duplicado"; `PS2_PRESENT_VBLUR=0.375` restaura o desfoque
  vertical antigo sem mexer no resto. As linhas verticais dos FMVs NAO eram isso (ver secao
  "Widescreen nativo e FMVs").

## ETAPA E/F (nov/2026) — investigacao do efeito de save e do fogo

Contexto: comparacoes contra o PCSX2 e contra o modo `sub`/`native` do recomp, com
savestates proprios (`master_mode.ps2` = efeito de save; `batalha_fogo.ps2` = cena de
fogo, carrega no frame 18500). Todas as instrumentacoes sao opt-in (padrao OFF) e sem
variavel o hash segue `b0fd15f1…` (master) / `6ca501c7…` (batalha, frame 18534).

### T0 — grid sub x native

- Em 1x `sub` e `native` sao **byte-identicos** (`13ae6415…`). A diferenca so existe em
  escala > 1 (~4% de gradiente em 6x, mais duro no azul).
- Log de escala na inicializacao:
  `[gpu] window|headless scale <S> internal <W>x<H> [surface <W>x<H>]`.
  Headless 4k auto = **scale 6 (3072²)**; janela 4k auto = **scale 5 (2560²)** com surface
  2194x1160 — `--resolution 4k` nao e aplicado na janela (wgpu limita a superficie).

### Dump/log de draws (opt-in)

`PS2_DRAW_LOG=<frame>` grava `evidencias/ETAPA_E/T1/draws_<frame>.tsv` com: idx (ordem de
commit), prim, fst, tme, tbp0/tbw/psm/tw/th, TEX1 decodificado (mmag/mmin/lcm/l/k),
clamp, fbp/fbw/fbpsm, abe/alpha, bbox, uv_min/max, dudx/dvdy, flags, in_g_rendered,
fonte (vram/decoded), last_writer, uniform_pct, f_bilinear, sub_grid, flags_final,
step_native.

- O GS desenha a cada **dois** frames; o par/ímpar depende do savestate (master: par;
  batalha: ímpar). `frame_<N>.ppm` e um dos campos.
- `in_g_rendered` e por **pagina de 8 KiB** (`(addr>>13)&511`), marcada no flush pelo bbox
  do destino, e **por intervalo** `[lo,hi)` — nao pela lista real de paginas. Isso
  supermarca paginas (ex.: 8 tiras que escrevem paginas alternadas marcam o retangulo
  inteiro) e foi a causa de `last_writer=draw:7` em texturas que o batch nao escreveu.
- `PS2_WRITER_EXACT=1` marca so as linhas/colunas de pagina do bbox; nao mudou **nenhum**
  draw nos dois logs (a uniao das 8 tiras cobre o retangulo todo). Mas em 15501 o resultado
  difere do writer (54% dos pixels) — transiente dos primeiros frames, nao explicado.

### O efeito de save (master_mode)

- As "faixas" e o "anel" sao **render targets** dentro do RT 256x256 em `fbp=14336`
  (paginas 448-463). O draw 18850 e um **overlay sem textura (TME=0)** que cobre o RT
  inteiro e por isso vira `last_writer` — nao desenha o feixe.
- Uniformidade SxS: 14416 (anel) **0%** (detalhe real em 6x); 14450 (faixas) **100%**
  (conteudo replicado). Uploads/savestate replicam SxS (`write_pixel`), entao o bilinear
  de passo sub-texel nao mistura nada (rampa de 1 px) — e essa a razao do efeito "duro".
- O texto "START button : Open Menu" e **copia 1:1 do framebuffer** (tbp 0 -> fbp 3584).
  A fonte foi desenhada antes do savestate: `SKIP/ONLY` nos idx 3-6 nao remove o texto.
- `PS2_BILIN_STEP_TBP=<lista>` aplica o passo nativo por TBP; `PS2_BILIN_STEP=writer` usa o
  criterio de escritor; as duas variaveis **se somam** (uniao).
- `PS2_BILIN_STEP_MAG`/`uniform` foram avaliados e **nao implementados**: o criterio por
  magnificacao pega o fogo mas tambem 11-37% da tela; o criterio por uniformidade nao
  reproduz a uniao (differe em ~800-6000 draws).

### PCSX2 como referencia

- Captura interna 3072² exige `ScreenshotSize = 2` (Internal, Uncorrected). Os valores
  1/3 dao 4096x3072 (aspect corrected, reamostrado) — inutilizaveis para 1:1.
- **O gate estava em `filter=0` (texture filtering Nearest)**: com nearest o PCSX2 parece
  igual/mais duro; com **`filter=1` (Bilinear PS2)** e na mesma escala ele e mais suave:
  |grad| 1,93/2,58/2,85 (faixas/azul/hud) contra 2,60/3,04/3,83 do recomp.
- GameDB (SLES-55605): `halfPixelOffset: 2` (Aligns post effects), `nativeScaling: 2`
  (Fixes post effects), VU clamps 3, SoftwareRendererFMVHack.
- A/B `nativeScaling` (GameDB 2 x manual 0): **desprezivel** — anel 2,20 x 2,23, HUD
  2,97 x 2,99; `halfPixelOffset` 0 x 2 deu **hash identico**.
- Anti-Blur (`pcrtc_antiblur`): no PCSX2 (`GSState.cpp` ~7406-7640) ele **nao filtra** —
  ajusta os offsets/rects dos dois circuitos PCRTC (X com |Δ|=1; Y com |Δ|<=1 e DBY
  alternando) para o merge nao borrar. ON x OFF no mesmo frame: Y/X sobe (HUD 0,918 ->
  1,006; cenario 1,249 -> 1,412; anel 1,287 -> 1,490) e o deslocamento subpixel medido e
  dy ~ +0,37 px.

### Semantica de SMODE2 e o INT invertido

- `pcsx2/GS/GSRegs.h`: `SMODE2 { INT:1; FFMD:1; DPMS:2; }`.
  `GSState.cpp:isinterlaced() { return !!m_regs->SMODE2.INT; }` → **INT=1 = entrelacado**;
  FFMD=1 e' "half frames"/field mode (usado em `GSRenderer.cpp:165/187`).
- O recomp faz `interlaced = (smode2 & 1) == 0` → **inverte o INT**. Valores reais no jogo:
  SMODE2=0x1 (INT=1), PMODE=0x7F67 (circuito 1), DISPFB=0x1070 (fbp 3584 — o display le
  3584, nao o fbp 0), DISPLAY DY 73/72, MAGH 5, MAGV 1.
- `PS2_INT_PCSX2=1` (opt-in) passa a usar a leitura do PCSX2: com INT=1 o padrao vira bob
  (Y do HUD 6,29 -> 8,95) e fica igual ao blend (os dois campos deste frame sao quase iguais).

### Apresentacao (blit da janela)

- `blit.wgsl` le por **storage buffer** (sem sampler), 8 taps em `texel_linear` (sharp
  bilinear em X; box no eixo minificado) + **desfoque vertical** `vblur` com 2 taps
  simetricos `t.y ± dy` (deslocamento liquido **zero**).
  `vert_blur()` default **0** (`PS2_PRESENT_VBLUR`; 0.375 restaura o desfoque antigo, 0.5 suaviza mais).
  `PS2_PRESENT_NEAREST=1` forca nearest.
- Janela 2194x1160 (viewport 16:9 = 2062x1160): com scale 6 cada pixel de saida cobre
  ~1,49 colunas e ~2,65 linhas; sem `PS2_SCALE` (5x) ~1,24 e ~2,21.
- Teste na janela (X11, `import -window`): `VBLUR=0` sobe o gradiente vertical do HUD de
  10,49 para 12,43 (+18%); com `writer + lista` fica 11,46 (os dois efeitos se compensam).
- O mangohud contamina o canto do HUD — capturas de medicao devem ser sem ele.

### Cena do fogo (batalha_fogo)

- Grupo de draws do fogo: idx 18343-18403 (59 draws, texturizados, bbox na chama).
  Texturas: 5344 (atlas cinza, 256x118 util), 4448 (atlas laranja, 120x114), 3744/9376
  (atlas de bolas de fogo), 15360 (no **momento do draw** e' fogo; no fim do frame ja foi
  sobrescrito por outra coisa).
- As texturas do fogo moram dentro do buffer copiado pelas **8 tiras (idx 0-7)**:
  copia FB(tbp 0) -> fbp 3584, palavras 229.376-491.519; 4448 na palavra 284.672 (tira 3)
  e 5344 em 342.016 (tira 7). Dump com e sem `PS2_DRAW_SKIP=0-7` deu **identico** — o
  `last_writer=draw:7` vinha da marcacao por intervalo, nao de escrita real.
- TEX1 dos draws do fogo: `0x00000F1800000160` → LCM 0, MXL 0, MMAG 3, MMIN 1, MTBA 0,
  L 0, K 0. `make_setup` usa so o bit 5 do MMAG para `F_BILINEAR`; MMIN/L/K/MXL sao
  ignorados (sem mipmaps). MMAG=2 (10b) cairia como nearest por causa do teste de 1 bit.
- `PS2_BILIN_STEP_TBP` nos TBPs do fogo: rampas nos degraus 2→4 px, mas o 15360 (textura de
  tela cheia) e' quem realmente suaviza — e e' o que estoura o fora (3,6% -> 40,4%).
- Passo do texel na tela (6x): 5344 ~32x38 px, 4448 ~14x16, 15360 ~19x22, 3744 ~48x56.

### Instrumentacoes de diagnostico (todas opt-in, padrao OFF)

- `PS2_DRAW_LOG=<frame>`, `PS2_DRAW_SKIP/ONLY`, `PS2_BILIN_STEP_TBP`, `PS2_BILIN_STEP=writer`,
  `PS2_BILIN_EXCLUDE`, `PS2_PAGE_DUMP`, `PS2_WRITER_EXACT`.
- `PS2_DUMP_VRAM_TEX=<tbp,...>` + `PS2_DUMP_VRAM_FRAME`: dump nativo (CLUT), grade 6x e
  recorte UV (PPM; convertido a PNG/4x fora).
- `PS2_DUMP_VRAM_FB=<fbp>`: framebuffer 6x (3072²) em PPM.
- `PS2_DUMP_VRAM_AT_DRAW=<idx>`: readback da textura imediatamente antes do draw.
- `PS2_DEBUG_STEP_TINT=1`: 25% magenta nos draws com passo nativo (diagnostico de cobertura).
- `PS2_INT_PCSX2=1`: INT=1 -> entrelacado (leitura PCSX2).
- `PS2_DISPLAY_LOG=1`: imprime PMODE, SMODE2 (INT/FFMD), DISPFB1/2, DISPLAY1/2 (DH/DV/MAGH/MAGV).

### Estado dos dados

- Evidencias em `evidencias/ETAPA_E/` (T0, T1: draws/TSVs, relatorios) e
  `evidencias/ETAPA_F/` (batalha_fogo: dumps de textura, crops, capturas de janela,
  RELATORIO*.md). `PROGRESSO.md` tem data/comando/hash de cada rodada.
- C35 (`first = floor(lo)`), Etapa A, B e D e RT_NATIVE ficaram intactos.

## Widescreen nativo, formato de tela e linhas dos FMVs (01/11/2026)

### `--patches` aceita os formatos do PCSX2

`load_patches` (runtime/src/system.cpp) passou a ler, na mesma lista:

- formato do recomp: `place endereco bytes valor` (place **0 = boot**, **1 = todo vblank**);
- pnach: `patch=<place>,EE,<endereco>,<word|short|byte|double|extended>,<valor>`;
- cheat do PCSX2 (.cht): `20b3ef14 3f400000` (o nibble alto do endereco e' o tamanho:
  0/1/2/3 -> 1/2/4/8 bytes). Mastercodes (nibble 8+) e condicionais sao ignorados.

Place vem da semantica do PCSX2: 0 = so no boot, 1 = continuo (todo vblank), 2 = os dois.
`PS2_PATCH_LOG=1` imprime o que foi carregado e as 3 primeiras aplicacoes com o valor lido
de volta da memoria.

**Importante para savestates:** um patch de place 0 e' aplicado no boot, mas
`--load-state` restaura a RAM inteira depois e desfaz o valor. O cheat de widescreen precisa
de **place 1**:

```
widescreen_recomp.txt:  1 00b3ef14 4 3f400000
```

(`widescreen.cht` e `games/naruto/patches/patches.txt` tambem funcionam direto agora: o
segundo usa `patch=1,EE,20B3EF14,extended,3F400000`.)

### FOV de 16:9/21:9 sem esticar a geometria

O patch escreve o eixo X da projecao em `0x00B3EF14`. Valor = `(4/3) / aspecto_desejado`:

| alvo | valor float | bits | arquivo |
|---|---|---|---|
| 4:3 | 1.0 | 3F800000 | (sem patch) |
| 16:9 | 0.75 | 3F400000 | `widescreen_recomp.txt` |
| 21:9 | 0.5714286 | 3F124925 | `widescreen_21x9.txt` |
| 32:9 | 0.375 | 3EC00000 | `widescreen_32x9.txt` |

Usar junto com `--widescreen` (apresentacao 16:9) ou `PS2_PRESENT_AR`:

```bash
ps2run ... --widescreen --patches widescreen_recomp.txt      # 16:9
ps2run ... --widescreen --patches widescreen_21x9.txt --resolution 1680x720 \
       PS2_PRESENT_AR=21:9                                   # 21:9
```

Medido no frame 15520 (master_mode.ps2, 720p): o FOV abre de verdade (mais mundo nas
laterais, Naruto menor) e, na apresentacao 16:9/21:9, as proporcoes voltam ao normal.
Sem o patch, 16:9/21:9 esticam a imagem 4:3. Com o patch, so o HUD/texto 2D continua
esticado pelo formato (e' a limitacao do hack do ElHecht; o mundo 3D fica correto).

### `PS2_PRESENT_AR` agora muda o viewport (nao so o filtro)

Antes o aspecto era calculado em `present()` apenas para o footprint do filtro; o viewport
real do passe de blit era fixo (16:9 com `--widescreen`, senao 4:3). Agora o `submit()` usa
`g_present_vw/g_present_vh` escolhidos em `present()`:

- `4:3` -> pillarbox real (1280x720 -> conteudo 960x720, barras de 160 px);
- `16:9`, `21:9` -> preenche a janela (respeitando o formato);
- `fit` -> aspecto da fonte (512x512 -> quadrado, com barras).

Padrao inalterado: `--widescreen` -> 16:9 esticado; sem ele -> 4:3.

### Linhas verticais nos FMVs: causa e correcao

Sintoma reproduzido de forma deterministica no filme do inicio (`--press 700:start+cross:60`,
frame 3200): linhas verticais a cada 64 px nativos (no interno 2x: x = 640, 768, 896...).

O `PS2_DRAW_LOG=3200` mostra o layout do player de video:

```
idx 9..16  prim 6  fst 1  tbp 15360  tbw 1  psm 00  tw 64  th 512
bbox 0..63,64..127,...  uv 0..63/0..511  dudx 1.0  dvdy 1.0  last_writer upload
```

Sao **8 tiras de 64x512 PSMCT32**, desenhadas 1:1 como sprites. A lista de TBPs do fogo e o
criterio `writer` aplicavam o passo nativo (4 taps a 1 texel) nessas tiras; como cada tira e'
uma textura de 64 px com clamp/repeat, o tap da borda puxa a coluna do outro lado e cada
fronteira vira uma linha.

Correcao em `gpu.cpp`: nao aplicar o passo nativo (lista **nem** writer) quando o draw e'
exatamente esse layout de video:

```cpp
const bool video_strip = fst && prim == 6 && tbw == 1 && psm == 0
                         && tw_exp == 6 && th_exp == 9; // 64 x 512
```

Resultado (frame 3200): gradiente medio entre colunas nos multiplos de 64 cai de
**1,94** para **0,37** (sem passo nativo: 0,20); as linhas somem e os hashes do fogo
(`c1e38e6c`) e do master (`efe34dda`) ficam **identicos** ao padrao anterior.

Evidencias: `evidencias/ETAPA_F/fmv_lines_before_after.png` e
`fmv_lines_crop_before_after.png`.

### HUD adaptado ao formato de tela (sem esticar)

O HUD do jogo e' desenhado em coordenadas de tela e a apresentacao wide esticava tudo
horizontalmente. A correcao separa **arte 2D** (texturas decodificadas, `in_g=0`) de
**UI renderizada em textura** (retrato/engrenagem, `in_g=1`): a primeira e' pre-escalada
em X por `(4/3)/aspecto` (0,75 em 16:9, 0,571 em 21:9); a segunda fica **sem escala**,
porque o jogo ja' a desenha na proporcao certa. A ancora vem do scissor/da regiao:

- scissor na metade esquerda -> ancora na borda esquerda (engrenagem/retrato/barra);
- scissor na metade direita -> ancora na borda direita;
- scissor de largura total -> ancora no centro (texto "START button : Open Menu").

Assim o icone fica na borda, o texto continua centralizado e **a forma nao distorce**
(um circulo continua circulo). A altura nao muda, entao a legibilidade se mantem.

Filtros para nao tocar no mundo:

- `prim == 3/4/5/6` (triangulos e sprites) com `xyoff == 0` e `in_g == 0` — pega HUD 2D,
  combo "12 hits", texto central, abas e a roda de icones do menu Master Mode;
- a **barra de vida** (`tbp 14928/14944`) e o **minimapa** (`tbp 15200/15072/15424/15616`
  com `q` entre 2e-5 e 1e-4 e bbox `x >= 420`) sao desenhados pela camera de UI 3D
  (`xyoff 0x7000`); entram por essa lista explicita, sem tocar no mundo;
- bbox pequena e dentro da tela (nao e' um passe de tela cheia);
- textura CLUT (PSMT4/8) **ou** no maximo 128x128;
- nunca as tiras de video do FMV (64x512) nem o anel do efeito de save (256x256).

O bbox **original** continua sendo usado para o batching/marcacao de paginas; so a
geometria desenhada e' corrigida. Com isso o mundo nao muda (4:3 idêntico; no 16:9/21:9
os efeitos que amostram o framebuffer, como o feixe, podem variar 1-2 niveis).

Ativar/desativar: padrao **ligado** em aspect > 4:3; `PS2_HUD_ASPECT=0` desliga,
`PS2_HUD_DEBUG=1` mostra os 12 primeiros draws corrigidos.

Evidencias: `hud_aspect_16x9_off/on.png`,
`hud_aspect_16x9_compare_top/bottom.png`, `hud_aspect_21x9.png`.

### PAL -> NTSC nativo (PALtoNTSC.pnach)

O disco e' PAL (SLES-556.05, `VMODE = PAL`), mas o runner ja roda o vblank a 60 Hz
(`--ntsc`, padrao). O que faltava era o **codigo/tabelas internas** do jogo: o pnach
`PALtoNTSC.pnach` tem 10070 linhas `patch=`, todas dentro do segmento executavel
(0x00100000..0x00627A80); 761 delas sao floats de tempo/velocidade e o resto sao
contadores de frames, tabelas de animacao e `lui` com constantes (ex.: 3.6 -> 3.0 em
`0x0022233C`).

Como o recomp e' AOT, escrever esses bytes na RAM em runtime **nao** muda o codigo nativo.
O caminho correto e' aplicar o pnach no ELF antes de recompilar:

```bash
scripts/port.sh naruto games/naruto/disc.iso --native-only --no-explore \
  --pnach-section "NTSC Video Mode" --pnach PALtoNTSC.pnach
```

O `scripts/pnach.py` grava o ELF patchado em `games/naruto/patched/SLES_556.05` e a lista
`games/naruto/patches.txt`. O `eerecomp` traduz o ELF patchado, entao as constantes NTSC
entram no C++ gerado (verificado: `L_0022233C` passou a `0x40400000`).

Em runtime, `--patches games/naruto/patches.txt` reaplica as 10070 escritas a cada vblank.
Isso e' o que faz um **savestate PAL** voltar ao NTSC: o state restaura as tabelas antigas
e o patch as corrige no proximo vblank. Para widescreen, repita a opcao:

```bash
build/native-naruto/ps2run games/naruto/patched/SLES_556.05 \
  --disc games/naruto/disc.iso --memcard memcard.ps2 --widescreen \
  --patches games/naruto/patches.txt --patches widescreen_recomp.txt \
  --load-state master_mode.ps2 --resolution 720p --filter bilinear
```

O `SYSTEM.CNF` continua dizendo PAL, mas o runner usa `--ntsc`/60 Hz; `--pal` forca 50 Hz.
O patch nao mexe em `0x00B3EF14`, entao widescreen e NTSC sao independentes.

### Saves antigos, HUD "baked" e o estado NTSC

O HUD do jogo nao e' redesenhado todo frame: ele fica gravado no framebuffer do savestate.
Um save PAL (`master_mode.ps2`) carrega esse HUD **ja esticado** e o runtime nao consegue
corrigir pixels antigos — so draws novos. Por isso, abrir/fechar o menu corrigia o HUD, mas
carregar o save antigo mostrava o HUD esticado de novo.

Foi gerado `master_mode_ntsc.ps2` (frame 15780): carrega o save antigo, abre/fecha o menu
para forcar o redesenho do HUD e salva o estado novo. O `run_game.sh` passou a usar esse
state e o `PS2_PRESENT_DUMP` confirma o HUD correto na apresentacao 16:9.

`PS2_PRESENT_DUMP=<arquivo.ppm>` (+ `PS2_PRESENT_DUMP_FRAME=<frame>`) renderiza o passe de
apresentacao para uma textura offscreen e grava PPM. E' a unica captura fiel do WebGPU
(o `import` do X11 pega o primeiro frame e o `x11grab` sai preto).

### Coloração vermelha no Master Mode: verificacao

Comparacoes deterministicas PAL (binario antigo, ELF original) x NTSC (ELF patchado +
`games/naruto/patches.txt`) no mesmo frame:

| cena | resultado |
|---|---|
| master_mode, frame 15520/15550/15600 | **byte-identico** |
| menu START (roda de icones) | **byte-identico** |
| Status (tela com cabecalho vermelho) | **byte-identico** (`fe59e840…`) |
| shop_shizune | **byte-identico** (`bc2289a7…`) |
| batalha, frame 18550 | **byte-identico** |
| batalha, frame 18700 | difere (timing/animation NTSC, sem vermelho) |

O `fe59e840…`/`bc2289a7…` mostra que o vermelho do cabecalho do Status e do shop e' arte
original, nao o patch. A media RGB das cenas fica em torno de 0.40/0.39/0.29 (marrom quente
do cenario), sem dominancia do canal R.

Duas escritas do pnach eram suspeitas e foram removidas:

| endereco | original (dado) | o que o pnach escrevia |
|---|---|---|
| `0x00504580` | `00000012` | `08000012` (`j 0x48`) |
| `0x0059A6AC` | `00000001` no save / `08000012` no ELF | `08000001` |

Elas nao sao codigo com code cave: caem numa tabela de dados (os words vizinhos sao floats
`0x3C75C28F`, `0x3F800000`). O `j 0x48` sobrescrevia `0x12` por um opcode de salto, podendo
corromper a tabela lida pelo Master Mode. O runtime passou a usar a lista sem essas duas
linhas (`games/naruto/patches.txt`, 10068 patches), e o ELF patchado foi regerado com
`PALtoNTSC_fixed.pnach`. O estado novo `master_mode_ntsc.ps2` foi salvo com essa lista e
tem `0x00504580 = 00000012` de volta.

Se a coloracao aparecer em uma cena especifica, rodar com
`PS2_PRESENT_DUMP=/tmp/tela.ppm` e comparar; o dump mostra o frame exato.

### Testando proporcoes de tela manualmente

Variaveis novas (todas opt-in):

| variavel | efeito |
|---|---|
| `PS2_PRESENT_AR=16:9` | viewport 16:9 + FOV/HUD automaticos |
| `PS2_PRESENT_AR=21:9` | viewport 21:9 (letterbox na janela) |
| `PS2_PRESENT_AR=2560:1080` | qualquer par W:H; tambem aceita float (`1.891`) |
| `PS2_FOV_ASPECT=21:9` | forca so o FOV do mundo para esse aspecto |
| `PS2_FOV_SCALE=0.7049` | forca o valor float do eixo X (0xB3EF14) |
| `PS2_HUD_SCALE=0.70` | forca o fator X do HUD (0,3..1,0) |
| `PS2_HUD_ASPECT=0` | desliga o HUD; um numero vira o fator |
| `PS2_HUD_REFRESH=200,400` | abre/fecha o menu nos frames +200/+400 para o jogo **redesenhar** o HUD no aspecto/escala atual |

`PS2_PRESENT_AR` tambem calcula automaticamente o FOV do mundo: escreve
`(4/3)/aspecto` em `0x00B3EF14` depois dos patches de arquivo. Ex.:

```bash
PS2_PRESENT_AR=2194:1160 PS2_HUD_REFRESH=200,400 \
  ./build/native-naruto/ps2run games/naruto/patched/SLES_556.05 ... \
  --load-state master_mode_ntsc_4kwin.ps2 --resolution 4k
```

O HUD fica no framebuffer do savestate; por isso, ao mudar a proporcao **ou** carregar um
save de outra escala, o `PS2_HUD_REFRESH` e' necessario para o jogo redesenhar o HUD. Sem
ele, so o mundo muda de FOV. O menu aparece por ~1-3 s durante o refresh.

Estados disponiveis:

| arquivo | escala/resolucao |
|---|---|
| `master_mode_ntsc.ps2` | 720p (scale 2) |
| `master_mode_ntsc_4kwin.ps2` | 4K em janela (surface 2194x1160, scale 5) |
| `master_mode_ntsc_4k.ps2` | 4K fullscreen (3840x2160, scale 6) |

O `run_game.sh` usa `master_mode_ntsc_4kwin.ps2` na linha de 4K em janela.

### Menu Practice Settings extraido para o recomp

O menu **Practice Settings** (Master Mode -> Practice -> VS -> Square) foi
capturado e salvo em `menu/`:

- `practice_settings_full.png`, `practice_settings_player.png`,
  `practice_settings_opponent.png`, `vs_practice_settings_screen.png`;
- overlay original `008F4000_8C45F35E.bin` (base `0x8F4000`, 0x38000 bytes) e as
  units recompiladas `recomp_code/m_008F4000_..._unit_0000/0001.cpp`;
- `overlay_strings.txt` (todas as strings ASCII) e `settings_strings.txt`
  (descricoes/opcoes em `0xEF00-0xF700` e `0x10C00-0x10F00`);
- `practice_settings.json` com a secao **Display Settings** (Resolution, Screen
  Mode, Aspect Ratio, FPS, Anti-Aliasing, Graphic API, Texture Filtering,
  Anisotropic Filtering, Draw Distance, MTVU, HUD Aspect Ratio, HUD Scale,
  V-Sync, Exit Game); as secoes Player/Opponent/Battle foram removidas;
- `generate_header.py` -> `runtime/src/menu/practice_settings.hpp`;
- `capture.sh` reproduz a sequencia de `--press`.

O runtime tem `runtime/src/menu/recomp_menu.cpp` (`ps2::menu::menu()`): navegacao
up/down/left/right/confirm/back, `sections()`, `revision()`, `print()` e os hooks
`on_change/on_apply/on_close`. O JSON e' a fonte de verdade para nomes/textos.

**Renderizacao implementada (opcao 1)**: o menu e' desenhado em
`runtime/src/menu/menu_overlay.cpp` (canvas 1920x1080, fonte bitmap
`menu_font.hpp`) e enviado como textura RGBA8 para o passe de apresentacao
(`gpu::present` + `blit.wgsl`, bindings 2/3 e bit 24 de `Blit.pad`). O canvas e'
letterboxed na viewport, sem distorcao em 4:3/16:9/21:9/32:9.

**Entrada**: abre com Square no Mode Select e no Master Mode
(`ps2::recomp_menu_available()`); o prompt **OPCOES [Square]** aparece no canto
superior direito no Mode Select e no canto inferior esquerdo no pause. Setas/W/S
navegam, X/Z/Enter aplicam, A/Esc/Backspace fecham. Enquanto aberto, o host
mascara o pad do jogo.

**Persistencia**: `runtime/src/menu/game_settings.cpp` le/grava
`game_settings.ini` na pasta do projeto antes do parsing da CLI (flags explicitas
vencem; `--widescreen` so liga os patches e **nao** forca 16:9 sobre o aspecto
salvo, mas `PS2_PRESENT_AR` explicito ainda tem prioridade).
A secao Display tem Resolution, Screen Mode, Aspect Ratio, FPS, Anti-Aliasing,
Graphic API, Texture Filtering, Anisotropic Filtering, Draw Distance, MTVU, HUD
Aspect Ratio, HUD Scale, V-Sync e Exit Game. O arquivo e' criado na primeira
execucao e **so e' regravado ao apertar X (Apply)**; mudancas pendentes mostram
o selo **NAO SALVO** e um `*` na linha, e fechar sem X descarta.
Resolution/Screen Mode/V-Sync/HUD Scale/AA/Texture Filtering aplicam na hora;
Aspect Ratio muda apresentacao e FOV no frame seguinte (com patches); Graphic
API e MTVU valem no proximo start. `Exit Game` abre uma confirmacao nativa
(**Sair do jogo / Cancelar**); so `Sair` chama `ps2::halt("exit game")`, e
`Cancelar`/Esc mantem o menu aberto.

**FPS**: a opcao FPS controla a **apresentacao** (`g_options.hz`), nao a logica
(`frame_cycles()` voltou ao nativo 60/50). Um acumulador de presents no vblank
mostra 30/60/120 imagens por segundo sem deixar o jogo lento. Com V-Sync On e
limitado ao monitor; com V-Sync Off pode passar da taxa do monitor. Medido:
30 -> `29,9 render / 59,8 logica / 33,4 ms`; 120 + VSync Off ->
`121,2 / 60,6 / 8,3 ms` em um monitor de 60 Hz. O titulo mostra FPS Render,
FPS Logica, Frame Time e Hz.

**Qualidade**: Anti-Aliasing tem Off/FXAA (4 taps)/FXAA HQ (8 taps)/FXAA Ultra
(12 taps) no `blit.wgsl`; Texture Filtering forca/segue o bit TEX1; HUD Aspect
Ratio corrige a moldura/retrato/barra de vida (tbp 15200/15136/14928/14944/
15072) e o minimapa (15072 + render target 14416); HUD Aspect vence o HUD
Scale manual quando escolhido; Graphic API oferece Auto/Vulkan (e DirectX 12 no
Windows); Software foi removido porque deixava o overlay invisivel; OpenGL nao
esta disponivel neste build do wgpu-native (o backend GL entra em panic);
MTVU liga/desliga a thread de VU1.
Anisotropic Filtering e Draw Distance ficam reservados (o GS nao tem aniso e o
jogo nao expoe draw distance), mas sao persistidos no ini.

**Objetos ao caminhar**: a correcao de HUD antiga tambem tocava 15360/15424/
15616 (texturas compartilhadas com o cenario) e deixava um triangulo preto/
objetos deslocados ao andar. Agora so as texturas de HUD acima sao corrigidas;
15360/15424/15616/15872/16224 ficam intocadas. Evidencia:
`menu/object_walk_hud_fix.png`.

**Janela/resolucao**: o tamanho inicial e' a opcao Resolution, mas a janela pode
ser maximizada/redimensionada. A escala interna e' **min(janela, Resolution)**:
1280x720 + Resolution 2560x1440 -> `Resolucao interna 1024x1024 / 2x`;
1920x1080 -> 1536x1536/3x; 2194x1160 -> 2048x2048/4x; 4K -> 2560x2560/5x.
`--window-size 1280x720` abre uma janela menor sem mudar a Resolution. Mudar
Resolution aplica a escala na hora (`gpu::set_scale`, com readback/restore da
VRAM) e restaura a janela do estado maximizado. O titulo (buffer de 768 bytes)
mostra `Resolucao janela`, `Resolucao configurada`, `Resolucao interna` e
`Supersampling` — antes o buffer de 192 truncava e o campo "interna" mostrava a
Resolution configurada, nao o custo real.

**2D em 16:9/21:9**: as telas 2D nativas 4:3 (title/loading, Mode Select e
character select/VS/Practice) recebem **pillarbox 4:3** em `present()` (FOV 4:3
tambem), em vez de esticar menus/HUD. O **pause do Master Mode usa o aspecto
escolhido** (nao volta mais para 4:3), assim como o battle/mundo 3D e o FOV. O
menu do recomp aberto usa o aspecto escolhido para ocupar a janela. O HUD
pequeno do battle continua com a correcao dinamica.

**Master Mode**: o menu/hint so aparece no **menu de pause** (Item/Quit/Status);
no mundo o Square continua sendo do jogo. O detector usa o estado de campo
(bit 0x800 em 0x617EB8) e o flag de menu (0x618074), mas exige o bit **limpo por
30 frames**, o **icone Map do pause** (tbp 15360/PSMT8 na faixa x 200-310,
y 15-130) e que esse desenho esteja no **ultimo quarto do frame** (HUD 2D
desenhado depois do cenario 3D). O latch antigo disparava em
transicoes/cutscenes, abria o menu no mundo e deixava o pad mascarado
(joystick "morto"). O pad so e' mascarado
enquanto o menu esta aberto; logs `[input] controller/pad` ajudam a diagnosticar.
O host tambem reescaneia/reabre controles a cada 1 s (`[input] controller
disconnected`) para recuperar pads que o driver desconectou sem evento.

**Frame time**: a textura do overlay e' fixa em 1920x1080 e o shader a encaixa
na viewport (letterbox 16:9), sem passe de escala na CPU. O render caiu de
~30 ms para ~1,6-3 ms em 720p (upload ~0,8 ms). Bench antigo com VSync
off/unlocked: 2194x1160/escala 5 -> ~10,9 ms (Mode Select), ~13,4 ms (Master
Mode + hint); 3840x2160/escala 6 -> ~15,7 ms, ~19,3 ms e ~20,2 ms; battle fogo
720p/escala 2 -> ~7,2 ms. Os tiers novos (4 em 2560 e 5 em 4K) reduzem ~36% e
~30% dos pixels; em 720p com Resolution 2560 o interno cai de 2560x2560 para
1024x1024 (~6,25x menos pixels), que era a maior fonte do consumo excessivo.

Detalhes e exemplo de teste: `menu/README.md`; estado pronto em
`menu/states/mode_select.ps2`.

### Captura de janela neste ambiente

`import -window <id>` devolve sempre o **primeiro** frame apresentado (o pixmap X nao e'
atualizado pelo swapchain Vulkan) e `ffmpeg -f x11grab` na area da janela sai preto (scanout
direto, sem compositor). Duas capturas na mesma execucao sao byte-identicas. Para validar
mudanca de imagem use o caminho headless deterministico (`--frames-dir`) ou compare o
**primeiro** frame de execucoes separadas; para o patch, o proprio `PS2_PATCH_LOG` mostra o
valor lido de volta da RAM.

### Hashes de referencia — build NTSC (01/11/2026)

Todos com `--patches games/naruto/patches.txt` (NTSC) e o ELF original; o FOV wide
acrescenta `--patches widescreen_recomp.txt`. "HUD on" e' o padrao em 16:9;
`PS2_HUD_ASPECT=0` desliga a correcao do HUD.

| cena | comando | hash (16 hex) |
|---|---|---|
| master_mode, 15520, 720p, 4:3 | `--resolution 720p` | `b9c2fcb38f35ba62` |
| master_mode, 15520, 720p, 16:9+FOV, HUD on | `--widescreen --resolution 720p` | `0778847c1e9af934` |
| master_mode, 15520, 720p, 16:9+FOV, HUD off | idem + `PS2_HUD_ASPECT=0` | `d2ad9030648551a2` |
| master_mode, 15520, 4K/6x, 16:9+FOV, HUD on | `PS2_SCALE=6 --resolution 4k` | `3b4dff440faac398` |
| master_mode, 15520, 4K/6x, 16:9+FOV, HUD off | idem + `PS2_HUD_ASPECT=0` | `1a6d4ba1899a2bfd` |
| batalha_fogo, 18534, 4K/6x, HUD off | `--frame-every 2` | `c1e38e6c7784496b` |
| batalha, frame 18700, NTSC | `--load-state batalha_fogo.ps2` | `fb137b4e9f241c24` |
| batalha, frame 18700, PAL (binario antigo) | idem, sem patches NTSC | `3953676defefe80b` |
| FMV, frame 3200, NTSC | `--press 700:start+cross:60` | `f2af9334fd1661c0` (linhas 0,38) |
| estado 720p novo, frame 16060, 16:9+HUD on | `--load-state master_mode_ntsc.ps2` | `ec31daeb0ef4a28d` |

Hashes de cenas **com HUD** dependem do estado/refresh e da escala; use as evidencias
visuais (`hud_present_16x9_final.png`, `hud_aspect_*`). O `efe34dda…` (master 4K sem FOV,
HUD off) continua valendo para o binario anterior (PAL); nesta build NTSC o frame estatico
do master e' o mesmo em 720p 4:3, mas nao foi remedido em 4K sem FOV.

## Estado atual — 08/11/2026 (Recomp Settings, escala 4x, HUD/mapa, pacing e draws)

Base: `backup_estado_atual` + menu Recomp Settings. Build de desenvolvimento:
`build/dev-naruto/ps2run`. O launcher `play-dev.sh` usa o ELF NTSC
`games/naruto/patched/SLES_556.05`, `jogo.iso`, `widescreen_recomp.txt` e
`games/naruto/memcard.ps2`.

### Configuracoes e carregamento do ini

- `game_settings.ini` atual: 1920x1080, Windowed, 16:9, FPS 60, FXAA Ultra,
  Graphic API Auto, Texture Filtering Bilinear, HUD Aspect Ratio Off, V-Sync Off.
- O ini e' procurado no `PS2_SETTINGS`, no diretorio atual e seus pais e tambem
  ao lado/acima do executavel. Abrir por atalho ou por `build/dev-naruto` nao
  cai mais nos defaults; o log mostra `[settings] loaded <caminho>`.
- Valores de todas as secoes conhecidas chegam ao runtime, inclusive
  `[game] invert_camera` (antes so o menu recebia e o sync sobrescrevia).
- Screen Mode salvo vale sem `--window`: launch normal (sem `--frames`/
  `--frames-dir`) abre janela/cheio/borderless conforme o ini. A Resolution do
  ini nao forca mais fullscreen; so `--resolution` na linha de comando forca.
- `--frames` e `--frames-dir` continuam sendo o caminho headless/benchmark.
- Recomp Settings: secoes Display Settings, Save States e Game Settings;
  R1/L1 trocam de secao (teclado Q/E). `Inverted Camera` inverte X e Y do
  analogico direito. Save State grava `states/naruto.ps2` e Load State
  restaura; o I/O roda na thread de emulacao no proximo limite de frame
  (`request_save_state`/`request_load_state`), sem serializar no meio de um
  dispatch. O caminho do state fica ao lado do ini.

### Escala interna e apresentacao

- A Resolution manda na escala: 960x720=2x, 1280x720=3x, 1600x900/1920x1080=4x,
  2560x1440=5x, 4K=6x. 1080p = 2048x1792 (antes 3x/1536x1344). O 3x era mais
  estreito que a janela de 1920, entao a apresentacao ampliava e texto/sprites
  ficavam pixelados; 4x faz downscale (supersampling). `PS2_INTERNAL_SCALE=n`
  e' override de desenvolvimento.
- Janela nao maximizada segue o aspecto escolhido (16:9 -> 1920x1080;
  21:9 -> 1920x823; 32:9 -> 1920x540). Maximizar e' permitido.
- Apresentacao em dois passes: o blit pesado (8 taps + FXAA + overlay) roda na
  Resolution configurada; um segundo passe barato (`fs_up`, uma amostra de
  textura) escala para a janela. Maximizado em 16:9 preenche sem barras;
  4:3 mantem barras laterais corretas. O custo de GPU nao cresce com a janela.
  O titulo mostra `Janela`, `Resolucao apresentada`, `Resolucao configurada`,
  `Resolucao interna` e `Supersampling`. `PS2_NO_PRESENT_CAP=1` desativa o
  limite so para diagnostico.
- `PS2_PRESENT_LOG=arquivo.csv` grava V (vblank), S (pace), E (post do
  present), P (present) e Q (fim do present) com timestamp em us.

### Filtragem (Bilinear) e FMVs

- A grade padrao continua `sub` (supersampled). `PS2_TEX_GRID=native` ainda
  existe como override de desenvolvimento; `PS2_BILIN_STEP*` e' ignorado com
  Texture Filtering = Bilinear.
- Com Bilinear, usam a grade nativa (bit 17 do Setup):
  - a camera de UI inteira (`xyoff 0x7000/0x7200`): retrato, barra de vida,
    textos, minimapa e mapa;
  - TBPs 14416, 14450, 14928, 14944, 15072, 15136, 15200 (reforco);
  - texturas <= 64x64 (fontes/icones);
  - texturas magnificadas >= 1.5x na tela.
  O cenario (camera normal) fica na grade sub. As tiras de FMV (64x512,
  tbw=1/psm=0) ficam sempre fora; os frames de FMV 7000/8000 continuam
  byte-identicos ao build sub. Overrides: `PS2_NO_UI_NATIVE=1`,
  `PS2_NATIVE_TEX_TBP=...`, `PS2_NATIVE_MAX_TEX=...`, `PS2_NATIVE_MAG=...`.
- HUD Aspect Ratio = Off no ini e como default (evita o bug de objetos no
  cenario do Master Mode). O HUD Aspect continua disponivel no menu.

### Frame pacing (micro travadas)

- O `pace()` rodava depois de postar o present e deixava o emulador adiantar
  2-3 vblanks ate a fila de audio encher (80 ms): os presents saiam em pares
  (2 ms + 25-33 ms) e o 1% low ficava em ~23 FPS.
- Correcao: `pace()` roda ANTES do post e limita cada vblank a 16,67 ms em
  tempo real (60 Hz); a fila de audio (60 ms) e' apenas guarda de underrun.
- MangoHud, master_mode andando, 1080p/4x, VSync Off (20 s por frame):
  - antes: media 60,0; 1% low 23,1; 0,1% low 22,5; 7,2% >40 ms; max 44,5 ms;
  - depois: media 60,0; 1% low 39,1; 0,1% low 34,7; nenhum frame >33 ms;
    max 28,8 ms; GPU 80% (pico 83%);
  - VSync On piora (~51 FPS medios, 1% low 28): manter Off.
- Ainda resta um padrao ~17/24 ms porque o GS desenha um campo a cada dois
  frames (~5-7 ms no frame pesado). Eliminar isso exigiria bufferizar um frame
  inteiro (mais latencia) ou otimizar o caminho de draws.
- Evidencias: `menu/mangohud_before_summary.csv`,
  `menu/mangohud_after_summary.csv`, `menu/mangohud_drawopt_summary.csv`,
  `menu/mangohud_after_perframe.csv`.

### Otimizacoes do caminho de draws

- `rendered()` (varredura de paginas de 8 KiB) so roda nos modos nativos
  experimentais e no draw log; com Bilinear normal era overhead em ~10 mil
  draws/frame.
- `add_spans`: bbox pequena (<= 64 tiles) usa o retangulo inteiro em vez da
  matematica de arestas (3 retas + divisoes em double); o raster ainda testa o
  triangulo real. Frame 3000 byte-identico. `PS2_SPAN_WHOLE_TILES=0` desliga.
- Listas de tiles preenchidas com store `uint16` direto.
- Medido no master_mode (623 frames): spans 588 -> 486 ms, fill 598 -> 571 ms
  (~11% no `flush_batch`). `PS2_BATCH_STATS=1` mostra flushes/prims e tempos.

### Perfil de CPU (o que puxa o "CPU real")

`PS2_PROFILE` no master_mode andando (amostras de CPU):

| item | ~% |
|---|---|
| `flush_batch` (draws/tiles) | 18% |
| `vif::run` (VIF/pacotes VU1) | 10,6% |
| `gpu::draw` (setup/batch) | 8,7% |
| VU1 recompilado | 14% |
| fila/GIF | 8% |
| GS write/vertex | 5% |
| IOP | 2,6% |

O `Uso CPU real 14%` do titulo e' do sistema inteiro (~1,6-1,7 nucleos); o
`Uso CPU EE 140%` e' o thread de emulacao. Nao e' resolucao nem AA. MTVU On x
Off deu praticamente o mesmo 1% low (~39). Proximos alvos: unpack/packet do
VIF, microcodigo VU1, fila GIF e caminho de write do GS.

### Evidencias e estados

- Estados de teste: `states/mode_select.ps2` (frame 3645),
  `states/master_mode.ps2` (frame 2677), referencia `pcsx2_mode_select.png`.
- Capturas do build atual: `menu/bilinear_fix_mode_select.png`,
  `menu/bilinear_fix_master_mode.png`, `menu/window_maximized_1920x1080.png`.
- Comando base de teste:

```bash
./build/dev-naruto/ps2run games/naruto/patched/SLES_556.05 \
  --disc jogo.iso --widescreen --ntsc --patches widescreen_recomp.txt \
  --load-state states/master_mode.ps2 --memcard games/naruto/memcard.ps2 --window
```

```bash
MANGOHUD_CONFIG="output_folder=/tmp/mh,autostart_log=1,log_duration=20,log_interval=0,fps,frametime,gpu_stats,cpu_stats" \
  mangohud ./build/dev-naruto/ps2run ... --window --frames 4500
```

### Build exportado — 08/11/2026

Pacote Linux x86_64 em `dist/`:

```text
dist/naruto5-recomp-linux-x86_64/        (pasta pronta para rodar)
dist/naruto5-recomp-linux-x86_64.tar.gz  (35 MB)
dist/naruto5-recomp-linux-x86_64.zip     (35 MB)
dist/SHA256SUMS
```

Conteudo: `ps2run` (build/dev-naruto), `SLES_556.05` patcheado NTSC,
`widescreen_recomp.txt` (+21:9/32:9), `game_settings.ini`, `jogar.sh`,
`LEIA-ME.txt`, `BUILD_INFO.txt`, `LICENSE` e `SHA256SUMS`. A ISO do jogo e o
memory card **nao** acompanham; o launcher aceita o caminho da ISO:

```bash
./jogar.sh /caminho/jogo.iso
```

Teste do pacote: `[settings] loaded .../dist/.../game_settings.ini`,
drawable 1920x1080, `[gpu] window scale 4 internal 2048x1792`.

Checksums dos arquivos:

```text
bad82b73195deba02f3f970602ce3f066b27304484e841d1eff8dd0490a2eb67  naruto5-recomp-linux-x86_64.tar.gz
0e5f9eb63a0da4b96c7a70ba550aecd8bc56578132e1f0572773972d9b54e127  naruto5-recomp-linux-x86_64.zip
```

O build e' Linux x86_64; nao ha toolchain MinGW/wgpu Windows configurado neste
ambiente para gerar um pacote Windows.

## Captura estatica total (fim do missing code na tela de New Game)

**Sintoma**: o build passava no smoke de 300 frames, mas o jogo fechava ao
entrar em New Game com `[ee] halted: missing code at 0x005E3BC0
(ra 0x0011A0EC)`. As funcoes em volta (`0x005E3A00`, `0x005E3BC0`,
`0x005E3C30`, `0x005E4C60`, ...) existiam no ELF, mas nao eram traduzidas.

**Causa**: `analyse()` calculava `text_begin/text_end` a partir do codigo ja
descoberto e usava essa faixa como filtro dos candidatos. O procedimento
`0x0011A0EC` percorre uma tabela de ponteiros de funcao em `0x005E7210` e
chama cada alvo com `jalr`; como a tabela fica depois do ultimo trecho de
codigo conhecido, nenhum dos alvos entrava na faixa e a varredura os
descartava. O `frame_starts` tambem exigia um `jr ra` imediatamente antes do
prologo, mas as funcoes sao alinhadas com padding de zeros.

**Correcao**:
- `scan_range()` usa o intervalo declarado pela imagem so quando ele existe
  (modulos SNR2); para o ELF principal, que nao tem secoes nomeadas, varre
  todos os segmentos executaveis, incluindo as tabelas de ponteiros.
- `prologue_starts()` semeia todo `addiu/daddiu $sp,$sp,-N` como funcao. Isso
  cobre alvos de `jalr`, vtables e tabelas de ponteiros sem exploracao.
- A varredura de todas as palavras do segmento continua alimentando
  candidatos; `tentative()` valida o alvo e, a cada rodada, os rejeitados sao
  reavaliados.
- `scripts/payloads.py` extrai `PRG/*.BIN` da ISO, confere que o arquivo tem
  codigo MIPS (>= 8 prologos) e o grava em `overlays/` como
  `<base>_<crc>.bin`. ADV/BTL/ETC vao direto para `0x006C6D00` sem depender
  do dump de RAM.
- Os checks de identidade das janelas e modulos passaram a usar inicios de
  funcao (palavras estaveis), nao qualquer palavra marcada como codigo, para
  que dados escritos pelo jogo nao invalidem a imagem.

**Resultado**: `eerecomp` passou de 237 para ~1216 unidades e a varredura
estatica leva ~3 s no ELF principal. Uma instalacao limpa com
`--no-explore` ja entra em New Game e roda o jogo; a exploracao passou a
achar 0 entry points novos e so captura imagens de janelas de runtime.
`0x005E3BC0`, `0x005E3C30`, `0x005E4C60`, `0x005E4CA0`, `0x005E4D20`,
`0x005E4E10`, `0x005E4E70` e `0x005E5090` agora sao entradas traduzidas.

## Entry points de overlay em tabelas de dados (0x006CEC10)

**Sintoma**: o build instalado fechava em `missing code at 0x006CEC10
(ra 0x003BF860)` ao entrar no modo selecionado, enquanto o build de dev
funcionava. O endereco fica em ADV (`0x006C6D00 + 0x7F10`) e e' uma funcao
folha que comeca com `jr ra`, sem prologo.

**Causa**: o programa principal chama `0x0011A130`, que percorre uma tabela de
ponteiros de funcao **em RAM** e usa `jalr`. A tabela e' inicializada a partir
de palavras em `0x00509A28` (e outras) no proprio ELF. `window_entries()` so
olhava instrucoes `jal`/`jmp`, entao os alvos guardados em dados nunca eram
semeados na imagem do overlay. A versao de dev tinha esses offsets porque
foram capturados em exploracao; o build limpo nao.

**Correcao**: `window_entries()` agora tambem varre todas as palavras dos
segmentos do programa principal e semeia na imagem do overlay qualquer valor
dentro de `[base, base+size)`. As tabelas de ponteiros viram entradas
estaticas, sem exploracao. Para ADV/BTL/ETC isso adicionou ~5,6 mil entry
points (de 1.485 para 7.102) e 0x006CEC10 passou a ser traduzido.

**Cache de analise**: `port.sh` grava `games/naruto/.analysis-key` com hash do
ELF, ISO, overlays, `entries.txt` e `vu1`. Se nada disso mudou, a exploracao e'
pulada e so a traducao/compilacao incremental roda. Uma alteracao de runtime
ou recompilador nao forca nova exploracao; `--refresh` forca quando necessario.

**Resultado**: o caminho que antes parava em 0x006CEC10 roda ate o limite de
frames. Rebuild de dev com cache: ~9 s em vez de refazer traducao + 3.600
frames de exploracao. O pacote publico continua sem dados do jogo: os PRG sao
extraidos da ISO do usuario e as tabelas de ponteiros vem do proprio ELF.

## VU1 AOT: lag no Master Mode e nas lutas

**Sintoma**: depois de corrigir os missing codes, o jogo rodava, mas Master
Mode e lutas ficavam lentos; o mode select era normal. O build nao tinha
nenhum programa VU1 compilado, entao `vu_micro.cpp` interpretava o microcodigo
a cada execucao. O dev tinha 51 arquivos em `games/naruto/vu1` e o build
publico tinha zero.

**Causa**: a exploracao de 3600/6000 frames terminava antes da primeira cena
3D (a tela de menu aparece por volta do frame 7800). O `PS2_VU_RECORD` so
grava quando um microprograma novo e' executado; sem chegar na luta, nada era
gravado, `vurecomp` nao gerava `vu1_*.cpp` e o runtime caia no interpretador.

**Correcao**:
- A exploracao padrao voltou para 12000 frames (GUI: 12000/16000), suficiente
  para passar pelo menu e pela primeira luta e gravar os primeiros VU1.
- `jogar.sh` exporta `PS2_VU_RECORD=games/naruto/vu1` e, ao sair, chama
  `scripts/vu_refresh.sh`. Se novos microprogramas foram gravados, ele roda
  `vurecomp` e `ninja`; o proximo inicio usa o caminho AOT. O `vu_refresh.sh`
  usa `.manifest` para nao rebuildar quando nada mudou.
- Assim, a primeira visita a uma cena nova pode gravar (e ficar no
  interpretador), mas depois de sair o launcher compila e as proximas visitas
  sao rápidas. O Master Mode do usuario ja foi gravado e compilado (42
  arquivos, 96 programas, 54 jump targets).

**Medicao** (8000 frames, Master Mode, mesma entrada):
- com VU1 AOT: 75 s (~106 fps headless);
- sem VU1 AOT (`PS2_NO_VU_AOT=1`): 163 s (~49 fps).
O diagnostico `PS2_NO_VU_AOT=1` foi adicionado ao runtime para comparar.

**Resultado**: Master Mode e lutas usam o codigo VU1 compilado. A primeira
sessao que descobre uma cena nova ainda grava, mas o launcher compila ao sair;
nao e' preciso refazer o build inteiro nem a exploracao.

## Link da GPU no pacote autocontido

**Sintoma**: a instalacao falhava no link com `undefined symbol:
ps2::gs::gpu::present_width()` (e outros) durante a exploracao. O `gpu.cpp`
nao entrava em `libps2runtime.a`.

**Causa**: o `package.sh` passou a copiar o `port.sh` do dev, que nao passava
`-DPS2_WGPU_DIR`/`-DSDL2_DIR`. O `CMakeCache` antigo apontava para
`third_party/wgpu-native`, que nao existe no pacote (o wgpu fica em
`toolchain/wgpu-native`); o `CMakeLists` entao omitia `runtime/src/gs/gpu/gpu.cpp`.

**Correcao**: `build_native()` roda `cmake` em toda build (nao so quando
`build.ninja` falta) e passa os caminhos do toolchain quando existem. Isso
tambem corrige qualquer mudanca futura na lista de fontes do runtime.

**Resultado**: `gpu.cpp` aparece no `build.ninja`, o link conclui e o build
instalado volta a abrir a janela/renderizar.

## Launcher e PS2_NO_VU_AOT

`PS2_NO_VU_AOT=1` foi criado so para comparar interpretador x AOT. Se ele ficar
exportado no shell (por exemplo, depois de um teste), o jogo volta a usar o
interpretador em todas as cenas e parece lento de novo. O `jogar.sh` agora:
- avisa `ignorando PS2_NO_VU_AOT (diagnostico); usando VU1 compilado`;
- remove a variavel antes de iniciar;
- imprime `[launcher] VU1 compilado: N programas` a partir de `vu1_table.cpp`.

Na instalacao de referencia: 55 arquivos VU1, 124 programas compilados. Teste
windowed (1080p, Master Mode e luta) ficou em 60,0 fps; headless AOT ~109 fps
contra ~49 fps com `PS2_NO_VU_AOT=1`.

## Port Windows x64 (instalador e runner nativo)

Ver a secao equivalente no diario de desenvolvimento. O pacote inclui
`instalar.ps1`/`instalar.bat`, `jogar.bat`, `scripts\port_win.py`,
`scripts\build_win.py` e `scripts\vu_refresh_win.py`. Testado sob Wine (runner
300 frames sem missing); falta teste em Windows real.

## Windows: 7 fps e atomic::wait

O fallback do libc++ no Windows fazia poll/no-op notify; a fila GS espera
varias vezes por frame. O pacote usa WaitOnAddress/WakeByAddressAll do
kernel32 (Windows 8+) com fallback de condition_variable. O instalador wizard
e o diagnostico.bat/launcher.log ajudam a confirmar VU1 compilado e o backend.
