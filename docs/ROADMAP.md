# Roadmap

In priority order:

1. **Vulkan renderer.** A native Vulkan renderer alongside Direct3D 11,
   selected in settings until it matches it: shaders compiled in advance (no
   shader compiler on the player's machine, no per-effect builds while
   playing), MSAA built in, and the path to a native Linux build.
2. **Loading hitches.** Read each episode's files ahead in the background,
   pre-fill the game's cache, and log what the game is doing in the remaining
   pauses.
3. **Music dropping out.** Catch it with the stream log and fix the cause.
4. **Low-end PCs** (ThinkPad T495, ROG Ally, Steam Deck): keep measuring with
   the `[PERF]` report; cut CPU-GPU waits and repeated texture/vertex work.
5. **Ultrawide:** keep the HUD and menus at 16:9 (anchored to the screen
   edges) and draw the full width.
6. **Debug cheats** to help testers (unlock all, episode select,
   invincibility, speed control); finish testing every Pandora's Box item.
7. **Octopus hit effect (Episode 3).** The red hit marks from its charged
   attack appear near the octopus instead of in front of the player.
8. **Before going public:** rewrite the few translated lines in
   `recomp_manual.c`; an icon we're free to use.
9. **Texture packs.** Hash-based dumping and replacement.
10. **Mouse aim** (maybe).

Done: settings menu, launcher (and its redesign), rumble, Windows installer
(works on Steam Deck/Proton), quick first start, Episode 3 freeze, enemy
health, vents, Episode 8 freeze, first-boss freeze, voices at the right pitch,
separate music and effects volume, 5.1 surround (the game's Dolby Digital
mode), Data folder, session logs, last boss under water (stencil), Pandora's
Box crash, anti-aliasing (FXAA), Microsoft's shader compiler for Proton,
faster shader builds and a pre-built shader list, experimental ultrawide.

Also: GitHub release preparation (licence and notice audit, build docs).
