# Roadmap

In priority order:

1. **Loading hitches.** Read each episode's files ahead in the background,
   pre-fill the game's cache, build the specialised graphics shaders during
   Setup, and log what the game is doing in the remaining pauses.
2. **Music dropping out.** Catch it with the stream log and fix the cause.
3. **Low-end PCs** (ThinkPad T495, ROG Ally): measure where the time goes,
   then cut CPU-GPU waits and repeated texture/vertex work.
4. **Pandora's Box crashes**, with debug cheats to help testers (unlock all,
   episode select, invincibility, speed control).
5. **5.1 audio:** map the rear speakers properly; test the Dolby Digital
   switch.
6. **Anti-aliasing:** SMAA/FXAA option.
7. **Octopus hit effect (Episode 3).** The red hit marks from its charged
   attack appear near the octopus instead of in front of the player.
8. **Before going public:** rewrite the few translated lines in
   `recomp_manual.c`; an icon we're free to use.
9. **Launcher styling** with art from the player's own disc.
10. **Texture packs.** Hash-based dumping and replacement.
11. **Mouse aim** (maybe).

Done: settings menu, launcher, rumble, Windows installer (works on Steam
Deck/Proton), quick first start, Episode 3 freeze, enemy health, vents,
Episode 8 freeze, first-boss freeze, voices at the right pitch, separate music
and effects volume, 5.1 output, Data folder, session logs.

Also: GitHub release preparation (licence and notice audit, build docs).
