/* The settings window shown before the game starts (see launcher.c). */
#ifndef ORTA_LAUNCHER_H
#define ORTA_LAUNCHER_H

/* Shows the launcher if it is due (first start, "show every time", or Shift
 * held) and writes the choices to ini_path. Returns 0 if the player chose
 * Quit, 1 to start the game. */
int launcher_maybe_run(const char *ini_path);

#endif
