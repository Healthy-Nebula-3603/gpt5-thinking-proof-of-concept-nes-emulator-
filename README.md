That is just a proof of concept.

A NES emulator in pure C make by GPT5 thinkiing hing with codex in 40 minutes.

Only works partically with a Soccer for a NES.


<img width="1403" height="1089" alt="Screenshot 2025-09-13 002928" src="https://github.com/user-attachments/assets/5433cf70-1cb3-4201-8b15-64d5e2b8f86f" />
<img width="1438" height="1094" alt="Screenshot 2025-09-13 000139" src="https://github.com/user-attachments/assets/93340f53-6633-4787-b664-c6c572329513" />
<img width="1375" height="1105" alt="Screenshot 2025-09-12 235215" src="https://github.com/user-attachments/assets/dd95583f-bcb3-4632-8c9b-4e3c70daa1f6" />



Requies installed SDL and LINUX enviroement. 


- Build: make
- Run: ./nes_emu Soccer.nes --sdl




------------------------------------------------------------------------------------------------------------



NES Emulator (Simple, NROM-only)

Overview
- Minimal Nintendo NES emulator in C focused on loading and running iNES (.nes) ROMs using the simplest mapper (NROM / Mapper 0) only.
- CPU: 6502 (NES variant, no decimal mode). All official opcodes implemented.
- PPU: Very simplified stub that only handles registers and generates NMIs at 60Hz. No real rendering.
- APU: Not implemented (registers stubbed).
- Controllers: Basic stubs for $4016/$4017.

Status / Expectations
- This is a learning-friendly skeleton that can parse iNES headers, map NROM PRG ROM, run CPU code, and tick a fake PPU/NMI. It will not render graphics or play audio.
- It may run simple homebrew and diagnostic ROMs that don’t require real PPU behavior. Commercial games generally won’t be playable.

Build
- Requires a POSIX-like environment and a C compiler (clang or gcc).
- No external dependencies.

Commands
- Build: `make -C nes-emu`
- Run (headless): `./nes-emu/nes_emu path/to/rom.nes`
- Run with SDL2 window: `./nes-emu/nes_emu path/to/rom.nes --sdl`
  - If SDL2 dev is not installed, it will fall back to headless automatically.
  - On Linux, install `libsdl2-dev` (or platform equivalent) to enable.

Project Structure
- `src/main.c`            Entry point, CLI, run loop
- `src/nes.{c,h}`         NES top-level, wiring CPU/PPU/Bus/Cart
- `src/cartridge.{c,h}`   iNES loader and NROM mapper
- `src/bus.{c,h}`         Memory map and IO stubs
- `src/cpu.{c,h}`         6502 CPU core
- `src/ppu.{c,h}`         PPU register stub + NMI timing
- `src/controller.{c,h}`  Controller (joypad) stubs
- `src/util.h`            Common helpers
- `src/video.{c,h}`       Optional SDL2-backed color fill per frame

Notes
- Mapper support: only Mapper 0 (NROM). PRG-RAM supported at $6000-$7FFF.
- PPU timing is coarse (CPU-cycle-derived vblank cadence). Only generates NMI; no real VRAM/CHR behavior.
- Decimal mode is disabled per NES CPU behavior.

Debugging
- Print first N instructions: `--trace-ins N`
- Print registers for first N frames: `--trace-frames N`
