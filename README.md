# JROrta — Panzer Dragoon Orta, recompiled for PC

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

## Building

Build instructions will be added when the full source tree is synced.

## Licence

Code from xboxrecomp keeps its MIT licence. The xemu-derived files keep their
LGPL-2.1-or-later licence and notices. See `LICENSE` and `NOTICE`, which will be
added with the source sync.
