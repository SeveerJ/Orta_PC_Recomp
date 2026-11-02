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
- **Pandora's Box** extras crash the game; they were never tested.
- **Slow on low-end PCs:** about 20 fps on a ThinkPad T495 at the original
  480 resolution. Being looked into.
- **5.1:** sound from behind reaches the rear-left speaker a little and the
  rear-right not at all, and nothing goes to the LFE. The Dolby Digital switch
  is untested.


## Saving

The game saves only when you choose to, at the end of each episode. Always
accept the save screen after clearing an episode.
