/* The settings window shown before the game starts (see launcher.c). */
#ifndef ORTA_LAUNCHER_H
#define ORTA_LAUNCHER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Shows the launcher if it is due (first start, "show every time", or Shift
 * held) and writes the choices to ini_path. Returns 0 if the player chose
 * Quit, 1 to start the game. */
int launcher_maybe_run(const char *ini_path);

/* The render resolution choices, shared by the launcher and the F1 menu.
 * settings.ini keeps the value ("480", "720", "1080", "1440", "2160" or
 * "Native"). The engine draws at a whole multiple of the original 480
 * lines, the smallest that covers the choice, and the result is scaled to
 * the window. */
#define ORTA_RES_COUNT 6
int  orta_res_index(const char *ini_value);       /* any value, old ones too */
const char *orta_res_value(int index);            /* what settings.ini gets */
int  orta_res_height(int index);                  /* Native: the screen's */
int  orta_res_scale(const char *ini_value);       /* 1..6 */
void orta_res_label(int index, int widescreen, char *out, int cap);
void orta_screen_size(int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif
