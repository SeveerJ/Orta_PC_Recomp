# Orta PC Recomp — Panzer Dragoon Orta, recompiled for PC

A native Windows build of **Panzer Dragoon Orta** (original Xbox), made by
statically recompiling the game's code to C and running it on a PC
reimplementation of the Xbox kernel, GPU (NV2A → Direct3D 11) and audio.

Built on [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nznet
(MIT, with xemu-derived parts under LGPL-2.1). Many GPU behaviours are matched
against [xemu](https://xemu.app).

> **Status:** work in progress. Every episode is playable from start to finish.
> See [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md) and [docs/ROADMAP.md](docs/ROADMAP.md).

## You need your own copy of the game

This repository contains **no game code or assets** — no XBE, no disc files,
no recompiled game code, no textures. You supply files from your own disc.

## Requirements

- Windows 10 or 11, 64-bit
- A DirectX 11 graphics card
- An Xbox controller or any XInput pad
- Your own copy of Panzer Dragoon Orta

## Repository layout

| Path | What it is |
|---|---|
| `xboxrecomp-main/` | The xboxrecomp toolkit, with this project's changes: the recompiler (`tools/`) and the runtime (`src/`), which covers the kernel, the NV2A GPU on Direct3D 11, audio, input and video |
| `orta/` | The Orta game project: `main.c` (settings.ini, start-up), `recomp_manual.c` (hand-written replacements such as controller, rumble and crash guards), `overlay.cpp` (the F1 settings menu), the build scripts and the playtest package template |
| `orta/seeds/` | Function addresses found while playing, which the recompiler needs. They are addresses only, with no game code. Copy them to `xboxrecomp-main/game_files/` before translating |
| `orta/third_party/imgui/` | Dear ImGui 1.91.9 (MIT) |
| `docs/` | Roadmap, known issues and changelog |

## Building

You need Windows 10/11 with Visual Studio 2022 (Desktop C++), CMake 3.20+ and
Python 3.

1. Copy `default.xbe` and the rest of the disc's files from **your own copy of
   the game** into `xboxrecomp-main/game_files/`.
2. Copy the three files from `orta/seeds/` into the same folder.
3. From a Developer PowerShell for VS 2022, in `orta/`, run
   `powershell -ExecutionPolicy Bypass -File .\orta_loop.ps1`. It
   disassembles and translates the game to C in `orta/src/recomp/gen/` (this
   output is never committed), builds with CMake into `build64/`, and starts
   the game.
4. `orta_package.ps1` then gathers a playable folder: the exe,
   `settings.ini`, `engine.ini` and the README from `orta/package/`.

The toolkit's own guide, `xboxrecomp-main/docs/GETTING_STARTED.md`, explains
the pipeline in depth.

## Licence

Code from xboxrecomp keeps its MIT licence (`xboxrecomp-main/LICENSE`). The
xemu-derived files keep their LGPL-2.1-or-later licence and notices
(`xboxrecomp-main/NOTICE`, `xboxrecomp-main/LICENSES/`). Dear ImGui is MIT
(`orta/third_party/imgui/LICENSE.txt`).
