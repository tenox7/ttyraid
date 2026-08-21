/* TTY Raid -- a vertically scrolling shooter rendered as ASCII art with AA-lib.
 *
 * River Raid flown through the outer rim: thread a scrolling asteroid canyon,
 * shoot drones, darts and cruisers, and keep the tank filled by flying over
 * fuel pods.  Nothing here is a character: the game draws grey pixels into
 * AA-lib's virtual framebuffer (two by two pixels per character cell) and lets
 * AA-lib pick the glyph that best matches each cell, so the canyon walls get
 * real slopes and the sprites get shading.  The terminal back end is plain
 * ANSI (see aatty.c) -- no curses, no terminfo.
 *
 * Controls: LEFT/RIGHT steer, UP/DOWN throttle (1..3), SPACE fires,
 * P pauses, Q back to title, D attract mode.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aatty.h"

/* ---- tuning ---------------------------------------------------------------- */

#define TICK_MS    55		/* game heartbeat */
#define FUEL_MAX   2200
#define FUEL_LOW   550
#define SECTOR_LEN 800		/* pixel rows scrolled per sector */
#define LIFE_EVERY 10000	/* extra ship per this many points */
#define DEATH_T    28		/* ticks of death pyrotechnics */
#define OVER_T     70		/* ticks the GAME OVER card stays up */
#define IDLE_T     220		/* title ticks before the demo self-starts */

/* AA-lib maps a cell brightness of ~240 to a solid inverse-video block; above
 * that its render table wraps back to a dim glyph, so 240 is our white. */
#define AA_MAX  240

#define MAXH    140		/* playfield rows we keep terrain for */
#define BOOM_W  6		/* explosion sprite width, for centring bursts */
#define MAXEN   16
#define MAXSHOT 24
#define MAXOB   20
#define MAXBX   14
#define MAXSTAR 200

enum { E_DRONE, E_DART, E_CRUZ };
enum { O_ROID, O_ROIDS, O_FUEL };
enum { M_TITLE, M_PLAY };

#define SHIP_W 6
#define SHIP_H 4			/* the exhaust row hangs below the box */

static const int EnW[] = { 6, 6, 12 }, EnH[] = { 4, 4, 6 };
static const int ObW[] = { 8, 4, 6 }, ObH[] = { 4, 4, 6 };

typedef struct { int alive, type, x, y, hp, t, dir, fl; } Ent;
typedef struct { int alive, foe, x, y, dx; } Shot;
typedef struct { int alive, type, x, y, hp; } Obj;
typedef struct { int alive, x, y, age; } Boom;
typedef struct { int x, y, bright; } Star;

static Ent  En[MAXEN];
static Shot Sh[MAXSHOT];
static Obj  Ob[MAXOB];
static Boom Bx[MAXBX];
static Star St[MAXSTAR];
static int NStars;			/* star count, scaled to the window area */

/* canyon: wall thickness in pixels for every playfield row */
static unsigned short Lw[MAXH], Rw[MAXH];

/* terrain generator */
static int TgCanyon, TgLen, TgLw, TgRw, TgTl, TgTr, TgStep, TgFuel, TgRoid;

/* player and game state */
static int Px, Py;
static int Lives, Fuel, Speed, Score, Hi, Dist, Sector;
static int Dying, Invuln, Over, FireCd, ScrollAcc, LowWarned;
static long NextLife;
static int GTick, Paused, DemoMode, Mode, Quit, IdleT;
static int InDx, InFire;

static char Banner[48];
static int BannerT;

/* aalib context and geometry */
static aa_context *C;
static aa_renderparams Rp;
static int W, H, SW, SH, PfH;

static void start_game(int demo);
/* ---- sprites ---------------------------------------------------------------- */

/* The cast is line art, one character per cell, painted straight into AA-lib's
 * text layer after the framebuffer render: the canyon keeps its dithered rock
 * texture while the ships stay crisp.  ' ' is transparent. */

static const char *const SHIP0[] = {			/* the exhaust row */
    " A ",						/* hangs below the */
    "/=\\",						/* hit box */
    " : ", 0
};
static const char *const SHIP1[] = {
    " A ",
    "/=\\",
    " . ", 0
};
static const char *const DRONE0[] = {			/* drifting mine */
    "\\+/",
    "/+\\", 0
};
static const char *const DRONE1[] = {
    "-+-",
    "-+-", 0
};
static const char *const DART0[] = {			/* diver, nose down */
    "===",
    "\\V/", 0
};
static const char *const DART1[] = {
    "-=-",
    "\\V/", 0
};
static const char *const CRUZ0[] = {			/* gun platform */
    "/====\\",
    "|=||=|",
    "\\====/", 0
};
static const char *const CRUZ1[] = {
    "/====\\",
    "|=++=|",
    "\\====/", 0
};
static const char *const ROID[] = {
    "/##\\",
    "\\##/", 0
};
static const char *const ROIDS[] = {
    "/\\",
    "\\/", 0
};
static const char *const FUEL[] = {
    ",-.",
    "|F|",
    "'-'", 0
};
static const char *const PBOLT[] = { "|", 0 };
static const char *const EBOLT[] = { "!", 0 };
static const char *const EXP0[] = {
    "\\|/",
    "-#-",
    "/|\\", 0
};
static const char *const EXP1[] = {
    "\\ /",
    " * ",
    "/ \\", 0
};
static const char *const EXP2[] = {
    ". .",
    "   ",
    ". .", 0
};

/* 5x7 pixel font for the big lettering; bit 4 is the leftmost pixel */
static const unsigned char Font[][7] = {
    { 0, 0, 0, 0, 0, 0, 0 },					/* space */
    { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 },		/* A */
    { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e },
    { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e },
    { 0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e },
    { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f },
    { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 },
    { 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f },
    { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 },
    { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e },
    { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c },
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f },
    { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 },
    { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },
    { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e },
    { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 },
    { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d },
    { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 },
    { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e },
    { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e },
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 },
    { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1b, 0x11 },
    { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 },
    { 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04 },
    { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f },		/* Z */
    { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e },		/* 0 */
    { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e },
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f },
    { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e },
    { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 },
    { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e },
    { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e },
    { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e },
    { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c },		/* 9 */
    { 0, 0, 0, 0x1f, 0, 0, 0 }					/* - */
};

