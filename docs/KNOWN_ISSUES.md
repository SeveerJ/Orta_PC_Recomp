# Known issues

- **Episode 3, octopus:** the charged laser attack doesn't render (fix in testing).
- **Episode 3, octopus:** short frame dips while new data streams in.
- **Vents:** the background can show as a flat brown area until you leave.
  Recent fixes may have cleared this; it needs checking.
- **Installed copy starts slowly:** the game built by `OrtaPC_Setup.exe` sits
  in Task Manager for minutes before its window opens. The engine itself is up
  in under a second once it starts logging, so the wait comes earlier (before
  the kernel layer starts). Not yet diagnosed: candidates are Windows Defender
  scanning the new unsigned exe, symbol loading at start-up (`SymInitialize`)
  in the clang-built exe, or the Xbox memory layout set-up behaving
  differently under the clang/MinGW build. The Visual Studio dev build does
  not do this.
- **Episode 8:** the game may rarely freeze just before the boss. Restart from
  your last save.

## Saving

The game saves only when you choose to, at the end of each episode. Always
accept the save screen after clearing an episode.
