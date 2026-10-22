# Changelog

## Unreleased

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