/* ---- canvas ----------------------------------------------------------------- */

static int rnd(int n)
{
    return n > 0 ? rand() % n : 0;
}

static void px(int x, int y, int v)
{
    if (x < 0 || y < 0 || x >= W || y >= PfH) return;
    aa_image(C)[y * W + x] = (unsigned char)(v > AA_MAX ? AA_MAX : v);
}

/* Stamp a sprite into the text layer.  Game coordinates are pixels, two to a
 * cell; a blank in the art leaves the rendered background showing. */
static void put_spr(const char *const *a, int x, int y, enum aa_attribute at)
{
    int r, c, cx, cy;

    x >>= 1;
    y >>= 1;
    for (r = 0; a[r]; r++)
        for (c = 0; a[r][c]; c++) {
            if (a[r][c] == 0x20) continue;
            cx = x + c;
            cy = y + r;
            if (cx < 0 || cy < 0 || cx >= SW || cy >= SH - 1) continue;
            aa_text(C)[cy * SW + cx] = a[r][c];
            aa_attrs(C)[cy * SW + cx] = at;
        }
}

/* big lettering straight into the framebuffer, scale pixels per font dot */
static int text_w(const char *s, int scale)
{
    return (int)strlen(s) * 6 * scale - scale;
}

static void draw_text(int x, int y, const char *s, int scale, int v)
{
    int i, r, c, sx, sy, g;

    x &= ~1;
    y &= ~1;
    for (i = 0; s[i]; i++, x += 6 * scale) {
        if (s[i] >= 'A' && s[i] <= 'Z') g = s[i] - 'A' + 1;
        else if (s[i] >= '0' && s[i] <= '9') g = s[i] - '0' + 27;
        else if (s[i] == '-') g = 37;
        else continue;
        for (r = 0; r < 7; r++)
            for (c = 0; c < 5; c++) {
                if (!(Font[g][r] & (0x10 >> c))) continue;
                for (sy = 0; sy < scale; sy++)
                    for (sx = 0; sx < scale; sx++)
                        px(x + c * scale + sx, y + r * scale + sy, v);
            }
    }
}

static void puts_mid(int row, const char *s, enum aa_attribute a)
{
    int x = (SW - (int)strlen(s)) / 2;
    aa_puts(C, x < 0 ? 0 : x, row, a, s);
}

/* ---- geometry helpers -------------------------------------------------------- */

