/*
 * STAR BREAKER for X68000 - elf2x68k, C++, no Human68k required.
 *
 * Build targets (see Makefile):
 *   breakout.sys - bootable system file. Put it on a disk as HUMAN.SYS
 *                  (make_boot_xdf.py does that) and the X68000 IPL boots
 *                  it directly, with no Human68k on the disk.
 *   breakout.x   - the same game as a Human68k .X executable.
 *
 * Display : CRTMOD 12 retimed to 60 Hz (CRTC pokes, pong-style),
 *           512x480 visible, 65536 colors. Frame pacing is locked
 *           to the VDISP signal (MFP GPIP) for flicker-free updates.
 * Input   : keyboard (cursor Left/Right, SPACE, alphabet keys, ESC)
 *           and joystick/joypad port 1+2 via IOCS JOYGET.
 * Sound   : YM2151 (OPM) via IOCS OPMSET. BGM sequencer (lead+bass)
 *           plus SE blips on separate channels.
 * Keys    : <-/-> or A/D or Z/X .. move paddle
 *           SPACE ................ launch ball / start / retry
 *           P .................... pause    M .... sound on/off
 *           ESC .................. back to title (on title: exit/halt)
 */

#include <x68k/iocs.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* constants                                                           */
/* ------------------------------------------------------------------ */

static constexpr int SCR_W = 512;
static constexpr int SCR_H = 480;            /* visible lines at 60 Hz */
static constexpr int CRT_MODE = 12;          /* 512x512 GVRAM, 65536 colors */

/* play field (inside the frame) */
static constexpr int FR_X0 = 8,  FR_Y0 = 44;   /* frame outer */
static constexpr int FR_X1 = 503, FR_Y1 = 471;
static constexpr int WALL_L = 16;              /* ball center limits */
static constexpr int WALL_R = 496;
static constexpr int WALL_T = 52;
static constexpr int FLOOR_Y = 468;            /* center below = miss */

static constexpr int PADDLE_W = 80;            /* 5 sprite tiles of 16 */
static constexpr int PADDLE_H = 10;
static constexpr int PADDLE_Y = 448;
static constexpr int PADDLE_SPEED = 5;         /* px per 1/50 s (scaled) */

static constexpr int BALL_R = 5;

static constexpr int BLK_COLS = 10;
static constexpr int BLK_ROWS = 6;
static constexpr int BLK_X = 21, BLK_Y = 80;
static constexpr int BLK_W = 41, BLK_H = 14;
static constexpr int BLK_PX = 47, BLK_PY = 19;

static constexpr int FP = 8;                   /* fixed point shift */
static constexpr int MAX_BALLS = 3;

/* 1 = show tiny key/joy debug readout (for input calibration builds;
   enable with -DDEBUG_HUD=1). Release builds keep it off. */
#ifndef DEBUG_HUD
#define DEBUG_HUD 0
#endif

/* 65536-color format: G(5) R(5) B(5) I(1) */
static constexpr uint16_t GRB(int r, int g, int b) {
  return (uint16_t)(((g & 31) << 11) | ((r & 31) << 6) | ((b & 31) << 1) | 1);
}
static constexpr uint16_t COL_BLACK = GRB(0, 0, 0);
static constexpr uint16_t COL_WHITE = GRB(31, 31, 31);
static constexpr uint16_t COL_BALL  = GRB(31, 30, 18);
static constexpr uint16_t COL_FRAME = GRB(2, 22, 28);
static constexpr uint16_t COL_FRAME_HI = GRB(10, 30, 31);
static constexpr uint16_t COL_PADDLE = GRB(12, 20, 30);
static constexpr uint16_t COL_PADDLE_HI = GRB(26, 31, 31);
static constexpr uint16_t COL_PADDLE_END = GRB(31, 18, 4);
static constexpr uint16_t COL_PANEL = GRB(2, 3, 10);
static constexpr uint16_t COL_PANEL_BD = GRB(4, 26, 30);

/* background vertical gradient (recomputed per row) */
static uint16_t bg_color(int y) {
  int t = y * 31 / SCR_H;              /* 0..31 */
  int r = 3 - (t * 3 / 31);
  int g = 4 - (t * 2 / 31);
  int b = 13 - (t * 9 / 31);
  return GRB(r, g, b);
}

/* ------------------------------------------------------------------ */
/* 60 Hz retiming + VDISP sync (technique from renatus-novus-x/pong)   */
/* ------------------------------------------------------------------ */

#define X68K_CRTC_BASE 0x00E80000UL
#define X68K_MFP_GPIP  0x00E88001UL
#define GPIP_VDISP_BIT 0x10

static int gpip_vdisp() {
  return _iocs_b_bpeek((const void *)X68K_MFP_GPIP) & GPIP_VDISP_BIT;
}

/* wait for the VDISP signal to rise; -1 on 1 s timeout */
static int wait_vdisp_edge() {
  int start = _iocs_ontime().sec;
  int level = gpip_vdisp();
  for (;;) {
    int next = gpip_vdisp();
    if (_iocs_ontime().sec - start > 100) return -1;
    if (next != level) {
      level = next;
      if (level != 0) return 0;
    }
  }
}

static void crtc_poke(int reg, int value) {
  _iocs_b_wpoke((void *)(X68K_CRTC_BASE + reg * 2), value);
}

/* CRTMOD 12 defaults to ~55 Hz with 512 visible lines. Retiming the
   CRTC inside the vertical blank (same values as pong) gives a
   524-line total, i.e. ~60 Hz, with 480 visible lines. Restoring the
   CRT mode on exit resets these registers. */
static void set_60hz() {
  if (wait_vdisp_edge() != 0) return;
  crtc_poke(5, 0x0001);
  crtc_poke(6, 0x0022);
  crtc_poke(7, 0x0202);
  crtc_poke(4, 0x020C);
}

/* ------------------------------------------------------------------ */
/* 8x8 font: "8x8 ORIGINAL FONT 1 fix" from the MSXPen shared code     */
/* (https://msxpen.com/codes/-Nq8q6wabU6mJ4onDmYE), glyphs for ASCII   */
/* 0x20..0x7F, row-major, bit7 = leftmost pixel. The same font as      */
/* used in the MSX version of breakout.                                */
/* ------------------------------------------------------------------ */

