PANZER DRAGOON ORTA - PC BUILD 0.8.0 (experimental)
==================================================

Thanks for testing! This is the original Xbox game translated to run
natively on Windows. It is a work in progress, so please note anything
odd (see "Reporting problems" below).

STARTING THE GAME
  Double-click "Panzer Dragoon Orta.exe". The launcher opens the first
  time (and whenever Shift is held as the game starts): choose your
  settings, then Play.

CONTROLS
  An Xbox controller (or any XInput pad) works as the original pad.
  Keyboard: arrows = D-pad, Enter = Start, Esc = Back, Z/X/A/S = A/B/X/Y,
            Q/W = triggers, C/V = White/Black.
  Alt+Enter switches between fullscreen and a window.
  F1 (or Back + Start on the pad) opens the settings menu.
  Close the window to quit (save in-game first!).

SETTINGS MENU (F1)
  The game pauses while it is open. Change the resolution, fullscreen,
  widescreen, VSync, border crop, volume and sound effects level there.
  Changes are saved to settings.ini when you close the menu; the
  description on the right says whether one needs a restart.

SAVES
  The game only saves when you choose to, at the end of each episode.
  Always say yes to the save screen after clearing an episode, or that
  episode will have to be played again. Saves go in game_files\UDATA -
  back that folder up if you want to keep them. Your existing saves are
  kept when you copy this update over the old folder.

CACHES
  The Data folder next to the game holds its caches (Data\Cache and
  Data\shader_cache). If the game misbehaves, closing it and deleting
  those two folders gives it a fresh start; nothing you need is lost.
  The first start after that takes a few minutes.

WHAT'S NEW IN THIS BUILD
  - New launcher in the style of the game's option menus.
  - Speakers = 5.1 uses the game's own Dolby Digital surround.
  - Steam Deck / Proton: ships Microsoft's shader compiler.
  - Smoother on low-end PCs (faster shader builds, more built by Setup).
  - Experimental ultrawide: RECOMP_ASPECT = 21:9 (or 32:9) under
    [Advanced] in settings.ini (the HUD is still stretched).
  - F1 menu: "Save graphics capture" for controller players.

KNOWN ISSUES
  - Episode 3: the octopus enemy's charged attack shows its hit marks near
    the octopus instead of up front on the screen.
  - Short stutters when lots of new enemies load in.

REPORTING PROBLEMS
  For a crash, freeze or glitch, note roughly where it happened and
  what you were doing; a screenshot helps a lot.

REQUIREMENTS
  Windows 10 or 11, 64-bit, a DirectX 11 graphics card.
  If Windows says a "VCRUNTIME" or "MSVCP" DLL is missing, install the
  Microsoft Visual C++ Redistributable (x64):
  https://aka.ms/vs/17/release/vc_redist.x64.exe
