/* aatty.h -- AA-lib display and keyboard drivers that speak plain ANSI to a
 * tty.  AA-lib's stock drivers want curses or slang (and a terminfo database);
 * these emit the escapes directly, so the game links against nothing but libaa
 * and libc and runs in an empty container.
 */

#ifndef AATTY_H
#define AATTY_H

#include <signal.h>
#include <aalib.h>

extern const struct aa_driver aa_tty_d;		/* display: ANSI on stdout */
extern const struct aa_kbddriver aa_tty_kbd_d;	/* keyboard: raw stdin */

extern volatile sig_atomic_t AaTtyQuit;		/* set by SIGINT / SIGTERM */

/* Render without a terminal: no escapes, no raw mode, fixed screen size.
 * Call before aa_init; used by the -shot screenshot mode. */
void aa_tty_headless(int w, int h);

void aa_tty_present(aa_context *c);		/* diffing flush: only what changed */
void aa_tty_repaint(void);			/* forget the shadow, redraw it all */

long aa_tty_ms(void);				/* monotonic-ish clock, ms */

#endif