static const uint8_t FONT8[96][8] = {
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
  {0x10,0x10,0x10,0x10,0x10,0x00,0x10,0x00},
  {0x24,0x24,0x48,0x00,0x00,0x00,0x00,0x00},
  {0x44,0xFE,0x44,0x44,0x44,0xFE,0x44,0x00},
  {0x10,0x7C,0x90,0x7C,0x12,0xFC,0x10,0x00},
  {0xE2,0xA4,0xE8,0x10,0x2E,0x4A,0x8E,0x00},
  {0x30,0x48,0x30,0x62,0x94,0x8C,0x72,0x00},
  {0x20,0x20,0x40,0x00,0x00,0x00,0x00,0x00},
  {0x08,0x10,0x20,0x20,0x20,0x10,0x08,0x00},
  {0x20,0x10,0x08,0x08,0x08,0x10,0x20,0x00},
  {0x00,0x00,0x28,0x10,0x28,0x00,0x00,0x00},
  {0x00,0x00,0x10,0x38,0x10,0x00,0x00,0x00},
  {0x00,0x00,0x00,0x00,0x40,0x40,0x80,0x00},
  {0x00,0x00,0x00,0x7C,0x00,0x00,0x00,0x00},
  {0x00,0x00,0x00,0x00,0x00,0x40,0x00,0x00},
  {0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x00},
  {0x7C,0x86,0x8A,0x92,0xA2,0xC2,0x7C,0x00},
  {0x10,0x30,0x50,0x10,0x10,0x10,0x10,0x00},
  {0xFC,0x02,0x02,0x7C,0x80,0x80,0xFE,0x00},
  {0xFC,0x02,0x02,0x7C,0x02,0x02,0xFC,0x00},
  {0x82,0x82,0x82,0xFE,0x02,0x02,0x02,0x00},
  {0xFE,0x80,0x80,0xFC,0x02,0x02,0xFC,0x00},
  {0x7C,0x80,0x80,0xFC,0x82,0x82,0x7C,0x00},
  {0xFE,0x82,0x82,0x02,0x02,0x02,0x02,0x00},
  {0x7C,0x82,0x82,0x7C,0x82,0x82,0x7C,0x00},
  {0x7C,0x82,0x82,0x7E,0x02,0x02,0x7C,0x00},
  {0x00,0x30,0x30,0x00,0x30,0x30,0x00,0x00},
  {0x00,0x30,0x30,0x00,0x30,0x30,0x60,0x00},
  {0x06,0x18,0x60,0x80,0x60,0x18,0x06,0x00},
  {0x00,0x00,0xFE,0x00,0x00,0xFE,0x00,0x00},
  {0xC0,0x30,0x0C,0x02,0x0C,0x30,0xC0,0x00},
  {0x38,0x44,0x44,0x08,0x10,0x00,0x10,0x00},
  {0x38,0x44,0x9A,0xAA,0xAA,0xAC,0x38,0x00},
  {0x7C,0x82,0x82,0xFE,0x82,0x82,0x82,0x00},
  {0xFC,0x82,0x82,0xFC,0x82,0x82,0xFC,0x00},
  {0x7E,0x80,0x80,0x80,0x80,0x80,0x7E,0x00},
  {0xF8,0x84,0x82,0x82,0x82,0x84,0xF8,0x00},
  {0xFE,0x80,0x80,0xFC,0x80,0x80,0xFE,0x00},
  {0xFE,0x80,0x80,0xFC,0x80,0x80,0x80,0x00},
  {0x7C,0x80,0x80,0x9E,0x82,0x82,0x7C,0x00},
  {0x82,0x82,0x82,0xFE,0x82,0x82,0x82,0x00},
  {0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x00},
  {0x02,0x02,0x02,0x02,0x82,0x82,0x7C,0x00},
  {0x82,0x84,0x88,0xF0,0x88,0x84,0x82,0x00},
  {0x80,0x80,0x80,0x80,0x80,0x80,0xFE,0x00},
  {0x82,0xC6,0xAA,0x92,0x82,0x82,0x82,0x00},
  {0x82,0xC2,0xA2,0x92,0x8A,0x86,0x82,0x00},
  {0x7C,0x82,0x82,0x82,0x82,0x82,0x7C,0x00},
  {0xFC,0x82,0x82,0xFC,0x80,0x80,0x80,0x00},
  {0x7C,0x82,0x82,0x92,0x8A,0x84,0x7A,0x00},
  {0xFC,0x82,0x82,0xFC,0x88,0x84,0x82,0x00},
  {0x7C,0x82,0x80,0x7C,0x02,0x82,0x7C,0x00},
  {0xFE,0x10,0x10,0x10,0x10,0x10,0x10,0x00},
  {0x82,0x82,0x82,0x82,0x82,0x82,0x7C,0x00},
  {0x82,0x82,0x44,0x44,0x28,0x28,0x10,0x00},
  {0x92,0x92,0x92,0xAA,0xAA,0x44,0x44,0x00},
  {0x82,0x44,0x28,0x10,0x28,0x44,0x82,0x00},
  {0x82,0x44,0x28,0x10,0x10,0x10,0x10,0x00},
  {0xFE,0x04,0x08,0x10,0x20,0x40,0xFE,0x00},
  {0x38,0x20,0x20,0x20,0x20,0x20,0x38,0x00},
  {0x82,0x44,0x28,0x7C,0x10,0x7C,0x10,0x00},
  {0x38,0x08,0x08,0x08,0x08,0x08,0x38,0x00},
  {0x10,0x28,0x44,0x82,0x00,0x00,0x00,0x00},
  {0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00},
  {0x20,0x10,0x08,0x00,0x00,0x00,0x00,0x00},
  {0x00,0x00,0x38,0x04,0x3C,0x44,0x3C,0x00},
  {0x40,0x40,0x78,0x44,0x44,0x44,0x78,0x00},
  {0x00,0x00,0x3C,0x40,0x40,0x40,0x3C,0x00},
  {0x04,0x04,0x3C,0x44,0x44,0x44,0x3C,0x00},
  {0x00,0x00,0x38,0x44,0x7C,0x40,0x38,0x00},
  {0x0C,0x10,0x7C,0x10,0x10,0x10,0x10,0x00},
  {0x00,0x00,0x3C,0x44,0x3C,0x04,0x38,0x00},
  {0x40,0x40,0x78,0x44,0x44,0x44,0x44,0x00},
  {0x10,0x00,0x10,0x10,0x10,0x10,0x10,0x00},
  {0x08,0x00,0x08,0x08,0x08,0x48,0x30,0x00},
  {0x20,0x20,0x24,0x28,0x30,0x28,0x24,0x00},
  {0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x00},
  {0x00,0x00,0x68,0x54,0x54,0x54,0x54,0x00},
  {0x00,0x00,0x78,0x44,0x44,0x44,0x44,0x00},
  {0x00,0x00,0x38,0x44,0x44,0x44,0x38,0x00},
  {0x00,0x00,0x78,0x44,0x44,0x78,0x40,0x00},
  {0x00,0x00,0x38,0x44,0x44,0x3C,0x04,0x00},
  {0x00,0x00,0x58,0x64,0x40,0x40,0x40,0x00},
  {0x00,0x00,0x3C,0x40,0x38,0x04,0x78,0x00},
  {0x00,0x10,0x7C,0x10,0x10,0x10,0x0C,0x00},
  {0x00,0x00,0x44,0x44,0x44,0x44,0x3C,0x00},
  {0x00,0x00,0x44,0x44,0x28,0x28,0x10,0x00},
  {0x00,0x00,0x54,0x54,0x54,0x54,0x28,0x00},
  {0x00,0x00,0x44,0x28,0x10,0x28,0x44,0x00},
  {0x00,0x00,0x44,0x44,0x3C,0x04,0x38,0x00},
  {0x00,0x00,0x7C,0x08,0x10,0x20,0x7C,0x00},
  {0x18,0x20,0x20,0x40,0x20,0x20,0x18,0x00},
  {0x10,0x10,0x10,0x00,0x10,0x10,0x10,0x00},
  {0x30,0x08,0x08,0x04,0x08,0x08,0x30,0x00},
  {0x22,0x54,0x88,0x00,0x00,0x00,0x00,0x00},
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

static const uint8_t *glyph_for(char c) {
  unsigned char u = (unsigned char)c;
  if (u < 0x20 || u > 0x7f) u = 0x20;
  return FONT8[u - 0x20];
}

/* ------------------------------------------------------------------ */
/* graphics helpers (IOCS)                                             */
/* ------------------------------------------------------------------ */

class Gfx {
public:
  static void fill(int x1, int y1, int x2, int y2, uint16_t color) {
    if (x1 > x2 || y1 > y2) return;
    if (x2 < 0 || y2 < 0 || x1 >= SCR_W || y1 >= SCR_H) return;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= SCR_W) x2 = SCR_W - 1;
    if (y2 >= SCR_H) y2 = SCR_H - 1;
    iocs_fillptr f = {(short)x1, (short)y1, (short)x2, (short)y2, color};
    _iocs_fill(&f);
  }
  static void line(int x1, int y1, int x2, int y2, uint16_t color) {
    iocs_lineptr l = {(short)x1, (short)y1, (short)x2, (short)y2,
                      color, 0xffff};
    _iocs_line(&l);
  }
  static void pset(int x, int y, uint16_t color) {
    if (x < 0 || y < 0 || x >= SCR_W || y >= SCR_H) return;
    iocs_psetptr p = {(short)x, (short)y, color};
    _iocs_pset(&p);
  }
  static int text_width(const char *s, int scale) {
    return (int)strlen(s) * 8 * scale;
  }
  static void text(int x, int y, int scale, uint16_t color,
                   const char *s) {
    for (int i = 0; s[i]; i++) {
      const uint8_t *g = glyph_for(s[i]);
      for (int row = 0; row < 8; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 8; col++) {
          if (bits & (0x80 >> col))
            fill(x + col * scale, y + row * scale,
                 x + col * scale + scale - 1,
                 y + row * scale + scale - 1, color);
        }
      }
      x += 8 * scale;
    }
  }
  static void text_center(int y, int scale, uint16_t color,
                          const char *s) {
    text((SCR_W - text_width(s, scale)) / 2, y, scale, color, s);
  }
  /* number with leading zeros, width digits */
  static void number(int x, int y, int scale, uint16_t color,
                     int value, int width) {
    char buf[16];
    for (int i = width - 1; i >= 0; i--) {
      buf[i] = (char)('0' + (value % 10));
      value /= 10;
    }
    buf[width] = 0;
    text(x, y, scale, color, buf);
  }
};

/* starfield, fixed pseudo-random positions */
static constexpr int STAR_N = 110;
static int star_x[STAR_N], star_y[STAR_N], star_b[STAR_N];

