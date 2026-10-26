PANZER DRAGOON ORTA - PC PLAYTEST BUILD (update 2)
=================================================

Thanks for testing! This is the original Xbox game translated to run
natively on Windows. It is a work in progress, so please note anything
odd (see "Reporting problems" below).

STARTING THE GAME
  Double-click "Panzer Dragoon Orta.exe".
  The first start takes about a minute before anything appears while the
  game sets up its cache - that is normal. Later starts are quicker.

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

WHAT'S NEW IN THIS BUILD
  - Enemy and boss health is now right (the game had been told it was
    running on a console from another region and used easier settings).
  - Water reflections now look like the original.
  - Hair and wings sit in the right place.
  - Episode 2 boss no longer flickers on its dark patches.
  - Episode 3 boss: proper shadow, and it runs at full speed.
  - Sound effects are no longer too loud or crackly (see SfxVolume).
  - Particle and sparkle effects that were missing are now drawn.
  - New settings menu (F1).

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
