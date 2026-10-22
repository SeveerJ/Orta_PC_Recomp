PANZER DRAGOON ORTA - PC PLAYTEST BUILD
=======================================

Thanks for testing! This is the original Xbox game translated to run
natively on Windows. It is a work in progress, so please note anything
odd (see "Reporting problems" below).

STARTING THE GAME
  Double-click "Panzer Dragoon Orta.exe".
  The first start takes about a minute before anything appears while the
  game sets up its cache - that is normal. Later starts are quicker.

CONTROLS
  An Xbox controller (or any XInput pad) works as the original pad.
  Alt+Enter switches between fullscreen and a window.
  Close the window to quit (save in-game first!).

SETTINGS (settings.ini - open it with Notepad)
  Resolution   2160 = 4K (default), 1440 for a 1080p/1440p screen.
               Lower it if the game runs slowly.
  Fullscreen   1 = fullscreen (default), 0 = window.
  Widescreen   1 = 16:9, 0 = original 4:3.
  FrameRateCap 60 (the game is made for 60).
  VSync        1 = no tearing (slightly more input lag).
  Volume       0 to 100.

SAVES
  The game only saves when you choose to, at the end of each episode.
  Always say yes to the save screen after clearing an episode, or that
  episode will have to be played again. Saves go in game_files\UDATA -
  back that folder up if you want to keep them.

WHAT'S NEW IN THIS BUILD
  - Every episode can now be played from start to finish.
  - The opening movie and the other movies now show their picture.
  - Widescreen: the black bars above and below the picture are gone.
    The game now draws the full picture instead of zooming in.

KNOWN ISSUES
  - Water reflections (the river in Episode 2) do not look right yet.
    Purely cosmetic.
  - Inside some vents the background can show as a flat brown area
    until you come out again.
  - Hair and wings can sit slightly lower than they should.
  - Short stutters when lots of enemies appear, and some slowdown
    during the Episode 3 boss.
  - Rarely the game may freeze just before the Episode 8 boss. If that
    happens, close it and restart from your last save.

REPORTING PROBLEMS
  For a crash, freeze or glitch, note roughly where it happened and
  what you were doing; a screenshot helps a lot.

REQUIREMENTS
  Windows 10 or 11, 64-bit, a DirectX 11 graphics card.
  If Windows says a "VCRUNTIME" or "MSVCP" DLL is missing, install the
  Microsoft Visual C++ Redistributable (x64):
  https://aka.ms/vs/17/release/vc_redist.x64.exe