static void stars_init() {
  uint32_t s = 0x12345678u;
  for (int i = 0; i < STAR_N; i++) {
    s = s * 1103515245u + 12345u;
    star_x[i] = (int)((s >> 8) % SCR_W);
    s = s * 1103515245u + 12345u;
    star_y[i] = (int)((s >> 8) % SCR_H);
    s = s * 1103515245u + 12345u;
    star_b[i] = (int)((s >> 8) % 3);
  }
}

static uint16_t star_color(int b) {
  switch (b) {
    case 0: return GRB(6, 6, 10);
    case 1: return GRB(10, 12, 16);
    default: return GRB(16, 16, 12);
  }
}

/* ------------------------------------------------------------------ */
/* direct GVRAM access (mode 12: 0xC00000, 1024 bytes per line).       */
/* The .sys runs in supervisor mode; the .X build enters supervisor    */
/* mode in Game::run() so these are safe in both. Much faster than     */
/* per-row IOCS fills.                                                 */
/* ------------------------------------------------------------------ */

static volatile uint16_t *gvram_ptr(int x, int y) {
  return (volatile uint16_t *)(0xC00000UL + (y << 10) + (x << 1));
}

/* fill one GVRAM row segment with a solid color, using 32-bit
   writes (two pixels at a time) where alignment allows */
static void gvram_fill_row(int x1, int x2, int y, uint16_t c) {
  volatile uint16_t *p = gvram_ptr(x1, y);
  int n = x2 - x1 + 1;
  if (x1 & 1) { *p++ = c; n--; }
  uint32_t cc = ((uint32_t)c << 16) | c;
  volatile uint32_t *p32 = (volatile uint32_t *)p;
  while (n >= 2) { *p32++ = cc; n -= 2; }
  if (n) *(volatile uint16_t *)p32 = c;
}

static void draw_stars_in(int x1, int y1, int x2, int y2) {
  for (int i = 0; i < STAR_N; i++) {
    if (star_x[i] >= x1 && star_x[i] <= x2 &&
        star_y[i] >= y1 && star_y[i] <= y2)
      *gvram_ptr(star_x[i], star_y[i]) = star_color(star_b[i]);
  }
}

/* restore the gradient background (+ stars) over a rectangle */
static void restore_bg(int x1, int y1, int x2, int y2) {
  if (x1 < 0) x1 = 0;
  if (y1 < 0) y1 = 0;
  if (x2 >= SCR_W) x2 = SCR_W - 1;
  if (y2 >= SCR_H) y2 = SCR_H - 1;
  for (int y = y1; y <= y2; y++) gvram_fill_row(x1, x2, y, bg_color(y));
  draw_stars_in(x1, y1, x2, y2);
}

static void draw_background() {
  for (int y = 0; y < SCR_H; y++) gvram_fill_row(0, SCR_W - 1, y, bg_color(y));
  draw_stars_in(0, 0, SCR_W - 1, SCR_H - 1);
}

/* ------------------------------------------------------------------ */
/* YM2151 (OPM) sound                                                  */
/* ------------------------------------------------------------------ */

class Fm {
public:
  bool muted = false;

  void init() {
    _iocs_opmset(0x14, 0x00);   /* timers off */
    _iocs_opmset(0x0f, 0x00);   /* noise off */
    _iocs_opmset(0x18, 0x00);   /* LFO freq */
    _iocs_opmset(0x19, 0x00);   /* AM depth 0 */
    _iocs_opmset(0x19, 0x80);   /* PM depth 0 */
    _iocs_opmset(0x1b, 0x00);   /* LFO waveform */
    /* ch0 lead, ch1 bass, ch2 SE blip, ch3 SE low */
    static const int lead_mul[4] = {1, 3, 2, 5};
    static const int lead_tl[4]  = {26, 40, 50, 62};
    voice(0, lead_mul, lead_tl, 9, 1, 7);
    static const int bass_mul[4] = {1, 1, 2, 1};
    static const int bass_tl[4]  = {16, 52, 60, 68};
    voice(1, bass_mul, bass_tl, 8, 2, 6);
    static const int se_mul[4] = {1, 3, 2, 4};
    static const int se_tl[4]  = {16, 40, 48, 56};
    voice(2, se_mul, se_tl, 14, 0, 10);
    static const int low_mul[4] = {1, 1, 1, 2};
    static const int low_tl[4]  = {14, 46, 54, 62};
    voice(3, low_mul, low_tl, 10, 0, 8);
    all_off();
  }

  /* one FM voice on a channel, algorithm 7 (all carriers) */
  void voice(int ch, const int mul[4], const int tl[4],
             int d1r, int d1l, int rr) {
    static const int slot_ofs[4] = {0, 8, 16, 24};
    _iocs_opmset(0x20 + ch, 0xc0 | 7);       /* L+R, FB=0, CON=7 */
    _iocs_opmset(0x38 + ch, 0x00);           /* PMS/AMS off */
    for (int s = 0; s < 4; s++) {
      int base = ch + slot_ofs[s];
      _iocs_opmset(0x40 + base, mul[s]);                 /* DT1/MUL */
      _iocs_opmset(0x60 + base, tl[s]);                  /* TL */
      _iocs_opmset(0x80 + base, 0x1f);                   /* KS/AR */
      _iocs_opmset(0xa0 + base, d1r);                    /* D1R */
      _iocs_opmset(0xc0 + base, 0x00);                   /* DT2/D2R */
      _iocs_opmset(0xe0 + base, (d1l << 4) | rr);        /* D1L/RR */
    }
  }

  static int midi_to_kc(int m) {
    static const int off[12] = {0, 2, 3, 4, 6, 7, 8, 10, 11, 12, 14, 15};
    int pc = m % 12;
    int oct = m / 12;
    return (oct - 2) * 16 + 14 + off[pc];
  }

  void note_on(int ch, int midi) {
    if (muted || midi < 24 || midi > 108) return;
    _iocs_opmset(0x28 + ch, midi_to_kc(midi));  /* KC */
    _iocs_opmset(0x30 + ch, 0);                 /* KF */
    _iocs_opmset(0x08, 0x78 | ch);              /* key on, all ops */
  }

  void note_off(int ch) { _iocs_opmset(0x08, ch); }

  void all_off() {
    for (int ch = 0; ch < 4; ch++) note_off(ch);
  }

  /* short SE blip on ch2 (or ch3 when low=true) */
  void se(int midi, bool low = false) {
    int ch = low ? 3 : 2;
    note_off(ch);
    note_on(ch, midi);
    se_timer_[low ? 1 : 0] = 6;
    se_ch_[low ? 1 : 0] = ch;
  }

  void update() {                 /* call once per frame */
    for (int i = 0; i < 2; i++) {
      if (se_timer_[i] > 0 && --se_timer_[i] == 0)
        note_off(se_ch_[i]);
    }
  }

  void set_muted(bool m) {
    muted = m;
    if (m) all_off();
  }

private:
  int se_timer_[2] = {0, 0};
  int se_ch_[2] = {2, 3};
};

static Fm g_fm;

/* ------------------------------------------------------------------ */
/* BGM patterns (midi notes, -1 = rest), 64 steps. Futuristic loop:   */
/* A-minor synthwave progression (Am F C G), the lead runs sequencer- */
/* style arpeggios over each chord while the bass drives an octave-   */
/* pulsing pattern on every step.                                     */
/* ------------------------------------------------------------------ */

static const int8_t BGM_LEAD[64] = {
  69,72,76,81, 76,72,69,72, 76,81,76,72, 69,-1,76,-1,   /* Am */
  65,69,72,77, 72,69,65,69, 72,77,72,69, 65,-1,72,-1,   /* F  */
  72,76,79,84, 79,76,72,76, 79,84,79,76, 72,-1,79,-1,   /* C  */
  67,71,74,79, 74,71,67,71, 74,79,83,-1, 81,-1,83,-1,   /* G  */
};
static const int8_t BGM_BASS[64] = {
  33,33,45,33, 33,33,45,33, 33,33,45,33, 33,45,33,45,   /* A1/A2 */
  29,29,41,29, 29,29,41,29, 29,29,41,29, 29,41,29,41,   /* F1/F2 */
  36,36,48,36, 36,36,48,36, 36,36,48,36, 36,48,36,48,   /* C2/C3 */
  31,31,43,31, 31,31,43,31, 31,31,43,31, 35,38,43,47,   /* G1/G2 + walk-up */
};
static constexpr int BGM_STEP_FRAMES = 13;   /* ~136 BPM 8th notes */

