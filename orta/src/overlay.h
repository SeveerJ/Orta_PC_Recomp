/* In-game settings menu (Dear ImGui over the D3D11 swap chain).
 *
 * F1, or Back + Start on the pad, opens it. While it is open the game is
 * held at its frame flip (the last frame stays on screen behind the menu),
 * the game's audio is muted and its controller input is blocked. Settings
 * are written to settings.ini when the menu closes. */
#ifndef ORTA_OVERLAY_H
#define ORTA_OVERLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Hook the menu into the runtime's window, present and flip. Call once at
 * start-up, after settings.ini has been read. */
void ovl_install(void);

/* Is the menu open? */
int  ovl_is_open(void);

/* Should the game see no controller input this frame? `held` says whether
 * any button or stick is held: after the menu closes, input stays blocked
 * until everything has been released, so the press that closed the menu
 * does not reach the game. */
int  ovl_block_input(int held);

#ifdef __cplusplus
}
#endif

#endif