static int hit(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static int wall_hit(int x, int y, int w, int h)
{
    int r;

    for (r = y; r < y + h; r++) {
        if (r < 0 || r >= PfH) continue;
        if (x < Lw[r] || x + w > W - Rw[r]) return 1;
    }
    return 0;
}

static void say(const char *s, int ticks)
{
    strncpy(Banner, s, sizeof Banner - 1);
    BannerT = ticks;
}

static void boom_at(int x, int y, int age)
{
    int i;

    for (i = 0; i < MAXBX; i++)
        if (!Bx[i].alive) {
            Bx[i].alive = 1;
            Bx[i].x = x;
            Bx[i].y = y;
            Bx[i].age = age;
            return;
        }
}

/* burst centred on a sprite w wide sitting at (x,y) */
static void boom_mid(int x, int y, int w, int age)
{
    boom_at(x + w / 2 - BOOM_W / 2, y, age);
}

/* ---- terrain generator ------------------------------------------------------- */

/* passage the walls must leave open, in pixels; tightens per sector */
static int min_pass(void)
{
    int p = W * 30 / 100 - Sector * 3;
    return p < W / 8 ? W / 8 : p;
}

static void tg_reset(void)
{
    TgCanyon = 0;
    TgLen = 90;			/* opening grace stretch */
    TgLw = TgRw = TgTl = TgTr = 0;
    TgStep = 0;
    TgFuel = 40;
    TgRoid = 20;
    memset(Lw, 0, sizeof Lw);
    memset(Rw, 0, sizeof Rw);
}

static void spawn_obj(int type, int x)
{
    int i;

    for (i = 0; i < MAXOB; i++)
        if (!Ob[i].alive) {
            Ob[i].alive = 1;
            Ob[i].type = type;
            Ob[i].x = x & ~1;
            Ob[i].y = -ObH[type];
            Ob[i].hp = type == O_ROID ? 2 : 1;
            return;
        }
}

/* generate the row that scrolls in at the top of the playfield */
static void terrain_row(void)
{
    int lo, hi, maxw, t;

    if (TgLen-- <= 0) {
        if (!TgCanyon) {
            TgCanyon = 1;		/* walls will grow from here */
            TgLen = 140 + rnd(180);
            TgStep = 0;
        } else if (!TgLw && !TgRw) {
            TgCanyon = 0;		/* ramp done: open space */
            TgLen = 100 + rnd(140);
        } else {
            TgTl = TgTr = 0;		/* ramp the walls out first */
            TgLen = 0;
        }
    }

    if (TgCanyon && TgLen > 0 && TgStep-- <= 0) {
        maxw = (W - min_pass()) / 2;	/* per side, in pixels */
        TgTl = rnd(maxw + 1);
        TgTr = rnd(maxw + 1);
        TgStep = 24 + rnd(36);
    }

    if (TgLw < TgTl) TgLw++;
    else if (TgLw > TgTl) TgLw--;
    if (TgRw < TgTr) TgRw++;
    else if (TgRw > TgTr) TgRw--;

    Lw[0] = (unsigned short)TgLw;
    Rw[0] = (unsigned short)TgRw;

    lo = TgLw + 4;			/* open pixels on the new row */
    hi = W - TgRw - 4;

    if (TgFuel-- <= 0 && hi - lo > ObW[O_FUEL] + 8) {
        spawn_obj(O_FUEL, lo + rnd(hi - lo - ObW[O_FUEL]));
        TgFuel = 46 + rnd(40);
        return;				/* keep the pod's row clean */
    }
    if (TgRoid-- <= 0 && hi - lo > ObW[O_ROID] + 6) {
        t = rnd(3) ? O_ROID : O_ROIDS;
        spawn_obj(t, lo + rnd(hi - lo - ObW[t]));
        TgRoid = (TgCanyon ? 26 : 16) + rnd(24 - (Sector > 7 ? 16 : Sector * 2));
    }
}

/* ---- spawning ---------------------------------------------------------------- */

static int SpawnT, ChainN, ChainX, ChainCd;

static int spawn_at(int type, int x)
{
    int i, w = EnW[type];

    for (i = 0; i < MAXEN; i++)
        if (!En[i].alive) {
            En[i].alive = 1;
            En[i].type = type;
            En[i].x = x & ~1;
            En[i].y = -EnH[type];
            En[i].hp = type == E_CRUZ ? 3 : 1;
            En[i].t = rnd(8);
            En[i].dir = rnd(2) ? 1 : -1;
            En[i].fl = 0;
            if (En[i].x < Lw[0] + 2) En[i].x = Lw[0] + 2;
            if (En[i].x > W - Rw[0] - 2 - w) En[i].x = W - Rw[0] - 2 - w;
            return En[i].x;
        }
    return -1;
}

static void spawn_enemy(void)
{
    int lo = Lw[0] + 2, hi = W - Rw[0] - 2;
    int r = rnd(100), type, i, x, ncruz = 0;

    if (hi - lo < 16) return;		/* canyon too tight to fly into */

    for (i = 0; i < MAXEN; i++)
        if (En[i].alive && En[i].type == E_CRUZ) ncruz++;

    if (Sector >= 2 && r >= 88 && !ncruz && hi - lo > 30) type = E_CRUZ;
    else if (Sector >= 1 && r >= 60) type = E_DART;
    else type = E_DRONE;

    x = spawn_at(type, lo + rnd(hi - lo - EnW[type]));

    if (type == E_DRONE && x >= 0 && rnd(100) < 35) {	/* column formation */
        ChainN = 2 + rnd(2);
        ChainX = x;
        ChainCd = 8;
    }
}

static void shot_at(int x, int y, int foe, int dx)
{
    int i, n = 0;

    if (!foe) {
        for (i = 0; i < MAXSHOT; i++)
            if (Sh[i].alive && !Sh[i].foe) n++;
        if (n >= 4) return;		/* four bolts in the air, tops */
    }
    for (i = 0; i < MAXSHOT; i++)
        if (!Sh[i].alive) {
            Sh[i].alive = 1;
            Sh[i].foe = foe;
            Sh[i].x = x & ~1;		/* keep bolts on the cell grid */
            Sh[i].y = y;
            Sh[i].dx = dx;
            return;
        }
}

/* ---- scoring and dying -------------------------------------------------------- */

static void add_score(int v)
{
    Score += v;
    if (Score > 999999) Score = 999999;
    if (Score >= NextLife) {
        NextLife += LIFE_EVERY;
        if (Lives < 5) Lives++;
        say("EXTRA SHIP", 30);
    }
}

static void kill_player(void)
{
    if (Dying || Over) return;
    boom_mid(Px, Py, SHIP_W, 0);
    boom_mid(Px + 3, Py - 3, SHIP_W, 2);
    boom_mid(Px - 3, Py + 1, SHIP_W, 4);
    Dying = DEATH_T;
}

static int damage_enemy(Ent *e, int sx, int sy)
{
    if (--e->hp > 0) {
        boom_at(sx - 3, sy - 2, 6);
        e->fl = 3;			/* hit feedback: flare bright */
        return 0;
    }
    e->alive = 0;
    boom_mid(e->x, e->y, EnW[e->type], 0);
    if (e->type == E_CRUZ) boom_mid(e->x, e->y - 2, 4, 2);
    add_score(e->type == E_DRONE ? 30 : e->type == E_DART ? 50 : 100);
    return 1;
}

static void respawn(void)
{
    int i, lo = Lw[Py], hi = W - Rw[Py];

    Px = (lo + hi) / 2 - SHIP_W / 2;
    if (Px < 0) Px = 0;
    if (Px > W - SHIP_W) Px = W - SHIP_W;
    Fuel = FUEL_MAX;
    LowWarned = 0;
    Speed = 2;
    Invuln = 45;
    for (i = 0; i < MAXSHOT; i++) Sh[i].alive = 0;
}

/* ---- the scroll step ---------------------------------------------------------- */

static void sector_up(void)
{
    char b[32];

    if (Sector < 99) Sector++;
    sprintf(b, "SECTOR %02d", Sector);
    say(b, 34);
    add_score(100);
}

static void do_scroll(void)
{
    int i, y;

    Dist++;
    for (y = PfH - 1; y > 0; y--) {
        Lw[y] = Lw[y - 1];
        Rw[y] = Rw[y - 1];
    }
    terrain_row();

    for (i = 0; i < NStars; i++)
        if (++St[i].y >= PfH) {
            St[i].y = 0;
            St[i].x = rnd(W);
        }
    for (i = 0; i < MAXOB; i++) {
        if (!Ob[i].alive) continue;
        if (++Ob[i].y >= PfH ||		/* off screen, or swallowed by rock */
            wall_hit(Ob[i].x, Ob[i].y, ObW[Ob[i].type], ObH[Ob[i].type]))
            Ob[i].alive = 0;
    }
    for (i = 0; i < MAXEN; i++)
        if (En[i].alive && ++En[i].y >= PfH) En[i].alive = 0;
    for (i = 0; i < MAXBX; i++)
        if (Bx[i].alive && ++Bx[i].y >= PfH) Bx[i].alive = 0;

    if (Dist % SECTOR_LEN == 0) sector_up();
}

/* ---- per-tick update ----------------------------------------------------------- */

static void move_shots(void)
{
    int i, j, k, step;

    for (i = 0; i < MAXSHOT; i++) {
        Shot *s = &Sh[i];
        if (!s->alive) continue;

        if (s->foe) {			/* enemy bolt */
            s->y += 3;
            if (GTick & 1) s->x += s->dx * 2;
            if (s->y >= PfH || wall_hit(s->x, s->y, 2, 2)) { s->alive = 0; continue; }
            if (!Dying && !Over && !Invuln &&
                hit(s->x, s->y, 2, 2, Px, Py, SHIP_W, SHIP_H)) {
                s->alive = 0;
                kill_player();
            }
            continue;
        }

        /* Player bolt climbs fast; stepped so a diver crossing our path
         * between ticks is hit rather than tunneled through. */
        for (step = 0; step < 4 && s->alive; step++) {
            if (step) s->y -= 2;
            if (s->y + 2 < 0 || wall_hit(s->x, s->y, 2, 2)) { s->alive = 0; break; }
            for (j = 0; j < MAXEN && s->alive; j++) {
                Ent *e = &En[j];
                if (!e->alive) continue;
                if (!hit(s->x, s->y, 2, 2, e->x, e->y, EnW[e->type], EnH[e->type]))
                    continue;
                s->alive = 0;
                damage_enemy(e, s->x, s->y);
            }
            for (k = 0; k < MAXOB && s->alive; k++) {
                Obj *o = &Ob[k];
                if (!o->alive) continue;
                if (!hit(s->x, s->y, 2, 2, o->x, o->y, ObW[o->type], ObH[o->type]))
                    continue;
                s->alive = 0;
                if (o->type == O_ROID && --o->hp > 0) {
                    boom_at(s->x - 3, s->y - 2, 6);
                    continue;
                }
                o->alive = 0;
                boom_mid(o->x, o->y, ObW[o->type], 0);
                add_score(o->type == O_FUEL ? 80 : o->type == O_ROID ? 20 : 10);
            }
        }
    }
}

static void move_enemies(void)
{
    int i, j, w, h, lo, hi;

    for (i = 0; i < MAXEN; i++) {
        Ent *e = &En[i];
        if (!e->alive) continue;
        e->t++;
        if (e->fl) e->fl--;
        w = EnW[e->type];
        h = EnH[e->type];

        switch (e->type) {
        case E_DRONE:			/* a mine adrift: carried by the scroll */
            if ((e->t & 3) == 2) e->x += ((e->t >> 2) & 1) ? 2 : -2;
            break;
        case E_DART:			/* dives: scroll speed plus its own */
            if (e->t & 1) {
                e->y++;
                if (e->x < Px) e->x += 2;
                else if (e->x > Px) e->x -= 2;
            }
            if (e->y >= 4 && e->y <= Py - 10 &&
                (e->t * 13 + e->x * 7) % 34 == 0) {
                shot_at(e->x + w / 2 - 1, e->y + h, 1,
                        Px > e->x ? 1 : Px < e->x ? -1 : 0);
                e->fl = 3;		/* muzzle flash telegraphs the shot */
            }
            break;
        case E_CRUZ:			/* gun platform drifting with the world */
            j = e->y < 0 ? 0 : e->y >= PfH ? PfH - 1 : e->y;
            lo = Lw[j] + 2;
            hi = W - Rw[j] - 2 - w;
            e->x += e->dir;
            if (e->x <= lo) { e->x = lo; e->dir = 1; }
            if (e->x >= hi) { e->x = hi; e->dir = -1; }
            if (e->y >= 4 && e->y <= Py - 12 && e->t % 26 == 0) {
                shot_at(e->x, e->y + h, 1, 0);
                shot_at(e->x + w - 2, e->y + h, 1, 0);
                e->fl = 3;
            }
            break;
        }

        if (e->y >= PfH) { e->alive = 0; continue; }
        if (wall_hit(e->x, e->y, w, h)) {
            e->alive = 0;
            boom_mid(e->x, e->y, w, 2);
            continue;
        }
        for (j = 0; j < MAXSHOT; j++) {	/* stepped onto a bolt in flight */
            if (!Sh[j].alive || Sh[j].foe) continue;
            if (!hit(Sh[j].x, Sh[j].y, 2, 2, e->x, e->y, w, h)) continue;
            Sh[j].alive = 0;
            if (damage_enemy(e, Sh[j].x, Sh[j].y)) break;
        }
        if (!e->alive) continue;
        if (!Dying && !Over && !Invuln &&
            hit(e->x, e->y, w, h, Px, Py, SHIP_W, SHIP_H)) {
            e->alive = 0;
            boom_mid(e->x, e->y, w, 0);
            kill_player();
        }
    }
}

static void check_objects(void)
{
    int i;

    for (i = 0; i < MAXOB; i++) {
        Obj *o = &Ob[i];
        int w = ObW[o->type], h = ObH[o->type];
        if (!o->alive) continue;
        if (o->type == O_FUEL) {
            if (!Dying && !Over && hit(o->x, o->y, w, h, Px, Py, SHIP_W, SHIP_H)) {
                Fuel += 100;		/* linger over the pod for a full tank */
                if (Fuel > FUEL_MAX) Fuel = FUEL_MAX;
                if (Fuel > FUEL_LOW) LowWarned = 0;
            }
            continue;
        }
        if (!Dying && !Over && !Invuln &&
            hit(o->x, o->y, w, h, Px, Py, SHIP_W, SHIP_H)) {
            o->alive = 0;
            boom_mid(o->x, o->y, w, 0);
            kill_player();
        }
    }
}

static void update_play(void)
{
    int i;

    GTick++;
    if (BannerT > 0) BannerT--;

    if (Over) {				/* game over card, world still rolls */
        if (--Over == 0) {
            if (!DemoMode && Score > Hi) Hi = Score;
            if (DemoMode) {
                start_game(1);		/* attract mode loops forever */
                return;
            }
            Mode = M_TITLE;
            IdleT = 0;
            return;
        }
    }

    if (Dying) {
        if ((Dying & 3) == 0)
            boom_mid(Px - 4 + rnd(9), Py - 4 + rnd(9), SHIP_W, 0);
        if (--Dying == 0) {
            if (--Lives <= 0) {
                say("GAME OVER", OVER_T + 4);
                Over = OVER_T;
            } else {
                respawn();
                say("GET READY", 24);
            }
        }
    }

    /* throttle -> scroll steps; the world freezes under the GAME OVER card */
    if (!Over) {
        ScrollAcc += Speed * 2;
        while (ScrollAcc >= 3) {
            ScrollAcc -= 3;
            do_scroll();
        }
    }

    if (!Dying && !Over) {
        Px += InDx > 8 ? 8 : InDx < -8 ? -8 : InDx;
        Px &= ~1;			/* hit box and sprite share the cell grid */
        if (Px < 0) Px = 0;
        if (Px > W - SHIP_W) Px = W - SHIP_W;
        if (FireCd > 0) FireCd--;
        if (InFire && FireCd == 0) {
            shot_at(Px + SHIP_W / 2 - 1, Py - 2, 0, 0);
            FireCd = 2;
        }
        if (Invuln > 0) Invuln--;

        Fuel -= 1 + Speed;
        if (Fuel <= FUEL_LOW && !LowWarned) {
            LowWarned = 1;
            say("LOW FUEL", 20);
        }
        if (Fuel <= 0) {
            Fuel = 0;
            Invuln = 0;
            say("OUT OF FUEL", 24);
            kill_player();
        }
        if (!Invuln && wall_hit(Px, Py, SHIP_W, SHIP_H)) kill_player();
    }
    InDx = 0;
    InFire = 0;

    move_shots();
    move_enemies();
    check_objects();

    for (i = 0; i < MAXBX; i++)
        if (Bx[i].alive && ++Bx[i].age > 14) Bx[i].alive = 0;

    if (!Over && !Dying) {
        if (ChainN > 0 && --ChainCd <= 0) {
            ChainN--;
            ChainCd = 8;
            spawn_at(E_DRONE, ChainX);
        }
        if (--SpawnT <= 0) {
            spawn_enemy();
            SpawnT = 32 - (Sector > 8 ? 16 : Sector * 2) + rnd(16);
        }
    }
}

/* ---- demo pilot ---------------------------------------------------------------- */

/* cost of sitting at column x: everything bearing down on that lane, weighted
 * by how soon it arrives.  Scoring lanes instead of reacting to the nearest
 * hazard keeps the autopilot from dithering between two bad choices. */
static int lane_cost(int x)
{
    int i, c = 0, d;

    for (i = 0; i < MAXOB; i++) {
        if (!Ob[i].alive || Ob[i].type == O_FUEL) continue;
        d = Py - Ob[i].y;
        if (d < -SHIP_H || d > 70) continue;
        if (!hit(x - 4, 0, SHIP_W + 8, 1, Ob[i].x, 0, ObW[Ob[i].type], 1))
            continue;
        c += 70 - (d > 0 ? d : 0);
    }
    for (i = 0; i < MAXEN; i++) {
        if (!En[i].alive) continue;
        d = Py - En[i].y;
        if (d < -SHIP_H || d > 30) continue;	/* further off we shoot it */
        if (!hit(x - 4, 0, SHIP_W + 8, 1, En[i].x, 0, EnW[En[i].type], 1))
            continue;
        c += 30 - (d > 0 ? d : 0);
    }
    for (i = 0; i < MAXSHOT; i++) {
        if (!Sh[i].alive || !Sh[i].foe) continue;
        d = Py - Sh[i].y;
        if (d < 0 || d > 40) continue;
        if (!hit(x - 2, 0, SHIP_W + 4, 1, Sh[i].x, 0, 2, 1)) continue;
        c += 40 - d;
    }
    return c;
}

static void demo_think(void)
{
    int i, r, lo, hi, tx, x, v, best, bestv, d;

    /* the corridor that stays open over the next dozen ticks */
    lo = 0;
    hi = W;
    for (r = Py - 40 > 0 ? Py - 40 : 0; r <= Py && r < PfH; r++) {
        if (Lw[r] > lo) lo = Lw[r];
        if (W - Rw[r] < hi) hi = W - Rw[r];
    }
    if (hi - lo < SHIP_W + 8) {
        lo = Lw[Py];
        hi = W - Rw[Py];
    }
    lo += 2;
    hi -= 2;

    tx = (lo + hi) / 2 - SHIP_W / 2;

    if (Fuel < FUEL_MAX / 3) {		/* thirsty: chase the nearest pod */
        best = -1;
        d = 9999;
        for (i = 0; i < MAXOB; i++)
            if (Ob[i].alive && Ob[i].type == O_FUEL && Ob[i].y < Py &&
                Py - Ob[i].y < d) {
                d = Py - Ob[i].y;
                best = i;
            }
        if (best >= 0) tx = Ob[best].x + ObW[O_FUEL] / 2 - SHIP_W / 2;
    } else {				/* line up on the closest raider */
        best = -1;
        d = 9999;
        for (i = 0; i < MAXEN; i++)
            if (En[i].alive && Py - En[i].y > 30 && Py - En[i].y < d) {
                d = Py - En[i].y;
                best = i;
            }
        if (best >= 0)
            tx = En[best].x + EnW[En[best].type] / 2 - SHIP_W / 2;
    }
    if (tx < lo) tx = lo;
    if (tx > hi - SHIP_W) tx = hi - SHIP_W;

    best = Px;
    bestv = 1 << 28;
    for (x = lo; x <= hi - SHIP_W; x += 2) {
        v = lane_cost(x) * 6 + (x > tx ? x - tx : tx - x);
        if (v < bestv) {
            bestv = v;
            best = x;
        }
    }
    d = best - Px;
    InDx = d > 8 ? 8 : d < -8 ? -8 : d;

    /* fire when a target sits in the gun lane; spare pods we want to drink */
    InFire = 0;
    for (i = 0; i < MAXEN; i++)
        if (En[i].alive && En[i].y < Py && Py - En[i].y < 90 &&
            hit(Px, 0, SHIP_W, 1, En[i].x, 0, EnW[En[i].type], 1))
            InFire = 1;
    for (i = 0; i < MAXOB; i++) {
        if (!Ob[i].alive || Ob[i].y >= Py || Py - Ob[i].y > 70) continue;
        if (!hit(Px, 0, SHIP_W, 1, Ob[i].x, 0, ObW[Ob[i].type], 1)) continue;
        if (Ob[i].type != O_FUEL) InFire = 1;
        else if (Fuel > FUEL_MAX * 3 / 4) InFire = 1;
        else InFire = 0;
    }

    Speed = Fuel < FUEL_MAX / 4 ? 1 : TgCanyon ? 2 : 3;
}


/* ---- drawing -------------------------------------------------------------------- */

static void draw_walls(void)
{
    int x, y, n, v;

    for (y = 0; y < PfH; y++) {
        for (x = 0, n = Lw[y]; x < n; x++) {
            v = x >= n - 2 ? 205 : 30 + 55 * x / n + (x * 37 + (Dist + y) * 101) % 8;
            px(x, y, v);
        }
        for (x = 0, n = Rw[y]; x < n; x++) {
            v = x >= n - 2 ? 205 : 30 + 55 * x / n + (x * 37 + (Dist + y) * 101) % 8;
            px(W - 1 - x, y, v);
        }
    }
}

static void draw_hud(void)
{
    int row = SH - 1, x = 0, i, n, ships = Lives - 1 > 4 ? 4 : Lives - 1;
    char b[96];
    enum aa_attribute a;

    memset(aa_text(C) + row * SW, ' ', SW);
    memset(aa_attrs(C) + row * SW, AA_NORMAL, SW);

    sprintf(b, " SCORE %06d  ", Score);
    aa_puts(C, x, row, AA_BOLD, b);
    x += (int)strlen(b);

    a = Fuel <= FUEL_LOW && (GTick >> 2 & 1) ? AA_REVERSE : AA_BOLD;
    aa_puts(C, x, row, a, "FUEL ");
    x += 5;
    n = Fuel * 12 / FUEL_MAX;
    for (i = 0; i < 12 && x < SW; i++, x++)
        aa_puts(C, x, row, i < n ? AA_REVERSE : AA_NORMAL, i < n ? " " : "-");

    /* on a narrow terminal the score and the tank are all that fits */
    sprintf(b, "  SPD %d  SEC %02d  SHIPS ", Speed, Sector);
    if (x + (int)strlen(b) + ships < SW) {
        aa_puts(C, x, row, AA_BOLD, b);
        x += (int)strlen(b);
        for (i = 0; i < ships; i++, x++)
            aa_puts(C, x, row, AA_BOLD, "A");
    }
    if (DemoMode && x + 5 < SW)
        aa_puts(C, SW - 5, row, (GTick >> 2 & 1) ? AA_REVERSE : AA_BOLD, "DEMO");
}

static void draw_play(void)
{
    int i, frame = (GTick >> 2) & 1;

    /* scenery first: stars and canyon go through AA-lib's renderer */
    memset(aa_image(C), 0, W * H);
    for (i = 0; i < NStars; i++)
        px(St[i].x, St[i].y, St[i].bright ? 95 : 45);
    draw_walls();
    aa_render(C, &Rp, 0, 0, SW, SH - 1);

    /* then the cast, stamped over it as characters */
    for (i = 0; i < MAXOB; i++) {
        if (!Ob[i].alive) continue;
        put_spr(Ob[i].type == O_ROID ? ROID :
                Ob[i].type == O_ROIDS ? ROIDS : FUEL,
                Ob[i].x, Ob[i].y,
                Ob[i].type == O_FUEL ? AA_REVERSE : AA_BOLD);
    }
    for (i = 0; i < MAXEN; i++) {
        Ent *e = &En[i];
        if (!e->alive) continue;
        put_spr(e->type == E_DRONE ? (frame ? DRONE1 : DRONE0) :
                e->type == E_DART ? (frame ? DART1 : DART0) :
                (frame ? CRUZ1 : CRUZ0),
                e->x, e->y, e->fl ? AA_REVERSE : AA_BOLD);
    }
    for (i = 0; i < MAXSHOT; i++)
        if (Sh[i].alive)
            put_spr(Sh[i].foe ? EBOLT : PBOLT, Sh[i].x, Sh[i].y, AA_BOLD);

    if (!Dying && !Over && (!Invuln || (GTick & 1)))
        put_spr(frame ? SHIP1 : SHIP0, Px, Py, AA_BOLD);

    for (i = 0; i < MAXBX; i++) {
        if (!Bx[i].alive) continue;
        put_spr(Bx[i].age < 4 ? EXP0 : Bx[i].age < 9 ? EXP1 : EXP2,
                Bx[i].x, Bx[i].y, Bx[i].age < 4 ? AA_REVERSE :
                Bx[i].age < 9 ? AA_BOLD : AA_NORMAL);
    }

    draw_hud();

    if (BannerT > 0 || Paused || Over)
        puts_mid(Paused || Over ? (SH - 1) / 2 : SH - 4,
                 Paused ? "   PAUSED   " : Banner, AA_REVERSE);

    aa_tty_present(C);
}
static void draw_title(void)
{
    int i, gx, scale, cx = W / 2, frame = (GTick >> 2) & 1;
    int lrow = 2, lh, srow, hrow, grow, brow, prow, krow;
    char b[40];

    memset(aa_image(C), 0, W * H);
    for (i = 0; i < NStars; i++)
        px(St[i].x, St[i].y, St[i].bright ? 95 : 45);

    /* even scales only: an odd one would straddle the 2-pixel cell grid */
    scale = W >= 240 && SH >= 34 ? 4 : 2;
    if (text_w("TTY RAID", scale) > W - 8) scale = 2;
    lh = (7 * scale + 1) / 2;
    draw_text(cx - text_w("TTY RAID", scale) / 2, lrow * 2, "TTY RAID", scale,
              AA_MAX);

    srow = lrow + lh;
    hrow = srow + 1;
    grow = hrow + 2;
    brow = grow + 4;
    prow = brow + 2;
    krow = prow + 2;

    aa_render(C, &Rp, 0, 0, SW, SH - 1);

    gx = SW / 2 - 18;
    if (gx >= 0 && brow < SH - 1) {
        put_spr(frame ? SHIP1 : SHIP0, gx * 2, (grow + 1) * 2, AA_BOLD);
        put_spr(frame ? DRONE1 : DRONE0, (gx + 6) * 2, (grow + 1) * 2, AA_BOLD);
        put_spr(frame ? DART1 : DART0, (gx + 12) * 2, (grow + 1) * 2, AA_BOLD);
        put_spr(frame ? CRUZ1 : CRUZ0, (gx + 18) * 2, grow * 2, AA_BOLD);
        put_spr(ROID, (gx + 26) * 2, (grow + 1) * 2, AA_BOLD);
        put_spr(FUEL, (gx + 32) * 2, grow * 2, AA_REVERSE);
    }

    puts_mid(srow, "AN ASCII SPACE SHOOTER RENDERED WITH AA-LIB", AA_NORMAL);
    sprintf(b, "HIGH SCORE  %06d", Hi);
    puts_mid(hrow, b, AA_BOLD);

    if (gx >= 0 && brow < SH - 1) {
        aa_puts(C, gx, brow, AA_NORMAL, "YOU");
        aa_puts(C, gx + 6, brow, AA_NORMAL, "30");
        aa_puts(C, gx + 12, brow, AA_NORMAL, "50");
        aa_puts(C, gx + 18, brow, AA_NORMAL, "100");
        aa_puts(C, gx + 26, brow, AA_NORMAL, "20");
        aa_puts(C, gx + 32, brow, AA_NORMAL, "FUEL");
    }
    if (prow < SH - 1)
        puts_mid(prow, ">>>  PRESS SPACE TO LAUNCH  <<<",
                 (GTick >> 2 & 1) ? AA_REVERSE : AA_BOLD);
    if (krow + 1 < SH - 1) {
        puts_mid(krow, "ARROWS STEER AND THROTTLE     SPACE FIRES", AA_NORMAL);
        puts_mid(krow + 1, "P PAUSE     D DEMO     Q QUIT", AA_NORMAL);
    }
    if (krow + 3 < SH - 1)
        puts_mid(krow + 3, "FLY OVER FUEL PODS TO REFUEL - MIND THE ROCKS",
                 AA_NORMAL);

    memset(aa_text(C) + (SH - 1) * SW, ' ', SW);
    memset(aa_attrs(C) + (SH - 1) * SW, AA_NORMAL, SW);
    puts_mid(SH - 1, "github.com/tenox7/ttyraid", AA_NORMAL);

    aa_tty_present(C);
}


/* ---- setup ------------------------------------------------------------------------ */

static void layout(void)
{
    W = aa_imgwidth(C);			/* two pixels per character cell */
    H = aa_imgheight(C);
    SW = aa_scrwidth(C);
    SH = aa_scrheight(C);
    PfH = (SH - 1) * 2;			/* the bottom text row is the status bar */
    if (PfH > H) PfH = H;
    if (PfH > MAXH) PfH = MAXH;
    Py = PfH - SHIP_H - 6;
    if (Px > W - SHIP_W) Px = W - SHIP_W;
}

static void init_stars(void)
{
    int i;

    NStars = W * PfH / 150;
    if (NStars > MAXSTAR) NStars = MAXSTAR;
    if (NStars < 20) NStars = 20;
    for (i = 0; i < NStars; i++) {
        St[i].x = rnd(W);
        St[i].y = rnd(PfH);
        St[i].bright = rnd(4) == 0;
    }
}

static void start_game(int demo)
{
    int i;

    DemoMode = demo;
    Lives = 3;
    Score = 0;
    Sector = 1;
    Dist = 0;
    Fuel = FUEL_MAX;
    Speed = 2;
    Dying = Over = 0;
    Invuln = 30;
    FireCd = 0;
    ScrollAcc = 0;
    LowWarned = 0;
    NextLife = LIFE_EVERY;
    Paused = 0;
    SpawnT = 24;
    ChainN = 0;
    InDx = InFire = 0;
    Px = (W - SHIP_W) / 2;
    for (i = 0; i < MAXEN; i++) En[i].alive = 0;
    for (i = 0; i < MAXSHOT; i++) Sh[i].alive = 0;
    for (i = 0; i < MAXOB; i++) Ob[i].alive = 0;
    for (i = 0; i < MAXBX; i++) Bx[i].alive = 0;
    tg_reset();
    say("SECTOR 01", 30);
    Mode = M_PLAY;
}

/* ---- input ------------------------------------------------------------------------- */

static void to_title(void)
{
    if (!DemoMode && Score > Hi) Hi = Score;
    Mode = M_TITLE;
    IdleT = 0;
    Paused = 0;
}

static void handle_key(int k)
{
    if (k == 3) {
        Quit = 1;
        return;
    }
    if (k == 12) {			/* Ctrl-L: redraw */
        aa_tty_repaint();
        return;
    }
    if (Mode == M_PLAY && DemoMode) {	/* attract mode: any key wakes it */
        to_title();
        return;
    }
    if (Mode == M_TITLE) {
        IdleT = 0;
        switch (k) {
        case ' ': start_game(0); break;
        case 'd': case 'D': start_game(1); break;
        case 'q': case 'Q': case AA_ESC: Quit = 1; break;
        }
        return;
    }

    switch (k) {
    case AA_LEFT: case 'a': case 'A': case 'h': case 'H':
        InDx -= 4;
        break;
    case AA_RIGHT: case 'd': case 'D': case 'l': case 'L':
        InDx += 4;
        break;
    case AA_UP: case 'w': case 'W': case 'k': case 'K':
        if (Speed < 3) Speed++;
        break;
    case AA_DOWN: case 's': case 'S': case 'j': case 'J':
        if (Speed > 1) Speed--;
        break;
    case ' ':
        InFire = 1;
        break;
    case 'p': case 'P':
        Paused = !Paused;
        break;
    case 'q': case 'Q': case AA_ESC:
        to_title();
        break;
    }
}

/* Sit on the keyboard until the next tick is due.  Autorepeat on a held arrow
 * arrives as a stream of reports, so steering accumulates in InDx. */
static void pump(long until)
{
    int k;

    while (aa_tty_ms() < until && !Quit) {
        k = aa_getevent(C, 0);
        if (k == AA_RESIZE) {
            aa_resize(C);
            layout();
            init_stars();
            if (Mode == M_PLAY && Invuln < 30) Invuln = 30;
            continue;
        }
        if (k == AA_NONE) {
            struct timespec ts = { 0, 4000000 };
            nanosleep(&ts, NULL);
            continue;
        }
        handle_key(k);
    }
}

/* ---- main -------------------------------------------------------------------------- */

static void shot(const char *path)
{
    FILE *f = fopen(path, "w");
    int x, y;

    if (!f) return;
    for (y = 0; y < SH; y++) {
        for (x = 0; x < SW; x++) fputc(aa_text(C)[y * SW + x], f);
        fputc('\n', f);
    }
    fclose(f);
}


/* PGM of what the terminal would show: the AA-lib font rasterised through the
 * text and attribute buffers.  Handy for eyeballing the art off-terminal. */
static void preview(const char *path)
{
    const struct aa_font *f = aa_currentfont(C);
    int fh = f->height, x, y, r, c, on, a;
    unsigned char bits;
    FILE *fp = fopen(path, "wb");

    if (!fp) return;
    fprintf(fp, "P5\n%d %d\n255\n", SW * 8, SH * fh);
    for (y = 0; y < SH; y++)
        for (r = 0; r < fh; r++)
            for (x = 0; x < SW; x++) {
                bits = f->data[(unsigned char)aa_text(C)[y * SW + x] * fh + r];
                a = aa_attrs(C)[y * SW + x];
                for (c = 0; c < 8; c++) {
                    on = (bits >> (7 - c)) & 1;
                    if (a == AA_REVERSE || a == AA_SPECIAL) on = !on;
                    fputc(on ? (a == AA_DIM ? 140 : 255) : 0, fp);
                }
            }
    fclose(fp);
}

static void usage(const char *prog)
{
    printf("TTY Raid - an ASCII scrolling shooter drawn with AA-lib\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("  -h          this help\n");
    printf("  -d          start in self-playing attract mode\n");
    printf("  -f          Floyd-Steinberg dithering (softer, noisier)\n");
    printf("  -b N        brightness bias, 0..255\n");
    printf("  -c N        contrast, 0..127\n");
    printf("  -g N.N      gamma\n");
    printf("  -s FILE     headless: dump one frame as text and exit\n");
    printf("  -p FILE     headless: dump one frame as a PGM image\n");
    printf("  -S CxR      screen size for -s and -p (default 80x24)\n");
    printf("  -n N        ticks to run before -s -d dumps its frame\n\n");
    printf("Arrows steer and throttle, Space fires, P pauses, Q quits.\n");
}

int main(int argc, char *argv[])
{
    struct aa_hardware_params hp = aa_defparams;
    char *shotf = NULL, *prevf = NULL;
    int i, demo = 0, sw = 80, sh = 24, nticks = 260;
    long next;

    Rp = aa_defrenderparams;
    Rp.dither = AA_NONE;		/* crisp edges beat dither shimmer */

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--demo")) {
            demo = 1;
        } else if (!strcmp(argv[i], "-f")) {
            Rp.dither = AA_FLOYD_S;
        } else if (!strcmp(argv[i], "-b") && i + 1 < argc) {
            Rp.bright = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            Rp.contrast = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-g") && i + 1 < argc) {
            Rp.gamma = atof(argv[++i]);
        } else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            shotf = argv[++i];
        } else if (!strcmp(argv[i], "-S") && i + 1 < argc) {
            sscanf(argv[++i], "%dx%d", &sw, &sh);
        } else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            prevf = argv[++i];
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            nticks = atoi(argv[++i]);
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    srand(shotf || prevf ? 12345 : (unsigned)time(NULL));
    hp.supported = AA_NORMAL_MASK | AA_DIM_MASK | AA_BOLD_MASK | AA_REVERSE_MASK;
    if (shotf || prevf) aa_tty_headless(sw, sh);

    C = aa_init(&aa_tty_d, &hp, NULL);
    if (!C) {
        fprintf(stderr, "ttyraid: cannot open the terminal\n");
        return 1;
    }
    aa_initkbd(C, &aa_tty_kbd_d, 0);
    aa_hidecursor(C);
    layout();
    init_stars();

    if (shotf || prevf) {
        if (demo) {
            start_game(1);
            for (i = 0; i < nticks; i++) {
                demo_think();
                update_play();
            }
            draw_play();
        } else {
            Mode = M_TITLE;
            draw_title();
        }
        if (shotf) shot(shotf);
        if (prevf) preview(prevf);
        aa_uninitkbd(C);
        aa_close(C);
        return 0;
    }

    if (demo) start_game(1);
    else Mode = M_TITLE;

    next = aa_tty_ms() + TICK_MS;
    while (!Quit && !AaTtyQuit) {
        pump(next);
        if (Quit || AaTtyQuit) break;

        if (Mode == M_TITLE) {
            GTick++;
            IdleT++;
            draw_title();
            if (IdleT > IDLE_T) start_game(1);
        } else {
            if (DemoMode && !Dying && !Over) demo_think();
            if (!Paused) update_play();
            draw_play();
        }

        next += TICK_MS;
        if (aa_tty_ms() > next + 400) next = aa_tty_ms();
    }

    aa_uninitkbd(C);
    aa_close(C);
    return 0;
}