/* jingles: {midi, frames} pairs, midi=-2 ends */
struct JingleNote { int8_t midi; uint8_t frames; };
static const JingleNote JINGLE_CLEAR[] = {
  {72, 7}, {76, 7}, {79, 7}, {84, 17}, {-2, 0},
};
static const JingleNote JINGLE_OVER[] = {
  {64, 12}, {60, 12}, {55, 12}, {48, 31}, {-2, 0},
};
static const JingleNote JINGLE_START[] = {
  {60, 6}, {67, 6}, {72, 11}, {-2, 0},
};
static const JingleNote JINGLE_MISS[] = {
  {57, 7}, {45, 19}, {-2, 0},
};

/* ------------------------------------------------------------------ */
/* sprite patterns (16x16, PCG layout = four 8x8 quadrants TL,BL,TR,BR) */
/* palette block 1: 1 ball body, 2 highlight, 3 ball edge,              */
/* 4 paddle body, 5 paddle highlight, 6 paddle end cap, 7 paddle shade */
/* ------------------------------------------------------------------ */

static int idx_ball(int x, int y) {
  int dx = x - 8, dy = y - 8, d2 = dx * dx + dy * dy;
  if (d2 <= 25) return (dx <= -2 && dy <= -2) ? 2 : 1;
  if (d2 <= 36) return 3;
  return 0;
}
static int idx_pad_row(int y) { return y == 0 ? 5 : y == 9 ? 7 : 4; }
static int idx_pad_mid(int x, int y) {
  (void)x;
  return y > 9 ? 0 : idx_pad_row(y);
}
static int idx_pad_stud(int x, int y) {
  if (y > 9) return 0;
  if (y >= 3 && y <= 6 && x >= 5 && x <= 10) return 5;
  return idx_pad_row(y);
}
static int idx_pad_end(int x, int y) {
  if (y > 9) return 0;
  if (x < 6) return 6;
  return idx_pad_row(y);
}

static void build_pat16(uint8_t *out, int (*idxfn)(int, int)) {
  memset(out, 0, 128);
  int pos = 0;
  /* quadrant order is TL, BL, TR, BR (verified by test-card probe:
     the four 8x8 sub-patterns are consumed column-major) */
  for (int q = 0; q < 4; q++) {
    int ox = (q & 2) ? 8 : 0, oy = (q & 1) ? 8 : 0;
    for (int y = 0; y < 8; y++)
      for (int x = 0; x < 8; x++, pos++) {
        int idx = idxfn(ox + x, oy + y);
        if (!idx) continue;
        if (pos & 1) out[pos / 2] |= (uint8_t)idx;
        else out[pos / 2] |= (uint8_t)(idx << 4);
      }
  }
}

/* ------------------------------------------------------------------ */
/* game                                                                */
/* ------------------------------------------------------------------ */

enum class State { Title, Serve, Play, StageClear, GameOver, Goodbye };

struct Input {
  bool launch = false;
  bool pause = false;
  bool mute = false;
  bool esc = false;
  int dir = 0;              /* -1 / 0 / +1, held direction this frame */
  bool btn_edge = false;    /* joypad button newly pressed */
  int joy_raw = 0xff;
  int last_key = -1;        /* raw B_KEYINP code (debug) */
};

static uint16_t shade(uint16_t c, int d) {
  int r = ((c >> 6) & 31) + d;
  int g = ((c >> 11) & 31) + d;
  int b = ((c >> 1) & 31) + d;
  if (r < 0) r = 0;
  if (r > 31) r = 31;
  if (g < 0) g = 0;
  if (g > 31) g = 31;
  if (b < 0) b = 0;
  if (b > 31) b = 31;
  return GRB(r, g, b);
}

static const uint16_t ROW_MAIN[BLK_ROWS] = {
  GRB(31, 5, 8), GRB(31, 15, 3), GRB(30, 27, 4),
  GRB(7, 29, 9), GRB(4, 25, 29), GRB(19, 9, 30),
};
static const int ROW_SCORE[BLK_ROWS] = {60, 50, 40, 30, 20, 10};
static const uint16_t TITLE_COLS[6] = {
  GRB(31, 6, 8), GRB(31, 16, 3), GRB(30, 28, 4),
  GRB(8, 30, 10), GRB(5, 26, 30), GRB(20, 10, 30),
};

class Game {
public:
  void run() {
    super_ssp_ = _iocs_b_super(0);   /* for direct GVRAM access (.X) */
    prev_mode_ = _iocs_crtmod(CRT_MODE);
    _iocs_g_clr_on();
    set_60hz();
    _iocs_b_clr_al();
    _iocs_b_curoff();
    stars_init();
    g_fm.init();
    sprites_init();
    frame_sync_init();
#ifdef BENCH
    bench_main();   /* never returns */
#endif
    to_title();
    attract_ = true;   /* enable the idle demo (never set in bench) */
    for (;;) {
      Input in = poll_input();
      if (demo_mode_) {
        if (in.dir || in.launch || in.esc || in.pause || in.mute) {
          end_demo();              /* any input: back to title */
          in = Input();
        } else {
          in = demo_ai();
        }
      }
      bool keep = update(in);
      g_fm.update();
      if (!keep) break;
      /* under Human68k the text cursor can reappear after console
         output; keep it hidden (the HUD lives on the text plane) */
      _iocs_b_curoff();
      frame_sync();
    }
    /* only reached by the .X build (ESC on title) */
    sprites_set_visible(false);
    g_fm.all_off();
    _iocs_b_clr_al();
    _iocs_crtmod(prev_mode_);
    if (super_ssp_ >= 0) _iocs_b_super(super_ssp_);
  }

#ifdef BENCH
  /* Auto-play benchmark: title 5 s, then AI play 20 s. Reports the
     achieved fps and the average update+sound work per frame (ONTIME
     centiseconds), plus how many frames needed >= 2 cs of work
     (those are the frames that can miss a VDISP edge at 60 Hz). */
  void bench_main() {
    to_title();
    int t_frames = 0;
    int t0 = _iocs_ontime().sec;
    while (_iocs_ontime().sec - t0 < 500) {
      Input in;
      update(in);
      g_fm.update();
      frame_sync();
      t_frames++;
    }
    int t_elapsed = _iocs_ontime().sec - t0;

    start_game();
    int p_frames = 0, over2 = 0, serve_wait = 0;
    long work_acc = 0;
    t0 = _iocs_ontime().sec;
    while (_iocs_ontime().sec - t0 < 2000) {
      Input in;
      int center = paddle_x_ + PADDLE_W / 2;
      int target = (state_ == State::Play) ? bcx() : center;
      if (target < center - 2) in.dir = -1;
      else if (target > center + 2) in.dir = 1;
      if (state_ == State::Serve) {
        if (++serve_wait > 20) { in.launch = true; serve_wait = 0; }
      }
      if (state_ == State::GameOver) in.launch = true;
      int w0 = _iocs_ontime().sec;
      update(in);
      g_fm.update();
      int w1 = _iocs_ontime().sec;
      work_acc += (w1 - w0);
      if (w1 - w0 >= 2) over2++;
      frame_sync();
      p_frames++;
    }
    int p_elapsed = _iocs_ontime().sec - t0;

    g_fm.all_off();
    draw_background();
    uint16_t cw = GRB(31, 31, 31), cg = GRB(10, 31, 10),
             cy = GRB(31, 31, 10);
    Gfx::text(40, 50, 2, cy, "BENCH RESULTS");
    Gfx::text(40, 100, 2, cw, "TITLE FPS X100:");
    Gfx::number(40, 130, 2, cg, t_frames * 10000 / t_elapsed, 5);
    Gfx::text(40, 170, 2, cw, "PLAY FPS X100:");
    Gfx::number(40, 200, 2, cg, p_frames * 10000 / p_elapsed, 5);
    Gfx::text(40, 240, 2, cw, "WORK CS/FR X100:");
    Gfx::number(40, 270, 2, cg, (int)(work_acc * 100 / p_frames), 5);
    Gfx::text(40, 310, 2, cw, "FRAMES GE2CS:");
    Gfx::number(40, 340, 2, cg, over2, 5);
    Gfx::text(40, 380, 2, cw, "PLAY FRAMES:");
    Gfx::number(40, 410, 2, cg, p_frames, 5);
    for (;;) { }
  }
#endif

private:
  /* ---------------- state ---------------- */
  State state_ = State::Title;
  bool paused_ = false;
  bool exit_req_ = false;
  int prev_mode_ = 0;
  int super_ssp_ = -1;
  int frame_ = 0;

