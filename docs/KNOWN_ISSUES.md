# Known issues

- **Episode 3, octopus:** when its charged attack hits, the red hit marks
  appear near the octopus instead of up front on the screen.
- **Episode 3, octopus:** short frame dips while new data streams in.
- **Music sometimes doesn't start, or stops,** at the start of a level or
  after a berserk attack. Pausing and unpausing starts it again. Long-standing;
  `RECOMP_APU_STREAMLOG = 1` under `[Advanced]` logs what the music does, for
  reports.
- **Short hitches when an episode or boss loads** new files, worst on a hard
  disk: the game reads and caches its files one at a time. A fix (reading ahead
  and a pre-filled cache) is in progress.
- **Pandora's Box:** the missions, scenarios, encyclopedia and box game now
  run; not every sub-episode has been played through yet.
- **Low-end PCs:** a ThinkPad T495 now plays at about 30-50 fps (480), with
  short stutters the first time a new effect appears while its shader is built.
- **Steam Deck / Proton:** the shader-compiler fix in 0.8.0 is untested on a
  Deck so far; reports welcome.
- **Ultrawide (experimental):** the HUD and menus are stretched.
- **5.1:** the game itself sends nothing to the subwoofer channel; a receiver's
  bass management covers it.


## Saving

The game saves only when you choose to, at the end of each episode. Always
accept the save screen after clearing an episode.
