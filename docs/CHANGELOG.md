# Changelog

## 0.7.0

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