  int level_ = 1;
  int score_ = 0;
  int hi_ = 0;
  int balls_left_ = MAX_BALLS;
  bool blocks_[BLK_ROWS][BLK_COLS];
  int blocks_left_ = 0;

  int paddle_x_ = (SCR_W - PADDLE_W) / 2;
  int paddle_fx_ = ((SCR_W - PADDLE_W) / 2) << FP;
  int old_paddle_x_ = paddle_x_;
  int ball_fx_ = (SCR_W / 2) << FP;        /* fixed-point ball pos */
  int ball_fy_ = (PADDLE_Y - BALL_R - 1) << FP;
  int old_ball_cx_ = SCR_W / 2, old_ball_cy_ = PADDLE_Y - BALL_R - 1;
  int ball_vx_ = 0, ball_vy_ = 0;          /* fixed point (<<FP) */
  int bcx() const { return ball_fx_ >> FP; }
  int bcy() const { return ball_fy_ >> FP; }
  int broken_total_ = 0;

  int key_dir_ = 0, key_dir_timer_ = 0;
  int last_key_raw_ = -1;
  int joy_raw_ = 0xff;
  int dbg_key_ = -2, dbg_joy_ = -2;
  bool joy_btn_prev_ = false;

  /* sound sequencer */
  bool bgm_on_ = false;
  int bgm_step_ = 0, bgm_timer_ = 0;
  const JingleNote *jingle_ = nullptr;
  int jingle_idx_ = 0, jingle_timer_ = 0;

  int state_timer_ = 0;
  int title_cycle_ = 0;
  /* attract-mode demo play */
  bool demo_mode_ = false;
  bool attract_ = false;   /* enabled by run(); never in the bench */
  int title_idle_ = 0;     /* frames without input on the title */
  int saved_hi_ = 0;       /* hi-score snapshot taken at demo start */
  int demo_serve_wait_ = 0;
  int demo_over_t_ = 0;
  bool press_visible_ = true;

  /* ---------------- timing ---------------- */
  /* One game frame per display refresh. The wait returns at the start
     of the vertical blank, so the following update+draw runs inside
     the blank / early scan: no tearing, no flicker. If VDISP ever
     stalls (1 s timeout), fall back to ONTIME pacing at ~60 Hz. */
  bool vsync_ok_ = true;
  int sync_next_ = 0;
  int sync_acc_ = 0;
  void frame_sync_init() { sync_next_ = _iocs_ontime().sec; }
  void frame_sync() {
    if (vsync_ok_) {
      if (wait_vdisp_edge() == 0) return;
      vsync_ok_ = false;                 /* VDISP unavailable: fall back */
      sync_next_ = _iocs_ontime().sec;
      sync_acc_ = 0;
    }
    /* fallback: 5 cs per 3 frames = 1.667 cs = 60 Hz average */
    sync_acc_ += 5;
    sync_next_ += sync_acc_ / 3;
    sync_acc_ %= 3;
    int now = _iocs_ontime().sec;
    if (now - sync_next_ > 100 || sync_next_ - now > 100000)
      sync_next_ = now;                    /* resync after hiccups */
    while (_iocs_ontime().sec < sync_next_) { }
  }

  /* ---------------- input ---------------- */
  Input poll_input() {
    Input in;
    while (_iocs_b_keysns()) {
      int code = _iocs_b_keyinp();
      last_key_raw_ = code;
      /* B_KEYINP returns (scancode << 8) | ascii. Cursor keys have
         ascii = 0, so they must be recognized by scancode:
         Left = 0x3B, Right = 0x3D (measured on WebX68k). */
      int scan = (code >> 8) & 0xff;
      int k = code & 0xff;
      if (k >= 'A' && k <= 'Z') k += 'a' - 'A';
      if (k == 0x1b) { in.esc = true; continue; }
      if (k == ' ') { in.launch = true; continue; }
      if (scan == 0x3b || k == 0x1d) {
        key_dir_ = -1; key_dir_timer_ = 8; continue;
      }
      if (scan == 0x3d || k == 0x1c) {
        key_dir_ = +1; key_dir_timer_ = 8; continue;
      }
      switch (k) {
        case 'a': case 'z': key_dir_ = -1; key_dir_timer_ = 8; break;
        case 'd': case 'x': key_dir_ = +1; key_dir_timer_ = 8; break;
        case 'p': in.pause = true; break;
        case 'm': in.mute = true; break;
        default: break;
      }
    }
    int joy = _iocs_joyget(0) & _iocs_joyget(1);
    joy_raw_ = joy & 0xff;
    bool jleft = !(joy & 0x04);
    bool jright = !(joy & 0x08);
    bool jbtn = !(joy & 0x30);
    in.btn_edge = jbtn && !joy_btn_prev_;
    joy_btn_prev_ = jbtn;
    if (jleft) in.dir = -1;
    else if (jright) in.dir = +1;
    else if (key_dir_timer_ > 0) in.dir = key_dir_;
    if (key_dir_timer_ > 0) key_dir_timer_--;
    in.joy_raw = joy_raw_;
    in.last_key = last_key_raw_;
    if (in.btn_edge) in.launch = true;
    return in;
  }

  /* ---------------- sound sequencing ---------------- */
  void start_jingle(const JingleNote *j) {
    jingle_ = j;
    jingle_idx_ = 0;
    jingle_timer_ = 0;
    g_fm.note_off(0);
    g_fm.note_off(1);
  }
  bool jingle_active() const { return jingle_ != nullptr; }

  void sound_update() {
    if (jingle_active()) {
      if (jingle_timer_ > 0) { jingle_timer_--; return; }
      const JingleNote &n = jingle_[jingle_idx_];
      if (n.midi == -2) {
        jingle_ = nullptr;
        g_fm.note_off(0);
        g_fm.note_off(1);
        return;
      }
      g_fm.note_off(0);
      g_fm.note_off(1);
      g_fm.note_on(0, n.midi);
      if (n.midi >= 40) g_fm.note_on(1, n.midi - 12);
      jingle_timer_ = n.frames;
      jingle_idx_++;
      return;
    }
    if (!bgm_on_ || paused_) return;
    if (++bgm_timer_ < BGM_STEP_FRAMES) return;
    bgm_timer_ = 0;
    int step = bgm_step_;
    bgm_step_ = (bgm_step_ + 1) & 63;
    g_fm.note_off(0);
    int lead = BGM_LEAD[step];
    if (lead >= 0) g_fm.note_on(0, lead);
    g_fm.note_off(1);
    int bass = BGM_BASS[step];
    if (bass >= 0) g_fm.note_on(1, bass);
  }

  /* ---------------- sprites ---------------- */
  /* Sprite 0 = ball, sprites 1..5 = paddle segments. The ball and
     paddle live on the sprite plane during play, so per-frame drawing
     is just a few SP_REGST calls - no GVRAM erase/draw at all. */
  bool sprites_on_ = false;

  void sprites_init() {
    _iocs_sp_init();
    _iocs_spalet(1, 1, GRB(31, 30, 18));   /* ball body */
    _iocs_spalet(2, 1, GRB(31, 31, 31));   /* highlight */
    _iocs_spalet(3, 1, GRB(22, 19, 5));    /* ball edge */
    _iocs_spalet(4, 1, COL_PADDLE);
    _iocs_spalet(5, 1, COL_PADDLE_HI);
    _iocs_spalet(6, 1, COL_PADDLE_END);
    _iocs_spalet(7, 1, GRB(6, 10, 16));    /* paddle shade */
    static uint8_t pat[128];
    build_pat16(pat, idx_ball);
    _iocs_sp_defcg(0, 1, pat);
    build_pat16(pat, idx_pad_mid);
    _iocs_sp_defcg(4, 1, pat);
    build_pat16(pat, idx_pad_end);
    _iocs_sp_defcg(8, 1, pat);
    build_pat16(pat, idx_pad_stud);
    _iocs_sp_defcg(12, 1, pat);
  }

  void sprites_set_visible(bool on) {
    if (on == sprites_on_) return;
    sprites_on_ = on;
    if (on) {
      _iocs_sp_on();
      sprites_update();
    } else {
      _iocs_sp_off();
    }
  }

