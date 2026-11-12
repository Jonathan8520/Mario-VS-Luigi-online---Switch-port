/* nx_applet_screen.h -- the screen shown when the game is started in applet
 * mode (nx_applet_screen.c). MIT -- part of mvlo_nx. */
#ifndef NX_APPLET_SCREEN_H
#define NX_APPLET_SCREEN_H

/* Explains how to launch the game with full memory, waits for A, B or +, then
 * exits back to hbmenu. Falls back to fatal_error()'s text console if the
 * system font or the framebuffer is unavailable. Never returns. */
void nx_applet_mode_screen(void) __attribute__((noreturn));

#endif
