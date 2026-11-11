/* nx_kbd.h -- swkbd behind Unity's TouchScreenKeyboard (see nx_kbd.c).
 * MIT -- part of mvlo_nx. */
#ifndef NX_KBD_H
#define NX_KBD_H
void nx_kbd_show(const char *text, int type, int multiline, int secure,
                 const char *placeholder, int limit);
void nx_kbd_hide(void);
void nx_kbd_set_text(const char *text);   /* UnityPlayer.setSoftInputStr */
int  nx_kbd_is_open(void);
int  nx_kbd_take_closed(void);
void nx_kbd_pump(void);   /* render loop: delivers the result to Unity's natives */
#endif