  /* SP_REGST turned out to serialize on the vertical blank (each
     call costs up to a frame), so positions are written straight to
     sprite RAM: 0xEB0000 + no*8 = {x+16, y+16, code, priority}.
     Under the retimed CRTC (512x480) sprites land 6 lines lower
     than the graphics plane, so y is compensated by -6 (measured
     with the test-card probe). */
  void sprites_update() {
    if (!sprites_on_) return;
    volatile uint16_t *r = (volatile uint16_t *)0xEB0000UL;
    r[0] = (uint16_t)(bcx() - 8 + 16);
    r[1] = (uint16_t)(bcy() - 8 + 16 - 6);
    r[2] = (uint16_t)(0 | (1 << 8));
    r[3] = 3;
    for (int i = 0; i < 5; i++) {
      int code = (i == 0) ? 8 : (i == 4) ? (8 | 0x4000)
               : (i == 2) ? 12 : 4;
      volatile uint16_t *s = (volatile uint16_t *)(0xEB0000UL + (1 + i) * 8);
      s[0] = (uint16_t)(paddle_x_ + i * 16 + 16);
      s[1] = (uint16_t)(PADDLE_Y + 16 - 6);
      s[2] = (uint16_t)(code | (1 << 8));
      s[3] = 3;
    }
  }

  /* ---------------- drawing ---------------- */
  void draw_frame() {
    /* 4 bars, outer highlight + inner shadow lines */
    Gfx::fill(FR_X0, FR_Y0, FR_X1, FR_Y0 + 3, COL_FRAME);
    Gfx::fill(FR_X0, FR_Y1 - 3, FR_X1, FR_Y1, COL_FRAME);
    Gfx::fill(FR_X0, FR_Y0, FR_X0 + 3, FR_Y1, COL_FRAME);
    Gfx::fill(FR_X1 - 3, FR_Y0, FR_X1, FR_Y1, COL_FRAME);
    Gfx::line(FR_X0, FR_Y0, FR_X1, FR_Y0, COL_FRAME_HI);
    Gfx::line(FR_X0, FR_Y1, FR_X1, FR_Y1, COL_FRAME_HI);
    Gfx::line(FR_X0, FR_Y0, FR_X0, FR_Y1, COL_FRAME_HI);
    Gfx::line(FR_X1, FR_Y0, FR_X1, FR_Y1, COL_FRAME_HI);
  }

  void draw_block(int r, int c) {
    int x = BLK_X + c * BLK_PX;
    int y = BLK_Y + r * BLK_PY;
    uint16_t main = ROW_MAIN[r];
    Gfx::fill(x, y, x + BLK_W - 1, y + BLK_H - 1, main);
    Gfx::line(x, y, x + BLK_W - 1, y, shade(main, 12));
    Gfx::line(x, y + 1, x + BLK_W - 1, y + 1, shade(main, 6));
    Gfx::line(x, y + BLK_H - 1, x + BLK_W - 1, y + BLK_H - 1,
              shade(main, -13));
    Gfx::line(x, y, x, y + BLK_H - 1, shade(main, -6));
  }

  void draw_blocks() {
    for (int r = 0; r < BLK_ROWS; r++)
      for (int c = 0; c < BLK_COLS; c++)
        if (blocks_[r][c]) draw_block(r, c);
  }

  /* ---------------- HUD (console text plane) ---------------- */
  /* The HUD lives on the text plane: updating a changed value costs
     one B_LOCATE + one B_PRINT, instead of ~100 graphics fills for a
     full redraw. Text columns are 8 px, rows 16 px, which maps well
     onto the old graphics layout. */
  int hud_score_ = -1, hud_hi_ = -1, hud_level_ = -1;
  int hud_sound_ = -1, hud_balls_ = -1;

  static void fmt_digits(char *buf, int value, int width) {
    for (int i = width - 1; i >= 0; i--) {
      buf[i] = (char)('0' + value % 10);
      value /= 10;
    }
    buf[width] = 0;
  }

  void draw_ball_icons() {
    restore_bg(408, 6, 462, 24);
    for (int i = 0; i < MAX_BALLS - 1; i++)
      if (i < balls_left_ - 1) draw_ball_at(420 + i * 26, 15, true);
  }

  void hud_setup() {
    _iocs_b_clr_al();
    _iocs_b_locate(2, 0);
    _iocs_b_print("SCORE");
    _iocs_b_locate(24, 0);
    _iocs_b_print("HI");
    _iocs_b_locate(42, 0);
    _iocs_b_print("BALL");
    _iocs_b_locate(2, 1);
    _iocs_b_print("STAGE");
    hud_score_ = hud_hi_ = hud_level_ = hud_sound_ = hud_balls_ = -1;
    hud_update();
    _iocs_b_curoff();
  }

  void hud_update() {
    char buf[16];
    if (score_ != hud_score_) {
      hud_score_ = score_;
      fmt_digits(buf, score_, 6);
      _iocs_b_locate(11, 0);
      _iocs_b_print(buf);
    }
    if (hi_ != hud_hi_) {
      hud_hi_ = hi_;
      fmt_digits(buf, hi_, 6);
      _iocs_b_locate(29, 0);
      _iocs_b_print(buf);
    }
    if (level_ != hud_level_) {
      hud_level_ = level_;
      fmt_digits(buf, level_, 2);
      _iocs_b_locate(9, 1);
      _iocs_b_print(buf);
    }
    int snd = g_fm.muted ? 0 : 1;
    if (snd != hud_sound_) {
      hud_sound_ = snd;
      _iocs_b_locate(24, 1);
      _iocs_b_print(snd ? "SOUND ON " : "SOUND OFF");
    }
    if (balls_left_ != hud_balls_) {
      hud_balls_ = balls_left_;
      draw_ball_icons();
    }
  }

  /* kept name for the existing call sites: partial update only */
  void draw_hud() { hud_update(); }

  void draw_ball_at(int cx, int cy, bool mini) {
    static const int hw[9] = {2, 3, 4, 4, 4, 4, 4, 3, 2};
    int s = mini ? 0 : 0;
    for (int dy = -4; dy <= 4; dy++) {
      uint16_t col = (dy < -1) ? GRB(31, 31, 26)
                   : (dy > 2)  ? GRB(24, 22, 8) : COL_BALL;
      Gfx::fill(cx - hw[dy + 4] + s, cy + dy, cx + hw[dy + 4], cy + dy,
                col);
    }
    Gfx::pset(cx - 1, cy - 2, COL_WHITE);
  }

  void draw_paddle() {
    int x = paddle_x_;
    Gfx::fill(x, PADDLE_Y, x + PADDLE_W - 1, PADDLE_Y + PADDLE_H - 1,
              COL_PADDLE);
    Gfx::line(x, PADDLE_Y, x + PADDLE_W - 1, PADDLE_Y, COL_PADDLE_HI);
    Gfx::line(x, PADDLE_Y + 1, x + PADDLE_W - 1, PADDLE_Y + 1,
              shade(COL_PADDLE, 6));
    Gfx::line(x, PADDLE_Y + PADDLE_H - 1, x + PADDLE_W - 1,
              PADDLE_Y + PADDLE_H - 1, shade(COL_PADDLE, -12));
    Gfx::fill(x, PADDLE_Y, x + 5, PADDLE_Y + PADDLE_H - 1,
              COL_PADDLE_END);
    Gfx::fill(x + PADDLE_W - 6, PADDLE_Y, x + PADDLE_W - 1,
              PADDLE_Y + PADDLE_H - 1, COL_PADDLE_END);
    Gfx::fill(x + PADDLE_W / 2 - 3, PADDLE_Y + 3,
              x + PADDLE_W / 2 + 2, PADDLE_Y + 6, COL_PADDLE_HI);
  }

  /* ball and paddle are sprites now: per-frame "drawing" is just
     repositioning them */
  void draw_play_entities() { sprites_update(); }

  void draw_all_play() {
    draw_background();
    draw_frame();
    draw_blocks();
    hud_setup();
    sprites_set_visible(true);
    sprites_update();
  }

  void draw_panel(const char *title, uint16_t tcol,
                  const char *line2, const char *line3) {
    Gfx::fill(76, 184, 436, 296, COL_PANEL);
    Gfx::fill(76, 184, 436, 187, COL_PANEL_BD);
    Gfx::fill(76, 293, 436, 296, COL_PANEL_BD);
    Gfx::fill(76, 184, 79, 296, COL_PANEL_BD);
    Gfx::fill(433, 184, 436, 296, COL_PANEL_BD);
    Gfx::text_center(204, 3, tcol, title);
    if (line2) Gfx::text_center(244, 2, COL_WHITE, line2);
    if (line3) Gfx::text_center(272, 1, GRB(14, 20, 24), line3);
  }

