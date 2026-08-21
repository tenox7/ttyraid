/* aatty.c -- ANSI tty back end for AA-lib.  See aatty.h. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>

#include "aatty.h"

#define MAXCOLS 220		/* clamp: the image buffer is 2x this, and a */
#define MAXROWS 70		/* huge window would only make cells tiny */

volatile sig_atomic_t AaTtyQuit;

static char Obuf[1 << 16];
static struct termios OldTio;
static int RawOn, HeadW, HeadH;
static int CurX, CurY, CurAttr;
static volatile sig_atomic_t Resized;

/* shadow of what the terminal is showing, for the diffing present */
static unsigned char ShTxt[MAXCOLS * MAXROWS], ShAtt[MAXCOLS * MAXROWS];
static int ShW, ShH;

void aa_tty_headless(int w, int h)
{
    HeadW = w;
    HeadH = h;
}

long aa_tty_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

/* ---- display -------------------------------------------------------------- */

static void on_winch(int sig)
{
    (void)sig;
    Resized = 1;
    signal(SIGWINCH, on_winch);
}

static void on_quit(int sig)
{
    (void)sig;
    AaTtyQuit = 1;
}

static int tty_init(const struct aa_hardware_params *p, const void *data,
		    struct aa_hardware_params *dst, void **priv)
{
    (void)p; (void)data; (void)priv;

    dst->font = NULL;
    dst->supported = AA_NORMAL_MASK | AA_DIM_MASK | AA_BOLD_MASK |
		     AA_BOLDFONT_MASK | AA_REVERSE_MASK;
    dst->minwidth = 40;
    dst->minheight = 12;
    dst->maxwidth = MAXCOLS;
    dst->maxheight = MAXROWS;
    if (HeadW) return 1;

    if (!isatty(1)) return 0;
    setvbuf(stdout, Obuf, _IOFBF, sizeof Obuf);
    /* alternate screen, no cursor, no autowrap (a glyph in the bottom right
     * corner must not scroll the screen out from under us) */
    fputs("\033[?1049h\033[?25l\033[?7l\033[m\033[2J", stdout);
    fflush(stdout);
    ShW = ShH = 0;
    CurX = CurY = CurAttr = -1;
    signal(SIGWINCH, on_winch);
    signal(SIGINT, on_quit);
    signal(SIGTERM, on_quit);
    return 1;
}

static void tty_uninit(aa_context *c)
{
    (void)c;
    if (HeadW) return;
    signal(SIGWINCH, SIG_DFL);
    fputs("\033[m\033[?7h\033[?25h\033[?1049l", stdout);
    fflush(stdout);
}

static void tty_getsize(aa_context *c, int *w, int *h)
{
    struct winsize ws;

    (void)c;
    Resized = 0;
    if (HeadW) {
	*w = HeadW;
	*h = HeadH;
	return;
    }
    *w = 80;
    *h = 24;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
	*w = ws.ws_col;
	*h = ws.ws_row;
    }
    if (*w > MAXCOLS) *w = MAXCOLS;
    if (*h > MAXROWS) *h = MAXROWS;
    if (*w < 40) *w = 40;
    if (*h < 12) *h = 12;
    ShW = ShH = 0;
    CurX = CurY = CurAttr = -1;
}

static void tty_setattr(aa_context *c, int attr)
{
    static const char *const sgr[] = {
	"\033[m", "\033[0;2m", "\033[0;1m", "\033[0;1m",
	"\033[0;7m", "\033[0;7m"
    };

    (void)c;
    if (attr < 0 || attr > AA_SPECIAL) attr = AA_NORMAL;
    if (HeadW || attr == CurAttr) return;
    fputs(sgr[attr], stdout);
    CurAttr = attr;
}

static void tty_print(aa_context *c, const char *s)
{
    (void)c;
    if (HeadW) return;
    fputs(s, stdout);
    CurX += (int)strlen(s);
}

static void tty_gotoxy(aa_context *c, int x, int y)
{
    (void)c;
    if (HeadW || (x == CurX && y == CurY)) return;
    printf("\033[%d;%dH", y + 1, x + 1);
    CurX = x;
    CurY = y;
}

