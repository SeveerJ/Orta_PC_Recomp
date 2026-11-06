# Changelog

## 0.8.0-experimental

### New
- **New launcher,** styled after the game's own option menus: pages for
  Video, Sound and Launcher, a description of every setting with its
  default, a green mark on anything changed, and full controller support
  (D-pad/stick, LB/RB for pages, A or Start to play).
- **5.1 surround uses the game's own Dolby Digital mode:** with Speakers = 5.1
  the game places sounds across all six speakers itself, as on an Xbox set to
  Dolby Digital. Tested on a home-theatre system.
- **Ultrawide (experimental):** `RECOMP_ASPECT = 21:9` (or `32:9`) under
  `[Advanced]` widens the 3D view instead of stretching it. The HUD and menus
  are still stretched in this first version.
- **"Save graphics capture"** in the F1 menu (the same as F11), for players
  on a controller or Steam Deck.

### Fixes
- **Steam Deck / Proton:** the game now ships Microsoft's shader compiler
  instead of relying on Wine's, which crashed or took many minutes on the
  game's graphics shaders (the likely cause of black cutscenes, glitches and
  low frame rates on the Deck).
- **Low-end PCs:** the faster per-effect graphics shaders are built about twice
  as fast, on three background threads, and Setup now builds 129 of them in
  advance. A ThinkPad T495 went from about 15-20 fps to 30-50 fps in play.

### Notes
- The shader cache is rebuilt once after updating (Setup does this, so the
  first start stays quick).
- The installer now includes `d3dcompiler_47_ms.dll` (Microsoft's
  redistributable shader compiler, from the Windows SDK) and two fonts for the
  launcher (SIL Open Font License, see `FONTS_LICENSE.txt`).

## 0.7.1

### Fixes
- **Last boss** now disappears under the water as it should: the Xbox's
  stencil masking is emulated (it was ignored before).
- **Pandora's Box** missions and scenarios no longer crash on start (a piece of
  the game's sound code was never translated).

### New
- **Anti-aliasing:** Off / FXAA (fast) / FXAA (high quality), in the launcher,
  the F1 menu and settings.ini.
- **Performance report** in `session_log.txt`: CPU use per thread every 10 s,
  and per-frame graphics timings, to help diagnose slow PCs.

## 0.6.0

### Installing
- **One-file installer** (`OrtaPC_Setup`): finds your disc image, checks it,
  downloads its tools, builds and installs the game. Works on Steam
  Deck/Proton.
- **Quick first start:** Setup prepares the graphics shaders, so the first
  start no longer takes minutes.

### New
- **Launcher** before the game starts: resolution (labelled with real sizes,
  480 by default), display mode, widescreen, VSync, volume, music, sound
  effects and speakers.
- **Separate music and sound-effect volumes,** 0-200, in the launcher and the
  F1 menu. Master volume no longer crackles when turned up.
- **5.1 output** (Speakers = 5.1), plus an experimental Dolby Digital switch.
- **Data folder** next to the game for the cache and compiled shaders,
  replacing the hidden `%LOCALAPPDATA%\xboxrecomp`. Existing saves and
  shaders are copied over on the first start.
- **Session logs** (`session_log.txt`, `session_log_previous.txt`) next to the
  game, so crashes and hangs leave a report.

### Fixes
- **Freeze after the first boss** (picture froze, sound carried on).
- **Voices too fast:** proper resampling for sound played at other rates.
- **Much lower CPU use** (title screen from about 2.6 cores to 0.4): polling
  loops and idle waits now sleep instead of spinning.
- **Loading:** game files use Windows' file cache.

## Earlier

### Graphics
- **Water (Episode 2 river):** the reflection now matches xemu. This adds
  per-pixel bump lookups and reads swizzled render targets as textures.
- **Hair and wings sitting too low:** the fixed-function vertex path now
  applies the viewport offset.
- **Episode 2 boss flicker:** back-face culling and strip winding are now
  implemented.
- **Episode 3 boss shadow:** real shadow maps (depth textures with compare).
  Replaces the checked pattern.
- **Episode 3 boss speed:** up from about 20 fps, after fixing texture sizes for
  2-byte formats and readback overlap.
- **Depth:** added depth-range culling and polygon offset.
- **Points and lines:** now drawn, via geometry shaders (in testing).

### New
- **Settings menu (F1, or Back + Start on the pad):** covers display, audio
  and controller settings. The game pauses and mutes while it is open, and
  changes are saved to settings.ini.
- **Rumble:** the game's vibration now reaches the PC pad. Set it with
  `Rumble` / `RumbleStrength` in settings.ini or in the menu.

### Audio
- **Sound effects too loud and clipping:** effects mixdown halved, a soft
  limiter added, and an `SfxVolume` setting.

### Tools
- **F11 frame capture:** for debugging.

## Playtest package 1
- Every episode playable start to finish.
- Movies show their picture.
- Widescreen without letterbox bars.