  /* ---------------- title ---------------- */
  void draw_title_text() {
    restore_bg(0, 80, SCR_W - 1, 156);
    const char *t = "STAR BREAKER";
    int len = (int)strlen(t);
    int scale = (len <= 8) ? 7 : (len <= 10) ? 6 : 5;
    int w = Gfx::text_width(t, scale);
    int x0 = (SCR_W - w) / 2;
    Gfx::text(x0 + 4, 90 + 4, scale, GRB(8, 4, 16), t);
    for (int i = 0; t[i]; i++) {
      char one[2] = {t[i], 0};
      Gfx::text(x0 + i * 8 * scale, 90, scale,
                TITLE_COLS[(i + title_cycle_) % 6], one);
    }
    int uy = 90 + 8 * scale + 2;
    Gfx::fill(x0, uy, x0 + w, uy + 3, TITLE_COLS[title_cycle_ % 6]);
  }

  void draw_press(bool visible) {
    if (!visible) {
      restore_bg(80, 348, 432, 366);
      return;
    }
    Gfx::text_center(352, 2, COL_WHITE, "PRESS SPACE OR PAD BTN");
  }

  void to_title() {
    if (demo_mode_) {
      /* leaving the demo by any route: the demo never keeps the
         hi-score it made */
      demo_mode_ = false;
      hi_ = saved_hi_;
      title_idle_ = 0;
    }
    state_ = State::Title;
    paused_ = false;
    bgm_on_ = true;
    bgm_step_ = 0;
    bgm_timer_ = 0;
    title_cycle_ = 0;
    sprites_set_visible(false);
    _iocs_b_clr_al();          /* drop the text-plane HUD */
    draw_background();
    draw_title_text();
    /* decorative blocks + paddle + ball */
    for (int c = 0; c < BLK_COLS; c++) {
      int x = BLK_X + c * BLK_PX;
      uint16_t main = TITLE_COLS[c % 6];
      Gfx::fill(x, 216, x + BLK_W - 1, 216 + BLK_H - 1, main);
      Gfx::line(x, 216, x + BLK_W - 1, 216, shade(main, 12));
      Gfx::line(x, 216 + BLK_H - 1, x + BLK_W - 1, 216 + BLK_H - 1,
                shade(main, -13));
    }
    center_paddle();
    draw_paddle_at_title();
    draw_ball_at(SCR_W / 2, 246, false);
    Gfx::text_center(300, 2, COL_WHITE, "HI-SCORE");
    Gfx::number((SCR_W - Gfx::text_width("000000", 2)) / 2, 322, 2,
                GRB(30, 30, 10), hi_, 6);
    press_visible_ = true;
    draw_press(true);
    Gfx::text_center(404, 1, GRB(16, 22, 26),
                     "MOVE : CURSOR / A D / Z X / PAD");
    Gfx::text_center(424, 1, GRB(16, 22, 26),
                     "SPACE : LAUNCH + START   P : PAUSE");
    Gfx::text_center(444, 1, GRB(16, 22, 26),
                     "M : SOUND ON/OFF   ESC : TITLE/EXIT");
    draw_debug_title();
  }

  void draw_paddle_at_title() {
    int save_y = paddle_x_;
    (void)save_y;
    /* paddle drawn at its normal place, lower than usual for title */
    int x = paddle_x_;
    Gfx::fill(x, 262, x + PADDLE_W - 1, 262 + PADDLE_H - 1, COL_PADDLE);
    Gfx::line(x, 262, x + PADDLE_W - 1, 262, COL_PADDLE_HI);
    Gfx::fill(x, 262, x + 5, 262 + PADDLE_H - 1, COL_PADDLE_END);
    Gfx::fill(x + PADDLE_W - 6, 262, x + PADDLE_W - 1,
              262 + PADDLE_H - 1, COL_PADDLE_END);
  }

  void draw_debug_title() {
#if !DEBUG_HUD
    return;
#endif
    restore_bg(0, SCR_H - 12, 90, SCR_H - 1);
    char buf[16];
    static const char HEX[] = "0123456789ABCDEF";
    int v = last_key_raw_ & 0xffff;
    buf[0] = 'K'; buf[1] = '=';
    buf[2] = HEX[(v >> 12) & 15]; buf[3] = HEX[(v >> 8) & 15];
    buf[4] = HEX[(v >> 4) & 15];  buf[5] = HEX[v & 15];
    buf[6] = ' '; buf[7] = 'J'; buf[8] = '=';
    buf[9] = HEX[(joy_raw_ >> 4) & 15]; buf[10] = HEX[joy_raw_ & 15];
    buf[11] = 0;
    Gfx::text(4, SCR_H - 11, 1, GRB(12, 14, 8), buf);
  }

  /* ---------------- game flow ---------------- */
  void build_blocks() {
    blocks_left_ = 0;
    for (int r = 0; r < BLK_ROWS; r++)
      for (int c = 0; c < BLK_COLS; c++) {
        blocks_[r][c] = true;
        blocks_left_++;
      }
  }

  void reset_ball_on_paddle() {
    ball_fx_ = (paddle_x_ + PADDLE_W / 2) << FP;
    ball_fy_ = (PADDLE_Y - BALL_R - 1) << FP;
    old_ball_cx_ = bcx();
    old_ball_cy_ = bcy();
    ball_vx_ = ball_vy_ = 0;
  }

  void start_game() {
    score_ = 0;
    level_ = 1;
    balls_left_ = MAX_BALLS;
    broken_total_ = 0;
    build_blocks();
    center_paddle();
    reset_ball_on_paddle();
    state_ = State::Serve;
    paused_ = false;
    bgm_on_ = true;
    bgm_step_ = 0;
    bgm_timer_ = 0;
    draw_all_play();
    start_jingle(JINGLE_START);
  }

  /* -------- attract mode: idle on title -> AI demo play -------- */
  void start_demo() {
    saved_hi_ = hi_;
    title_idle_ = 0;
    demo_serve_wait_ = 0;
    start_game();
    demo_mode_ = true;
    _iocs_b_locate(56, 1);
    _iocs_b_print("DEMO");
  }

  void end_demo() {
    title_idle_ = 0;
    to_title();   /* restores the hi-score and clears demo_mode_ */
  }

  Input demo_ai() {
    Input in;
    if (state_ == State::Serve) {
      int target = SCR_W / 2 - PADDLE_W / 2;
      in.dir = (paddle_x_ < target - 2) ? 1
               : (paddle_x_ > target + 2) ? -1 : 0;
      if (++demo_serve_wait_ > 40) {
        in.launch = true;
        demo_serve_wait_ = 0;
      }
    } else if (state_ == State::Play) {
      int bc = bcx(), pc = paddle_x_ + PADDLE_W / 2;
      in.dir = (bc < pc - 4) ? -1 : (bc > pc + 4) ? 1 : 0;
      demo_serve_wait_ = 0;
    } else if (state_ == State::StageClear) {
      in.launch = true;    /* proceed to the next stage */
    }
    return in;
  }

  void launch_ball() {
    int pm = speed_permille();
    int dir = (key_dir_ != 0) ? key_dir_ : 1;
    ball_vx_ = (640 * dir) * pm / 1000;
    ball_vy_ = -640 * pm / 1000;
    state_ = State::Play;
    g_fm.se(81);
  }

  int speed_permille() const {
    int pm = 1000 + (level_ - 1) * 90 + (broken_total_ / 8) * 15;
    if (pm > 1500) pm = 1500;
    return pm * 5 / 6;   /* 60 fps: keep the old 50 fps px/sec speeds */
  }

  /* paddle position is fixed-point so 250 px/s at 50 fps becomes the
     same 250 px/s at 60 fps (4.17 px/frame instead of 5) */
  static constexpr int PADDLE_SPEED_FP = (PADDLE_SPEED << FP) * 5 / 6;
  void move_paddle(int dir) {
    if (dir == 0) return;
    paddle_fx_ += dir * PADDLE_SPEED_FP;
    int min_fx = (FR_X0 + 4) << FP;
    int max_fx = (FR_X1 - 3 - PADDLE_W) << FP;
    if (paddle_fx_ < min_fx) paddle_fx_ = min_fx;
    if (paddle_fx_ > max_fx) paddle_fx_ = max_fx;
    paddle_x_ = paddle_fx_ >> FP;
  }
  void center_paddle() {
    paddle_x_ = (SCR_W - PADDLE_W) / 2;
    paddle_fx_ = paddle_x_ << FP;
    old_paddle_x_ = paddle_x_;
  }