static void tty_flush(aa_context *c)
{
    (void)c;
    if (!HeadW) fflush(stdout);
}

static void tty_cursor(aa_context *c, int mode)
{
    (void)c;
    if (!HeadW) fputs(mode ? "\033[?25h" : "\033[?25l", stdout);
}

const struct aa_driver aa_tty_d = {
    "tty", "ANSI terminal driver",
    tty_init, tty_uninit, tty_getsize, tty_setattr,
    tty_print, tty_gotoxy, tty_flush, tty_cursor
};

/* Send only the cells that changed since the last frame.  aa_flush repaints
 * the whole screen every time, which on a scrolling full-screen game is a few
 * kilobytes of escapes per frame and visibly tears. */
void aa_tty_present(aa_context *c)
{
    const unsigned char *t = (const unsigned char *)aa_text(c);
    const unsigned char *a = (const unsigned char *)aa_attrs(c);
    int w = aa_scrwidth(c), h = aa_scrheight(c), i, n;

    if (HeadW) return;
    if (w != ShW || h != ShH) {
	ShW = w;
	ShH = h;
	memset(ShTxt, 0, sizeof ShTxt);
	memset(ShAtt, 0xff, sizeof ShAtt);
	fputs("\033[m\033[2J", stdout);
	CurX = CurY = CurAttr = -1;
    }
    n = w * h;
    if (n > (int)sizeof ShTxt) n = (int)sizeof ShTxt;
    for (i = 0; i < n; i++) {
	if (t[i] == ShTxt[i] && a[i] == ShAtt[i]) continue;
	ShTxt[i] = t[i];
	ShAtt[i] = a[i];
	tty_gotoxy(c, i % w, i / w);
	tty_setattr(c, a[i]);
	fputc(t[i], stdout);
	CurX = (i % w) == w - 1 ? -1 : CurX + 1;	/* autowrap is off */
    }
    fflush(stdout);
}

void aa_tty_repaint(void)
{
    ShW = ShH = 0;
}

/* ---- keyboard ------------------------------------------------------------- */

static int kbd_init(aa_context *c, int mode)
{
    struct termios t;

    (void)c; (void)mode;
    if (HeadW || !isatty(0)) return 1;
    if (tcgetattr(0, &OldTio)) return 1;
    t = OldTio;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_iflag &= ~(IXON | ICRNL);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    RawOn = 1;
    return 1;
}

static void kbd_uninit(aa_context *c)
{
    (void)c;
    if (RawOn) tcsetattr(0, TCSANOW, &OldTio);
    RawOn = 0;
}

/* one byte, or -1 if nothing arrives within ms (ms < 0 waits forever) */
static int readb(int ms)
{
    struct timeval tv, *tp = NULL;
    unsigned char ch;
    fd_set fds;

    FD_ZERO(&fds);
    FD_SET(0, &fds);
    if (ms >= 0) {
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	tp = &tv;
    }
    if (select(1, &fds, NULL, NULL, tp) <= 0) return -1;
    if (read(0, &ch, 1) != 1) return -1;
    return ch;
}

static int kbd_get(aa_context *c, int wait)
{
    int ch, b;

    (void)c;
    if (Resized) {
	Resized = 0;
	return AA_RESIZE;
    }
    if (HeadW) return AA_NONE;
    ch = readb(wait ? -1 : 0);
    if (ch < 0) return AA_NONE;
    if (ch != 27) return ch == 127 ? AA_BACKSPACE : ch;

    /* CSI / SS3: a lone ESC is only an ESC if nothing follows it */
    b = readb(25);
    if (b < 0) return AA_ESC;
    if (b != '[' && b != 'O') return AA_UNKNOWN;
    while ((b = readb(25)) >= 0 && b >= 0x30 && b <= 0x3f)
	;			/* skip parameter bytes */
    switch (b) {
    case 'A': return AA_UP;
    case 'B': return AA_DOWN;
    case 'C': return AA_RIGHT;
    case 'D': return AA_LEFT;
    }
    return AA_UNKNOWN;
}

const struct aa_kbddriver aa_tty_kbd_d = {
    "tty", "ANSI terminal keyboard driver",
    0, kbd_init, kbd_uninit, kbd_get
};
