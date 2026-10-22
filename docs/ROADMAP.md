# Roadmap

In priority order:

1. **Octopus laser (Episode 3).** Its charged attack doesn't render. Points and
   lines were never drawn; geometry-shader support has been added and is
   awaiting a test.
2. **Rumble.** Pass the game's XInputSetState motor speeds through to the PC pad,
   with On/Off and strength settings.
3. **Settings overlay.** An in-game menu (Dear ImGui on D3D11) that pauses the
   game and covers video, graphics, audio and controls, saved to settings.ini.
4. **Existing bugs.** Brown void in vents, a possible freeze before the Episode 8
   boss, octopus loading stutters, and the playtest issue list.
5. **Texture packs.** Hash-based dumping and replacement, including high-res
   replacements.
6. **Mouse aim** (maybe). The mouse acts as a virtual stick, with a possible
   direct-aim mode.

Also: GitHub release preparation (licence and notice audit, build docs).