  /* ---------------- physics ---------------- */
  void physics() {
    for (int sub = 0; sub < 2; sub++) {
      int prev_cx = bcx(), prev_cy = bcy();
      ball_fx_ += ball_vx_ >> 1;
      ball_fy_ += ball_vy_ >> 1;
      int cx = bcx(), cy = bcy();
      /* walls */
      if (cx - BALL_R < WALL_L) {
        ball_fx_ = (WALL_L + BALL_R) << FP;
        ball_vx_ = -ball_vx_;
        cx = bcx();
        g_fm.se(50, true);
      } else if (cx + BALL_R > WALL_R) {
        ball_fx_ = (WALL_R - BALL_R) << FP;
        ball_vx_ = -ball_vx_;
        cx = bcx();
        g_fm.se(50, true);
      }
      if (cy - BALL_R < WALL_T) {
        ball_fy_ = (WALL_T + BALL_R) << FP;
        ball_vy_ = -ball_vy_;
        cy = bcy();
        g_fm.se(50, true);
      }
      /* paddle */
      if (ball_vy_ > 0) {
        int prev_bottom = prev_cy + BALL_R;
        int new_bottom = cy + BALL_R;
        if (prev_bottom <= PADDLE_Y && new_bottom >= PADDLE_Y &&
            cx + BALL_R >= paddle_x_ &&
            cx - BALL_R <= paddle_x_ + PADDLE_W) {
          /* zone vectors in fixed point (px/frame << FP) */
          static const int zvx[5] = {-880, -640, 0, 640, 880};
          static const int zvy[5] = {-450, -640, -900, -640, -450};
          int rel = cx - (paddle_x_ + PADDLE_W / 2);
          int zone = (rel < -26) ? 0 : (rel < -9) ? 1 : (rel <= 9)
                     ? 2 : (rel <= 26) ? 3 : 4;
          int pm = speed_permille();
          ball_vx_ = zvx[zone] * pm / 1000;
          ball_vy_ = zvy[zone] * pm / 1000;
          ball_fy_ = (PADDLE_Y - BALL_R) << FP;
          cy = bcy();
          g_fm.se(62);
        }
      }
      /* blocks */
      if (hit_blocks(prev_cx, prev_cy)) {
        if (blocks_left_ <= 0) { on_stage_clear(); return; }
      }
      /* floor */
      if (bcy() - BALL_R > FLOOR_Y) { on_miss(); return; }
      if (state_ != State::Play) return;
    }
  }

  bool hit_blocks(int prev_cx, int prev_cy) {
    int cx = bcx(), cy = bcy();
    int x1 = cx - BALL_R, x2 = cx + BALL_R;
    int y1 = cy - BALL_R, y2 = cy + BALL_R;
    for (int r = 0; r < BLK_ROWS; r++) {
      for (int c = 0; c < BLK_COLS; c++) {
        if (!blocks_[r][c]) continue;
        int bx = BLK_X + c * BLK_PX, by = BLK_Y + r * BLK_PY;
        if (x2 < bx || x1 > bx + BLK_W - 1 ||
            y2 < by || y1 > by + BLK_H - 1)
          continue;
        /* hit */
        blocks_[r][c] = false;
        blocks_left_--;
        broken_total_++;
        score_ += ROW_SCORE[r];
        if (score_ > hi_) hi_ = score_;
        restore_bg(bx, by, bx + BLK_W - 1, by + BLK_H - 1);
        g_fm.se(74 + (BLK_ROWS - r) * 2);
        draw_hud();
        bool from_side =
            (prev_cx + BALL_R <= bx) || (prev_cx - BALL_R >= bx + BLK_W);
        bool from_top_bottom =
            (prev_cy + BALL_R <= by) || (prev_cy - BALL_R >= by + BLK_H);
        if (from_side && !from_top_bottom) ball_vx_ = -ball_vx_;
        else ball_vy_ = -ball_vy_;
        return true;
      }
    }
    return false;
  }

  void on_miss() {
    balls_left_--;
    if (balls_left_ > 0) {
      start_jingle(JINGLE_MISS);
      reset_ball_on_paddle();
      state_ = State::Serve;
      draw_hud();
    } else {
      if (score_ > hi_) hi_ = score_;
      state_ = State::GameOver;
      demo_over_t_ = 0;
      bgm_on_ = false;
      g_fm.note_off(0);
      g_fm.note_off(1);
      start_jingle(JINGLE_OVER);
      sprites_set_visible(false);
      draw_panel("GAME OVER", GRB(31, 8, 8), "SPACE : RETRY",
                 "ESC : TITLE");
      draw_hud();
    }
  }

  void on_stage_clear() {
    score_ += 500;
    if (score_ > hi_) hi_ = score_;
    state_ = State::StageClear;
    state_timer_ = 204;
    bgm_on_ = false;
    g_fm.note_off(0);
    g_fm.note_off(1);
    start_jingle(JINGLE_CLEAR);
    sprites_set_visible(false);
    draw_panel("STAGE CLEAR", GRB(10, 31, 14), "BONUS 500 PTS", nullptr);
    draw_hud();
  }

  /* ---------------- update ---------------- */
  bool update(const Input &in) {
    frame_++;
    if (in.mute) {
      g_fm.set_muted(!g_fm.muted);
      if (state_ == State::Serve || state_ == State::Play) draw_hud();
    }
    sound_update();

    switch (state_) {
      case State::Title: {
        if (in.dir || in.launch || in.esc || in.pause || in.mute)
          title_idle_ = 0;
        else if (attract_ && ++title_idle_ >= 300) {
          start_demo();          /* ~5 s idle: attract demo */
          return !exit_req_;
        }
        if ((frame_ % 54) == 0) {
          title_cycle_++;
          draw_title_text();
        }
        bool vis = (frame_ % 84) < 55;
        if (vis != press_visible_) {
          press_visible_ = vis;
          draw_press(vis);
        }
        if (last_key_raw_ != dbg_key_ || joy_raw_ != dbg_joy_) {
          dbg_key_ = last_key_raw_;
          dbg_joy_ = joy_raw_;
          draw_debug_title();
        }
        if (in.esc) {
          g_fm.all_off();
          draw_background();
          Gfx::text_center(220, 3, GRB(10, 26, 30), "THANK YOU!");
          Gfx::text_center(280, 1, GRB(16, 22, 26),
                           "RESET TO PLAY AGAIN");
#ifdef STANDALONE
          for (;;) { }   /* no OS to return to */
#else
          return false;
#endif
        }
        if (in.launch) start_game();
        break;
      }
      case State::Serve: {
        if (in.esc) { to_title(); break; }
        if (in.pause) { toggle_pause(); break; }
        if (paused_) break;
        move_paddle(in.dir);
        ball_fx_ = (paddle_x_ + PADDLE_W / 2) << FP;
        ball_fy_ = (PADDLE_Y - BALL_R - 1) << FP;
        draw_play_entities();
        if ((frame_ % 84) < 55) {
          Gfx::text_center(398, 1, GRB(20, 26, 30),
                           "SPACE / PAD BTN : LAUNCH");
        } else if ((frame_ % 84) == 55) {
          restore_bg(140, 396, 372, 408);
        }
        if (in.launch) launch_ball();
        break;
      }
      case State::Play: {
        if (in.esc) { to_title(); break; }
        if (in.pause) { toggle_pause(); break; }
        if (paused_) break;
        move_paddle(in.dir);
        physics();
        if (state_ == State::Play || state_ == State::Serve)
          draw_play_entities();
        break;
      }
      case State::StageClear: {
        if (in.esc) { to_title(); break; }
        if (--state_timer_ <= 0) {
          level_++;
          build_blocks();
          reset_ball_on_paddle();
          center_paddle();
          state_ = State::Serve;
          bgm_on_ = true;
          bgm_step_ = 0;
          bgm_timer_ = 0;
          draw_all_play();
        }
        break;
      }
      case State::GameOver: {
        if (demo_mode_) {
          if (++demo_over_t_ >= 150) end_demo();  /* ~2.5 s, then title */
          break;
        }
        if (in.esc) { to_title(); break; }
        if (in.launch && !jingle_active()) start_game();
        break;
      }
      case State::Goodbye:
        break;
    }
    return !exit_req_;
  }

  void toggle_pause() {
    paused_ = !paused_;
    if (paused_) {
      g_fm.note_off(0);
      g_fm.note_off(1);
      sprites_set_visible(false);
      draw_panel("PAUSED", GRB(30, 30, 10), "P : RESUME", "ESC : TITLE");
    } else {
      draw_all_play();
    }
  }
};

/* ------------------------------------------------------------------ */

static Game g_game;

int main() {
  g_game.run();
  return 0;
}
