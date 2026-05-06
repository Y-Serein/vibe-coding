// SPDX-License-Identifier: GPL-2.0+
/*
 * AIKB LCD dashboard sample.
 *
 * Renders a 960x412 landscape overview UI to /dev/fb0. The panel is native
 * 412x960 portrait, so the blitter can rotate the landscape canvas into a
 * portrait framebuffer when the LCD is mounted sideways.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef __maybe_unused
#define __maybe_unused
#endif
#include "../../../../u-boot-2021.10/include/video_font_data.h"

#ifndef AIKB_USE_FREETYPE
#define AIKB_USE_FREETYPE 1
#endif

#if AIKB_USE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

#define UI_W 960
#define UI_H 412
#define MAX_SESSIONS 8
#define LINE_BUF 1024
#define TERM_PAD_X 8
#define TERM_PAD_Y 8
#define TERM_STATUS_H 28
#define TERM_CELL_W VIDEO_FONT_WIDTH
#define TERM_CELL_H VIDEO_FONT_HEIGHT
#define TERM_COLS ((UI_W - TERM_PAD_X * 2) / TERM_CELL_W)
#define TERM_ROWS ((UI_H - TERM_STATUS_H - TERM_PAD_Y) / TERM_CELL_H)
#define TERM_MAX_ARGS 8
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

enum rotation {
	ROT_AUTO,
	ROT_NONE,
	ROT_CW,
	ROT_CCW,
};

enum pixel_format {
	PIXEL_FMT_AUTO,
	PIXEL_FMT_FB_FIELDS,
	PIXEL_FMT_ARGB8888,
	PIXEL_FMT_ABGR8888,
};

enum app_view {
	VIEW_TERMINAL,
	VIEW_DASHBOARD,
};

enum term_state {
	TERM_GROUND,
	TERM_ESC,
	TERM_CSI,
	TERM_OSC,
};

struct canvas {
	int w;
	int h;
	uint32_t *px;
};

struct term_cell {
	uint32_t ch;
	uint32_t fg;
	uint32_t bg;
	uint8_t flags;
};

struct terminal {
	struct term_cell cell[TERM_ROWS][TERM_COLS];
	int row;
	int col;
	int saved_row;
	int saved_col;
	int scroll_top;
	int scroll_bottom;
	uint32_t fg;
	uint32_t bg;
	uint8_t bold;
	uint8_t inverse;
	enum term_state state;
	int args[TERM_MAX_ARGS];
	int arg_count;
	int cur_arg;
	bool arg_active;
	bool private_mode;
	uint32_t utf8_cp;
	int utf8_need;
	int utf8_seen;
};

#if AIKB_USE_FREETYPE
struct font_ctx {
	FT_Library lib;
	FT_Face face;
	bool ready;
	int cell_w;
	int cell_h;
	int ascent;
	int descent;
	char path[256];
};
#else
struct font_ctx {
	bool ready;
	int cell_w;
	int cell_h;
	char path[1];
};
#endif

struct ui_session {
	char id[40];
	char session[32];
	char tool[32];
	char mode[20];
	char model[40];
	char reset[24];
	char state[16];
	char task[128];
	char now[160];
	float cost;
	int usage;
};

struct ui_model {
	struct ui_session sessions[MAX_SESSIONS];
	int count;
	int focus;
	int active_count;
	int avg_usage;
	float total_spend;
	char window[48];
};

struct fb_target {
	int fd;
	uint8_t *mem;
	size_t size;
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	enum rotation rotate;
	enum pixel_format pixel_format;
	uint8_t alpha;
	bool warned_msync;
	bool warned_pan;
};

static volatile sig_atomic_t g_stop;

static const uint32_t C_BG = 0x010201;
static const uint32_t C_PANEL = 0x050301;
static const uint32_t C_LINE = 0x3a240b;
static const uint32_t C_LINE2 = 0x6a410c;
static const uint32_t C_AMBER = 0xff8a00;
static const uint32_t C_YELLOW = 0xffc400;
static const uint32_t C_TEXT = 0xffe3a2;
static const uint32_t C_MUTED = 0xa77a3d;
static const uint32_t C_DIM = 0x5d421d;
static const uint32_t C_GREEN = 0x46c800;
static const uint32_t C_RED = 0xff4b2e;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void warnf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "aikb_lcd_ui: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}

static void safe_copy(char *dst, size_t dst_sz, const char *src)
{
	size_t i;

	if (!dst_sz)
		return;
	if (!src)
		src = "";
	for (i = 0; i + 1 < dst_sz && src[i]; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

static bool streq_ci(const char *a, const char *b)
{
	while (*a && *b) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return false;
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

static bool contains_ci(const char *haystack, const char *needle)
{
	size_t needle_len = strlen(needle);

	if (!needle_len)
		return true;
	for (; *haystack; haystack++) {
		size_t i;

		for (i = 0; i < needle_len; i++) {
			if (!haystack[i])
				return false;
			if (tolower((unsigned char)haystack[i]) !=
			    tolower((unsigned char)needle[i]))
				break;
		}
		if (i == needle_len)
			return true;
	}
	return false;
}

static void canvas_clear(struct canvas *c, uint32_t color)
{
	int i;
	int n = c->w * c->h;

	for (i = 0; i < n; i++)
		c->px[i] = color;
}

static void put_px(struct canvas *c, int x, int y, uint32_t color)
{
	if ((unsigned)x >= (unsigned)c->w || (unsigned)y >= (unsigned)c->h)
		return;
	c->px[y * c->w + x] = color;
}

static void fill_rect(struct canvas *c, int x, int y, int w, int h, uint32_t color)
{
	int yy;

	if (w <= 0 || h <= 0)
		return;
	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (x + w > c->w)
		w = c->w - x;
	if (y + h > c->h)
		h = c->h - y;
	if (w <= 0 || h <= 0)
		return;
	for (yy = y; yy < y + h; yy++)
		for (int xx = x; xx < x + w; xx++)
			c->px[yy * c->w + xx] = color;
}

static void hline(struct canvas *c, int x, int y, int w, uint32_t color)
{
	fill_rect(c, x, y, w, 1, color);
}

static void vline(struct canvas *c, int x, int y, int h, uint32_t color)
{
	fill_rect(c, x, y, 1, h, color);
}

static void stroke_rect(struct canvas *c, int x, int y, int w, int h, uint32_t color)
{
	hline(c, x, y, w, color);
	hline(c, x, y + h - 1, w, color);
	vline(c, x, y, h, color);
	vline(c, x + w - 1, y, h, color);
}

static void stroke_round_rect(struct canvas *c, int x, int y, int w, int h,
			      int r, uint32_t color)
{
	int inner = (r - 1) * (r - 1);
	int outer = r * r;

	if (r < 2) {
		stroke_rect(c, x, y, w, h, color);
		return;
	}

	hline(c, x + r, y, w - 2 * r, color);
	hline(c, x + r, y + h - 1, w - 2 * r, color);
	vline(c, x, y + r, h - 2 * r, color);
	vline(c, x + w - 1, y + r, h - 2 * r, color);

	for (int dy = 0; dy <= r; dy++) {
		for (int dx = 0; dx <= r; dx++) {
			int d = dx * dx + dy * dy;

			if (d < inner || d > outer)
				continue;
			put_px(c, x + r - dx, y + r - dy, color);
			put_px(c, x + w - 1 - r + dx, y + r - dy, color);
			put_px(c, x + r - dx, y + h - 1 - r + dy, color);
			put_px(c, x + w - 1 - r + dx, y + h - 1 - r + dy, color);
		}
	}
}

static void draw_char(struct canvas *c, int x, int y, unsigned char ch,
		      int scale, uint32_t color)
{
	const unsigned char *glyph = &video_fontdata[ch * VIDEO_FONT_HEIGHT];

	for (int row = 0; row < VIDEO_FONT_HEIGHT; row++) {
		unsigned char bits = glyph[row];

		for (int col = 0; col < VIDEO_FONT_WIDTH; col++) {
			if (bits & (0x80 >> col))
				fill_rect(c, x + col * scale, y + row * scale,
					  scale, scale, color);
		}
	}
}

static int text_w(const char *s, int scale)
{
	return (int)strlen(s) * VIDEO_FONT_WIDTH * scale;
}

static void draw_text(struct canvas *c, int x, int y, const char *s,
		      int scale, uint32_t color)
{
	for (; *s; s++) {
		draw_char(c, x, y, (unsigned char)*s, scale, color);
		x += VIDEO_FONT_WIDTH * scale;
	}
}

static void draw_text_fit(struct canvas *c, int x, int y, int max_w,
			  const char *s, int scale, uint32_t color)
{
	char tmp[192];
	int char_w = VIDEO_FONT_WIDTH * scale;
	int max_chars;
	size_t len;

	if (max_w <= 0)
		return;
	max_chars = max_w / char_w;
	if (max_chars <= 0)
		return;
	len = strlen(s);
	if ((int)len <= max_chars) {
		draw_text(c, x, y, s, scale, color);
		return;
	}
	if (max_chars > (int)sizeof(tmp) - 1)
		max_chars = (int)sizeof(tmp) - 1;
	memcpy(tmp, s, (size_t)max_chars);
	tmp[max_chars] = '\0';
	if (max_chars >= 1)
		tmp[max_chars - 1] = '~';
	draw_text(c, x, y, tmp, scale, color);
}

static void draw_text_right(struct canvas *c, int right, int y, const char *s,
			    int scale, uint32_t color)
{
	draw_text(c, right - text_w(s, scale), y, s, scale, color);
}

static void uppercase_copy(char *dst, size_t dst_sz, const char *src)
{
	size_t i;

	if (!dst_sz)
		return;
	for (i = 0; i + 1 < dst_sz && src[i]; i++)
		dst[i] = (char)toupper((unsigned char)src[i]);
	dst[i] = '\0';
}

#define TC_BOLD 0x01
#define TC_INVERSE 0x02
#define TC_WIDE 0x04
#define TC_TRAIL 0x08

static const uint32_t TERM_DEFAULT_FG = 0xd8dee9;
static const uint32_t TERM_DEFAULT_BG = 0x071013;
static const uint32_t TERM_STATUS_BG = 0x101820;
static const uint32_t TERM_STATUS_BLUE = 0x2f6f9f;
static const uint32_t TERM_STATUS_GREEN = 0x3f8f5f;
static const uint32_t TERM_STATUS_AMBER = 0xb87924;

static const uint32_t ansi_normal[8] = {
	0x101820, 0xbf616a, 0xa3be8c, 0xebcb8b,
	0x5e81ac, 0xb48ead, 0x88c0d0, 0xe5e9f0,
};

static const uint32_t ansi_bright[8] = {
	0x4c566a, 0xd06f79, 0xb1d196, 0xf0d98c,
	0x81a1c1, 0xc89bc0, 0x8fcedf, 0xffffff,
};

static uint32_t blend_rgb(uint32_t dst, uint32_t src, uint8_t alpha)
{
	uint32_t dr = (dst >> 16) & 0xff;
	uint32_t dg = (dst >> 8) & 0xff;
	uint32_t db = dst & 0xff;
	uint32_t sr = (src >> 16) & 0xff;
	uint32_t sg = (src >> 8) & 0xff;
	uint32_t sb = src & 0xff;
	uint32_t inv = 255u - alpha;

	return (((sr * alpha + dr * inv) / 255u) << 16) |
	       (((sg * alpha + dg * inv) / 255u) << 8) |
	       ((sb * alpha + db * inv) / 255u);
}

static bool is_powerline_cp(uint32_t cp)
{
	return cp == 0xe0b0 || cp == 0xe0b1 || cp == 0xe0b2 || cp == 0xe0b3;
}

static int term_cp_width(uint32_t cp)
{
	if (cp == 0)
		return 0;
	if (is_powerline_cp(cp))
		return 1;
	if ((cp >= 0x1100 && cp <= 0x115f) ||
	    (cp >= 0x2e80 && cp <= 0xa4cf) ||
	    (cp >= 0xac00 && cp <= 0xd7a3) ||
	    (cp >= 0xf900 && cp <= 0xfaff) ||
	    (cp >= 0xfe10 && cp <= 0xfe6f) ||
	    (cp >= 0xff00 && cp <= 0xff60) ||
	    (cp >= 0xffe0 && cp <= 0xffe6) ||
	    (cp >= 0x20000 && cp <= 0x3fffd))
		return 2;
	return 1;
}

static struct term_cell term_blank_cell(const struct terminal *t)
{
	struct term_cell cell;

	cell.ch = ' ';
	cell.fg = t->fg;
	cell.bg = t->bg;
	cell.flags = 0;
	return cell;
}

static void terminal_clear_rows(struct terminal *t, int start, int end)
{
	struct term_cell blank = term_blank_cell(t);

	if (start < 0)
		start = 0;
	if (end >= TERM_ROWS)
		end = TERM_ROWS - 1;
	for (int y = start; y <= end; y++)
		for (int x = 0; x < TERM_COLS; x++)
			t->cell[y][x] = blank;
}

static void terminal_reset(struct terminal *t)
{
	memset(t, 0, sizeof(*t));
	t->fg = TERM_DEFAULT_FG;
	t->bg = TERM_DEFAULT_BG;
	t->scroll_top = 0;
	t->scroll_bottom = TERM_ROWS - 1;
	t->state = TERM_GROUND;
	terminal_clear_rows(t, 0, TERM_ROWS - 1);
}

static void terminal_scroll_up(struct terminal *t, int top, int bottom, int n)
{
	struct term_cell blank = term_blank_cell(t);

	if (n <= 0)
		return;
	if (top < 0)
		top = 0;
	if (bottom >= TERM_ROWS)
		bottom = TERM_ROWS - 1;
	if (top > bottom)
		return;
	if (n > bottom - top + 1)
		n = bottom - top + 1;
	for (int y = top; y <= bottom - n; y++)
		memcpy(t->cell[y], t->cell[y + n], sizeof(t->cell[y]));
	for (int y = bottom - n + 1; y <= bottom; y++)
		for (int x = 0; x < TERM_COLS; x++)
			t->cell[y][x] = blank;
}

static void terminal_scroll_down(struct terminal *t, int top, int bottom, int n)
{
	struct term_cell blank = term_blank_cell(t);

	if (n <= 0)
		return;
	if (top < 0)
		top = 0;
	if (bottom >= TERM_ROWS)
		bottom = TERM_ROWS - 1;
	if (top > bottom)
		return;
	if (n > bottom - top + 1)
		n = bottom - top + 1;
	for (int y = bottom; y >= top + n; y--)
		memcpy(t->cell[y], t->cell[y - n], sizeof(t->cell[y]));
	for (int y = top; y < top + n; y++)
		for (int x = 0; x < TERM_COLS; x++)
			t->cell[y][x] = blank;
}

static void terminal_newline(struct terminal *t)
{
	if (t->row == t->scroll_bottom)
		terminal_scroll_up(t, t->scroll_top, t->scroll_bottom, 1);
	else if (t->row < TERM_ROWS - 1)
		t->row++;
}

static void terminal_put_cp(struct terminal *t, uint32_t cp)
{
	int width;
	struct term_cell cell;

	if (cp == '\n') {
		t->col = 0;
		terminal_newline(t);
		return;
	}
	if (cp == '\r') {
		t->col = 0;
		return;
	}
	if (cp == '\b') {
		if (t->col > 0)
			t->col--;
		return;
	}
	if (cp == '\t') {
		int spaces = 8 - (t->col % 8);

		while (spaces--)
			terminal_put_cp(t, ' ');
		return;
	}
	if (cp < 0x20 || cp == 0x7f)
		return;

	width = term_cp_width(cp);
	if (width <= 0)
		return;
	if (t->col + width > TERM_COLS) {
		t->col = 0;
		terminal_newline(t);
	}

	cell.ch = cp;
	cell.fg = t->fg;
	cell.bg = t->bg;
	cell.flags = (t->bold ? TC_BOLD : 0) | (t->inverse ? TC_INVERSE : 0) |
		     (width == 2 ? TC_WIDE : 0);
	t->cell[t->row][t->col] = cell;
	if (width == 2 && t->col + 1 < TERM_COLS) {
		struct term_cell trail = cell;

		trail.ch = ' ';
		trail.flags = TC_TRAIL;
		t->cell[t->row][t->col + 1] = trail;
	}
	t->col += width;
	if (t->col >= TERM_COLS) {
		t->col = 0;
		terminal_newline(t);
	}
}

static int terminal_arg(const struct terminal *t, int idx, int fallback)
{
	if (idx >= t->arg_count || t->args[idx] == 0)
		return fallback;
	return t->args[idx];
}

static void terminal_set_sgr(struct terminal *t)
{
	if (t->arg_count == 0) {
		t->fg = TERM_DEFAULT_FG;
		t->bg = TERM_DEFAULT_BG;
		t->bold = 0;
		t->inverse = 0;
		return;
	}
	for (int i = 0; i < t->arg_count; i++) {
		int n = t->args[i];

		if (n == 0) {
			t->fg = TERM_DEFAULT_FG;
			t->bg = TERM_DEFAULT_BG;
			t->bold = 0;
			t->inverse = 0;
		} else if (n == 1) {
			t->bold = 1;
		} else if (n == 22) {
			t->bold = 0;
		} else if (n == 7) {
			t->inverse = 1;
		} else if (n == 27) {
			t->inverse = 0;
		} else if (n == 39) {
			t->fg = TERM_DEFAULT_FG;
		} else if (n == 49) {
			t->bg = TERM_DEFAULT_BG;
		} else if (n >= 30 && n <= 37) {
			t->fg = ansi_normal[n - 30];
		} else if (n >= 40 && n <= 47) {
			t->bg = ansi_normal[n - 40];
		} else if (n >= 90 && n <= 97) {
			t->fg = ansi_bright[n - 90];
		} else if (n >= 100 && n <= 107) {
			t->bg = ansi_bright[n - 100];
		} else if ((n == 38 || n == 48) && i + 4 < t->arg_count &&
			   t->args[i + 1] == 2) {
			uint32_t color = ((uint32_t)(t->args[i + 2] & 0xff) << 16) |
					 ((uint32_t)(t->args[i + 3] & 0xff) << 8) |
					 (uint32_t)(t->args[i + 4] & 0xff);

			if (n == 38)
				t->fg = color;
			else
				t->bg = color;
			i += 4;
		}
	}
}

static void terminal_csi_dispatch(struct terminal *t, uint8_t final)
{
	int n;

	switch (final) {
	case 'A':
		t->row -= terminal_arg(t, 0, 1);
		if (t->row < 0)
			t->row = 0;
		break;
	case 'B':
		t->row += terminal_arg(t, 0, 1);
		if (t->row >= TERM_ROWS)
			t->row = TERM_ROWS - 1;
		break;
	case 'C':
		t->col += terminal_arg(t, 0, 1);
		if (t->col >= TERM_COLS)
			t->col = TERM_COLS - 1;
		break;
	case 'D':
		t->col -= terminal_arg(t, 0, 1);
		if (t->col < 0)
			t->col = 0;
		break;
	case 'H':
	case 'f':
		t->row = terminal_arg(t, 0, 1) - 1;
		t->col = terminal_arg(t, 1, 1) - 1;
		if (t->row < 0)
			t->row = 0;
		if (t->row >= TERM_ROWS)
			t->row = TERM_ROWS - 1;
		if (t->col < 0)
			t->col = 0;
		if (t->col >= TERM_COLS)
			t->col = TERM_COLS - 1;
		break;
	case 'J':
		n = terminal_arg(t, 0, 0);
		if (n == 2 || n == 3) {
			terminal_clear_rows(t, 0, TERM_ROWS - 1);
			t->row = 0;
			t->col = 0;
		} else if (n == 0) {
			struct term_cell blank = term_blank_cell(t);

			for (int x = t->col; x < TERM_COLS; x++)
				t->cell[t->row][x] = blank;
			terminal_clear_rows(t, t->row + 1, TERM_ROWS - 1);
		} else if (n == 1) {
			struct term_cell blank = term_blank_cell(t);

			terminal_clear_rows(t, 0, t->row - 1);
			for (int x = 0; x <= t->col && x < TERM_COLS; x++)
				t->cell[t->row][x] = blank;
		}
		break;
	case 'K': {
		struct term_cell blank = term_blank_cell(t);

		n = terminal_arg(t, 0, 0);
		if (n == 2) {
			for (int x = 0; x < TERM_COLS; x++)
				t->cell[t->row][x] = blank;
		} else if (n == 1) {
			for (int x = 0; x <= t->col && x < TERM_COLS; x++)
				t->cell[t->row][x] = blank;
		} else {
			for (int x = t->col; x < TERM_COLS; x++)
				t->cell[t->row][x] = blank;
		}
		break;
	}
	case 'L':
		terminal_scroll_down(t, t->row, t->scroll_bottom,
				     terminal_arg(t, 0, 1));
		break;
	case 'M':
		terminal_scroll_up(t, t->row, t->scroll_bottom,
				   terminal_arg(t, 0, 1));
		break;
	case 'm':
		terminal_set_sgr(t);
		break;
	case 'r':
		if (t->arg_count >= 2 && t->args[0] < t->args[1]) {
			t->scroll_top = terminal_arg(t, 0, 1) - 1;
			t->scroll_bottom = terminal_arg(t, 1, TERM_ROWS) - 1;
			if (t->scroll_top < 0)
				t->scroll_top = 0;
			if (t->scroll_bottom >= TERM_ROWS)
				t->scroll_bottom = TERM_ROWS - 1;
		} else {
			t->scroll_top = 0;
			t->scroll_bottom = TERM_ROWS - 1;
		}
		t->row = t->scroll_top;
		t->col = 0;
		break;
	case 's':
		t->saved_row = t->row;
		t->saved_col = t->col;
		break;
	case 'u':
		t->row = t->saved_row;
		t->col = t->saved_col;
		break;
	default:
		break;
	}
}

static void terminal_begin_csi(struct terminal *t)
{
	memset(t->args, 0, sizeof(t->args));
	t->arg_count = 0;
	t->cur_arg = 0;
	t->arg_active = false;
	t->private_mode = false;
}

static void terminal_process_ascii(struct terminal *t, uint8_t ch)
{
	switch (t->state) {
	case TERM_GROUND:
		if (ch == 0x1b) {
			t->state = TERM_ESC;
			return;
		}
		terminal_put_cp(t, ch);
		return;
	case TERM_ESC:
		if (ch == '[') {
			terminal_begin_csi(t);
			t->state = TERM_CSI;
		} else if (ch == ']') {
			t->state = TERM_OSC;
		} else if (ch == '7') {
			t->saved_row = t->row;
			t->saved_col = t->col;
			t->state = TERM_GROUND;
		} else if (ch == '8') {
			t->row = t->saved_row;
			t->col = t->saved_col;
			t->state = TERM_GROUND;
		} else if (ch == 'D') {
			terminal_newline(t);
			t->state = TERM_GROUND;
		} else if (ch == 'E') {
			t->col = 0;
			terminal_newline(t);
			t->state = TERM_GROUND;
		} else if (ch == 'M') {
			if (t->row == t->scroll_top)
				terminal_scroll_down(t, t->scroll_top,
						     t->scroll_bottom, 1);
			else if (t->row > 0)
				t->row--;
			t->state = TERM_GROUND;
		} else if (ch == 'c') {
			terminal_reset(t);
		} else {
			t->state = TERM_GROUND;
		}
		return;
	case TERM_CSI:
		if (ch == '?') {
			t->private_mode = true;
		} else if (isdigit(ch)) {
			if (t->arg_count == 0)
				t->arg_count = 1;
			t->args[t->arg_count - 1] =
				t->args[t->arg_count - 1] * 10 + ch - '0';
			t->arg_active = true;
		} else if (ch == ';') {
			if (t->arg_count == 0)
				t->arg_count = 1;
			if (t->arg_count < TERM_MAX_ARGS)
				t->args[t->arg_count++] = 0;
			t->arg_active = false;
		} else if (ch >= 0x40 && ch <= 0x7e) {
			terminal_csi_dispatch(t, ch);
			t->state = TERM_GROUND;
		}
		return;
	case TERM_OSC:
		if (ch == 0x07)
			t->state = TERM_GROUND;
		else if (ch == 0x1b)
			t->state = TERM_ESC;
		return;
	}
}

static void terminal_process_cp(struct terminal *t, uint32_t cp)
{
	if (cp < 0x80)
		terminal_process_ascii(t, (uint8_t)cp);
	else if (t->state == TERM_GROUND)
		terminal_put_cp(t, cp);
}

static void terminal_process_byte(struct terminal *t, uint8_t b)
{
	if (t->utf8_need) {
		if ((b & 0xc0) == 0x80) {
			t->utf8_cp = (t->utf8_cp << 6) | (uint32_t)(b & 0x3f);
			if (++t->utf8_seen >= t->utf8_need) {
				terminal_process_cp(t, t->utf8_cp);
				t->utf8_need = 0;
			}
			return;
		}
		t->utf8_need = 0;
	}
	if (b < 0x80) {
		terminal_process_ascii(t, b);
	} else if ((b & 0xe0) == 0xc0) {
		t->utf8_cp = b & 0x1f;
		t->utf8_need = 1;
		t->utf8_seen = 0;
	} else if ((b & 0xf0) == 0xe0) {
		t->utf8_cp = b & 0x0f;
		t->utf8_need = 2;
		t->utf8_seen = 0;
	} else if ((b & 0xf8) == 0xf0) {
		t->utf8_cp = b & 0x07;
		t->utf8_need = 3;
		t->utf8_seen = 0;
	}
}

static void terminal_write_string(struct terminal *t, const char *s)
{
	while (*s)
		terminal_process_byte(t, (uint8_t)*s++);
}

static void terminal_seed_mock(struct terminal *t)
{
	terminal_reset(t);
	terminal_write_string(t,
		"\x1b[2J\x1b[H"
		"\x1b[1;38;2;156;220;254mAIKB VT100 terminal\x1b[0m\r\n"
		"\x1b[38;2;163;190;140mClaude Code stream ready\x1b[0m  "
		"Type-C: /dev/ttyGS0\r\n"
		"Powerline: "
		"\x1b[38;2;255;255;255;48;2;47;111;159m AIKB "
		"\x1b[38;2;47;111;159;48;2;63;143;95m\xee\x82\xb0"
		"\x1b[38;2;255;255;255;48;2;63;143;95m VT100 "
		"\x1b[38;2;63;143;95;48;2;184;121;36m\xee\x82\xb0"
		"\x1b[38;2;255;255;255;48;2;184;121;36m Claude "
		"\x1b[38;2;184;121;36;48;2;7;16;19m\xee\x82\xb0\x1b[0m\r\n"
		"Mixed font: English + \xe4\xb8\xad\xe6\x96\x87 + "
		"Powerline \xee\x82\xb1 \xee\x82\xb3\r\n"
		"\r\n"
		"$ ");
}

#if AIKB_USE_FREETYPE
static bool font_load_face(struct font_ctx *font, const char *path)
{
	if (!path || !path[0])
		return false;
	if (access(path, R_OK) < 0)
		return false;
	if (!font->lib && FT_Init_FreeType(&font->lib))
		return false;
	if (font->face)
		FT_Done_Face(font->face);
	font->face = NULL;
	if (FT_New_Face(font->lib, path, 0, &font->face))
		return false;
	if (FT_Set_Pixel_Sizes(font->face, 0, TERM_CELL_H))
		return false;
	font->ready = true;
	font->cell_w = TERM_CELL_W;
	font->cell_h = TERM_CELL_H;
	font->ascent = (int)(font->face->size->metrics.ascender >> 6);
	font->descent = (int)(font->face->size->metrics.descender >> 6);
	safe_copy(font->path, sizeof(font->path), path);
	return true;
}

static bool font_init(struct font_ctx *font, const char *preferred)
{
	static const char *fallbacks[] = {
		"/usr/share/fonts/sarasa-gothic/SarasaMonoSC-Regular.ttf",
		"/usr/share/fonts/sarasa-gothic/SarasaTermSC-Regular.ttf",
		"/usr/share/fonts/sarasa-gothic/SarasaGothicSC-Regular.ttf",
		"/usr/share/fonts/sarasa/SarasaMonoSC-Regular.ttf",
		"/usr/share/fonts/sarasa/SarasaTermSC-Regular.ttf",
		"/usr/share/fonts/sarasa/SarasaGothicSC-Regular.ttf",
		"/usr/share/fonts/SarasaMonoSC-Regular.ttf",
		"/usr/share/fonts/SarasaTermSC-Regular.ttf",
		"/usr/share/fonts/SarasaGothicSC-Regular.ttf",
		"/usr/share/fonts/sorasa-gothic/SorasaGothic-Regular.ttf",
		"/mnt/system/fonts/SarasaMonoSC-Regular.ttf",
		"/mnt/system/fonts/SarasaTermSC-Regular.ttf",
		"/mnt/system/fonts/SarasaGothicSC-Regular.ttf",
		"/mnt/system/fonts/SorasaGothic-Regular.ttf",
		"/usr/share/fonts/wqy-zenhei/wqy-zenhei.ttc",
		"/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
	};

	memset(font, 0, sizeof(*font));
	if (font_load_face(font, preferred))
		return true;
	for (size_t i = 0; i < ARRAY_SIZE(fallbacks); i++) {
		if (font_load_face(font, fallbacks[i]))
			return true;
	}
	return false;
}

static void font_destroy(struct font_ctx *font)
{
	if (font->face)
		FT_Done_Face(font->face);
	if (font->lib)
		FT_Done_FreeType(font->lib);
	memset(font, 0, sizeof(*font));
}

static bool font_draw_cp(struct canvas *c, struct font_ctx *font, int x, int y,
			 int cell_w, uint32_t cp, uint32_t fg, uint32_t bg)
{
	FT_GlyphSlot slot;
	int baseline;
	int pen_x;

	if (!font->ready || !font->face)
		return false;
	if (FT_Load_Char(font->face, cp, FT_LOAD_RENDER))
		return false;
	slot = font->face->glyph;
	baseline = y + (TERM_CELL_H - (font->ascent - font->descent)) / 2 +
		   font->ascent;
	pen_x = x + (cell_w - (int)(slot->advance.x >> 6)) / 2 +
		slot->bitmap_left;
	if (pen_x < x)
		pen_x = x;
	for (int row = 0; row < (int)slot->bitmap.rows; row++) {
		for (int col = 0; col < (int)slot->bitmap.width; col++) {
			int dx = pen_x + col;
			int dy = baseline - slot->bitmap_top + row;
			uint8_t a;
			uint32_t dst;

			if (dx < x || dx >= x + cell_w || dy < y ||
			    dy >= y + TERM_CELL_H)
				continue;
			a = slot->bitmap.buffer[row * slot->bitmap.pitch + col];
			dst = c->px[dy * c->w + dx];
			put_px(c, dx, dy, blend_rgb(dst ? dst : bg, fg, a));
		}
	}
	return true;
}
#else
static bool font_init(struct font_ctx *font, const char *preferred)
{
	(void)preferred;
	memset(font, 0, sizeof(*font));
	return false;
}

static void font_destroy(struct font_ctx *font)
{
	(void)font;
}

static bool font_draw_cp(struct canvas *c, struct font_ctx *font, int x, int y,
			 int cell_w, uint32_t cp, uint32_t fg, uint32_t bg)
{
	(void)c; (void)font; (void)x; (void)y; (void)cell_w;
	(void)cp; (void)fg; (void)bg;
	return false;
}
#endif

static void draw_powerline_cp(struct canvas *c, int x, int y, uint32_t cp,
			      uint32_t fg, uint32_t bg)
{
	fill_rect(c, x, y, TERM_CELL_W, TERM_CELL_H, bg);
	if (cp == 0xe0b0) {
		for (int yy = 0; yy < TERM_CELL_H; yy++) {
			int w = yy < TERM_CELL_H / 2 ? yy + 1 : TERM_CELL_H - yy;

			fill_rect(c, x, y + yy, w * TERM_CELL_W / (TERM_CELL_H / 2),
				  1, fg);
		}
	} else if (cp == 0xe0b2) {
		for (int yy = 0; yy < TERM_CELL_H; yy++) {
			int w = yy < TERM_CELL_H / 2 ? yy + 1 : TERM_CELL_H - yy;
			int px = w * TERM_CELL_W / (TERM_CELL_H / 2);

			fill_rect(c, x + TERM_CELL_W - px, y + yy, px, 1, fg);
		}
	} else if (cp == 0xe0b1) {
		vline(c, x + TERM_CELL_W / 2, y + 2, TERM_CELL_H - 4, fg);
	} else if (cp == 0xe0b3) {
		vline(c, x + TERM_CELL_W / 2, y + 2, TERM_CELL_H - 4, fg);
	}
}

static void draw_unknown_cp(struct canvas *c, int x, int y, int w,
			    uint32_t fg, uint32_t bg)
{
	fill_rect(c, x, y, w, TERM_CELL_H, bg);
	stroke_rect(c, x + 1, y + 2, w - 2, TERM_CELL_H - 4, fg);
	hline(c, x + 3, y + TERM_CELL_H / 2, w - 6, fg);
}

static void draw_terminal_cell(struct canvas *c, struct font_ctx *font,
			       const struct term_cell *cell, int x, int y)
{
	uint32_t fg = cell->fg;
	uint32_t bg = cell->bg;
	int cell_w = (cell->flags & TC_WIDE) ? TERM_CELL_W * 2 : TERM_CELL_W;

	if (cell->flags & TC_TRAIL)
		return;
	if (cell->flags & TC_INVERSE) {
		uint32_t tmp = fg;

		fg = bg;
		bg = tmp;
	}
	if (cell->flags & TC_BOLD)
		fg = blend_rgb(fg, 0xffffff, 70);
	if (is_powerline_cp(cell->ch)) {
		draw_powerline_cp(c, x, y, cell->ch, fg, bg);
		return;
	}
	fill_rect(c, x, y, cell_w, TERM_CELL_H, bg);
	if (cell->ch == ' ')
		return;
	if (font_draw_cp(c, font, x, y, cell_w, cell->ch, fg, bg))
		return;
	if (cell->ch < 256)
		draw_char(c, x, y, (unsigned char)cell->ch, 1, fg);
	else
		draw_unknown_cp(c, x, y, cell_w, fg, bg);
}

static void draw_powerline_segment(struct canvas *c, int *x, int y, int h,
				   uint32_t color, uint32_t next,
				   const char *text)
{
	int pad = 10;
	int w = text_w(text, 1) + pad * 2;

	fill_rect(c, *x, y, w, h, color);
	draw_text(c, *x + pad, y + 7, text, 1, 0xffffff);
	*x += w;
	for (int yy = 0; yy < h; yy++) {
		int tw = yy < h / 2 ? yy + 1 : h - yy;
		int px = tw * 14 / (h / 2);

		fill_rect(c, *x, y + yy, px, 1, color);
		fill_rect(c, *x + px, y + yy, 14 - px, 1, next);
	}
	*x += 14;
}

static void render_terminal(struct canvas *c, const struct terminal *term,
			    struct font_ctx *font)
{
	char time_buf[16];
	int status_y = UI_H - TERM_STATUS_H;
	int x = 0;
	time_t now = time(NULL);
	struct tm tm_now;

	canvas_clear(c, TERM_DEFAULT_BG);
	for (int row = 0; row < TERM_ROWS; row++) {
		for (int col = 0; col < TERM_COLS; col++) {
			draw_terminal_cell(c, font, &term->cell[row][col],
					   TERM_PAD_X + col * TERM_CELL_W,
					   TERM_PAD_Y + row * TERM_CELL_H);
		}
	}
	if ((now & 1) == 0) {
		int cx = TERM_PAD_X + term->col * TERM_CELL_W;
		int cy = TERM_PAD_Y + term->row * TERM_CELL_H;

		fill_rect(c, cx, cy + TERM_CELL_H - 3, TERM_CELL_W, 2,
			  0x9cdcfe);
	}

	fill_rect(c, 0, status_y, UI_W, TERM_STATUS_H, TERM_STATUS_BG);
	draw_powerline_segment(c, &x, status_y, TERM_STATUS_H,
			       TERM_STATUS_BLUE, TERM_STATUS_GREEN, " AIKB ");
	draw_powerline_segment(c, &x, status_y, TERM_STATUS_H,
			       TERM_STATUS_GREEN, TERM_STATUS_AMBER, " VT100 ");
	draw_powerline_segment(c, &x, status_y, TERM_STATUS_H,
			       TERM_STATUS_AMBER, TERM_STATUS_BG, " Claude ");
	localtime_r(&now, &tm_now);
	snprintf(time_buf, sizeof(time_buf), "%02d:%02d", tm_now.tm_hour,
		 tm_now.tm_min);
	draw_text_right(c, UI_W - 12, status_y + 7, time_buf, 1, 0xd8dee9);
	if (font && font->ready)
		draw_text_fit(c, x + 8, status_y + 7, 360, font->path, 1,
			      0x8aa1b1);
}

static uint32_t state_color(const char *state)
{
	if (streq_ci(state, "WAIT") || streq_ci(state, "permission_needed"))
		return C_YELLOW;
	if (streq_ci(state, "DONE") || streq_ci(state, "done"))
		return C_GREEN;
	if (streq_ci(state, "ERR") || streq_ci(state, "ERROR") ||
	    streq_ci(state, "error"))
		return C_RED;
	if (streq_ci(state, "IDLE") || streq_ci(state, "idle"))
		return C_DIM;
	return C_GREEN;
}

static const char *state_label(const char *state)
{
	if (streq_ci(state, "WAIT") || streq_ci(state, "permission_needed"))
		return "WAIT";
	if (streq_ci(state, "DONE") || streq_ci(state, "done"))
		return "DONE";
	if (streq_ci(state, "ERR") || streq_ci(state, "ERROR") ||
	    streq_ci(state, "error"))
		return "ERR";
	if (streq_ci(state, "IDLE") || streq_ci(state, "idle"))
		return "IDLE";
	return "RUN";
}

static const char *tool_label(const char *tool)
{
	if (streq_ci(tool, "claude-code") || streq_ci(tool, "claude_code"))
		return "Claude Code";
	if (streq_ci(tool, "codex") || streq_ci(tool, "openai-codex"))
		return "Codex";
	return tool[0] ? tool : "-";
}

static void draw_usage_bar(struct canvas *c, int x, int y, int w, int h,
			   int pct, bool active)
{
	int segments = 16;
	int gap = active ? 4 : 3;
	int seg_w = (w - gap * (segments - 1)) / segments;
	int filled;

	if (seg_w < 2)
		seg_w = 2;
	if (pct < 0)
		pct = 0;
	if (pct > 100)
		pct = 100;
	filled = (pct * segments + 99) / 100;
	for (int i = 0; i < segments; i++) {
		uint32_t color = i < filled ? C_YELLOW : C_LINE;

		fill_rect(c, x + i * (seg_w + gap), y, seg_w, h, color);
	}
}

static void draw_state_dots(struct canvas *c, int x, int y, uint32_t color)
{
	for (int row = 0; row < 5; row++)
		for (int col = 0; col < 4; col++)
			fill_rect(c, x + col * 6, y + row * 5, 3, 3, color);
}

static void format_time(char *buf, size_t buf_sz)
{
	time_t now = time(NULL);
	struct tm tm_now;

	localtime_r(&now, &tm_now);
	snprintf(buf, buf_sz, "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
}

static void model_set_mock(struct ui_model *m)
{
	memset(m, 0, sizeof(*m));
	m->count = 3;
	m->focus = 1;
	m->active_count = 3;
	m->avg_usage = 62;
	m->total_spend = 41.20f;
	safe_copy(m->window, sizeof(m->window), "5h rolling window");

	safe_copy(m->sessions[0].id, sizeof(m->sessions[0].id), "SLAM");
	safe_copy(m->sessions[0].session, sizeof(m->sessions[0].session), "SLAM");
	safe_copy(m->sessions[0].tool, sizeof(m->sessions[0].tool), "Claude Code");
	safe_copy(m->sessions[0].mode, sizeof(m->sessions[0].mode), "PLAN");
	safe_copy(m->sessions[0].model, sizeof(m->sessions[0].model), "opus-4.7");
	safe_copy(m->sessions[0].reset, sizeof(m->sessions[0].reset), "1h 09m");
	safe_copy(m->sessions[0].state, sizeof(m->sessions[0].state), "RUN");
	safe_copy(m->sessions[0].task, sizeof(m->sessions[0].task), "map alignment");
	safe_copy(m->sessions[0].now, sizeof(m->sessions[0].now), "optimizing scan matcher");
	m->sessions[0].cost = 12.40f;
	m->sessions[0].usage = 63;

	safe_copy(m->sessions[1].id, sizeof(m->sessions[1].id), "WEBAPI");
	safe_copy(m->sessions[1].session, sizeof(m->sessions[1].session), "WEBAPI");
	safe_copy(m->sessions[1].tool, sizeof(m->sessions[1].tool), "Codex");
	safe_copy(m->sessions[1].mode, sizeof(m->sessions[1].mode), "AUTO");
	safe_copy(m->sessions[1].model, sizeof(m->sessions[1].model), "gpt-4.1");
	safe_copy(m->sessions[1].reset, sizeof(m->sessions[1].reset), "2h 31m");
	safe_copy(m->sessions[1].state, sizeof(m->sessions[1].state), "WAIT");
	safe_copy(m->sessions[1].task, sizeof(m->sessions[1].task), "API integration cleanup");
	safe_copy(m->sessions[1].now, sizeof(m->sessions[1].now),
		  "editing router and auth middleware");
	m->sessions[1].cost = 23.10f;
	m->sessions[1].usage = 42;

	safe_copy(m->sessions[2].id, sizeof(m->sessions[2].id), "DOCS");
	safe_copy(m->sessions[2].session, sizeof(m->sessions[2].session), "DOCS");
	safe_copy(m->sessions[2].tool, sizeof(m->sessions[2].tool), "Claude Code");
	safe_copy(m->sessions[2].mode, sizeof(m->sessions[2].mode), "REVIEW");
	safe_copy(m->sessions[2].model, sizeof(m->sessions[2].model), "sonnet-3.7");
	safe_copy(m->sessions[2].reset, sizeof(m->sessions[2].reset), "0h 42m");
	safe_copy(m->sessions[2].state, sizeof(m->sessions[2].state), "DONE");
	safe_copy(m->sessions[2].task, sizeof(m->sessions[2].task), "release notes");
	safe_copy(m->sessions[2].now, sizeof(m->sessions[2].now), "waiting for review");
	m->sessions[2].cost = 5.70f;
	m->sessions[2].usage = 81;
}

static void draw_session_row(struct canvas *c, const struct ui_session *s,
			     int y, bool active)
{
	static const int col_session = 34;
	static const int col_tool = 164;
	static const int col_mode = 286;
	static const int col_model = 374;
	static const int col_cost = 492;
	static const int col_usage = 590;
	static const int col_bar = 646;
	static const int col_reset = 796;
	static const int col_state = 876;
	char buf[64];
	char name[40];
	uint32_t state_col = state_color(s->state);

	if (active) {
		stroke_round_rect(c, 18, y - 16, UI_W - 36, 66, 8, C_LINE2);
		stroke_round_rect(c, 19, y - 15, UI_W - 38, 64, 7, C_AMBER);

		uppercase_copy(name, sizeof(name), s->session);
		draw_text_fit(c, col_session, y, 112, name, 2, C_TEXT);
		draw_text_fit(c, col_tool, y, 100, tool_label(s->tool), 2, C_YELLOW);
		draw_text_fit(c, col_mode, y, 76, s->mode, 2, C_TEXT);
		draw_text_fit(c, col_model, y, 112, s->model, 2, C_AMBER);
		snprintf(buf, sizeof(buf), "$%.2f", s->cost);
		draw_text_fit(c, col_cost, y, 90, buf, 2, C_AMBER);
		snprintf(buf, sizeof(buf), "%d%%", s->usage);
		draw_text_fit(c, col_usage, y, 56, buf, 2, C_YELLOW);
		draw_usage_bar(c, col_bar, y + 5, 132, 18, s->usage, true);
		draw_text_fit(c, col_reset, y, 76, s->reset, 2, C_TEXT);
		draw_text_fit(c, col_state, y, 64, state_label(s->state), 2, state_col);
		draw_state_dots(c, 926, y + 2, state_col);
		return;
	}

	uppercase_copy(name, sizeof(name), s->session);
	draw_text_fit(c, col_session, y, 92, name, 1, C_MUTED);
	draw_text_fit(c, col_tool, y, 104, tool_label(s->tool), 1, C_AMBER);
	draw_text_fit(c, col_mode, y, 70, s->mode, 1, C_MUTED);
	draw_text_fit(c, col_model, y, 100, s->model, 1, C_AMBER);
	snprintf(buf, sizeof(buf), "$%.2f", s->cost);
	draw_text_fit(c, col_cost, y, 76, buf, 1, C_AMBER);
	snprintf(buf, sizeof(buf), "%d%%", s->usage);
	draw_text_fit(c, col_usage, y, 48, buf, 1, C_AMBER);
	draw_usage_bar(c, col_bar, y + 2, 132, 14, s->usage, false);
	draw_text_fit(c, col_reset, y, 72, s->reset, 1, C_MUTED);
	draw_text_fit(c, col_state, y, 48, state_label(s->state), 1, state_col);
	draw_state_dots(c, 928, y - 1, state_col);
}

static void draw_bottom_status(struct canvas *c, const struct ui_session *s)
{
	char buf[64];
	int x = 80;
	int y = 356;

	stroke_round_rect(c, 20, 338, UI_W - 40, 54, 7, C_LINE2);
	stroke_round_rect(c, 21, 339, UI_W - 42, 52, 6, C_LINE);
	draw_text(c, 38, 348, ">", 3, C_AMBER);

	draw_text(c, x, y, "focus:", 1, C_MUTED);
	x += text_w("focus:", 1) + 8;
	draw_text_fit(c, x, y, 80, s->session, 1, C_AMBER);
	x += 94;
	draw_text(c, x, y, "*", 1, C_TEXT);
	x += 28;

	draw_text(c, x, y, "task:", 1, C_MUTED);
	x += text_w("task:", 1) + 8;
	draw_text_fit(c, x, y, 220, s->task, 1, C_AMBER);
	x += 244;
	draw_text(c, x, y, "*", 1, C_TEXT);
	x += 28;

	draw_text(c, x, y, "cost:", 1, C_MUTED);
	x += text_w("cost:", 1) + 8;
	snprintf(buf, sizeof(buf), "$%.2f", s->cost);
	draw_text_fit(c, x, y, 70, buf, 1, C_AMBER);
	x += 92;
	draw_text(c, x, y, "*", 1, C_TEXT);
	x += 28;

	draw_text(c, x, y, "now:", 1, C_MUTED);
	x += text_w("now:", 1) + 8;
	draw_text_fit(c, x, y, UI_W - x - 40, s->now, 1, C_AMBER);
}

static void render_dashboard(struct canvas *c, const struct ui_model *m)
{
	char buf[96];
	char time_buf[16];
	int focus = m->focus;

	canvas_clear(c, C_BG);
	fill_rect(c, 8, 8, UI_W - 16, UI_H - 16, C_PANEL);
	stroke_round_rect(c, 8, 8, UI_W - 16, UI_H - 16, 12, C_LINE);

	draw_text(c, 32, 34, "OVERVIEW", 2, C_TEXT);
	snprintf(buf, sizeof(buf), "%d active", m->active_count);
	draw_text(c, 302, 42, buf, 1, C_AMBER);
	draw_text(c, 376, 42, "*", 1, C_TEXT);
	draw_text(c, 406, 42, m->window[0] ? m->window : "5h rolling window", 1, C_MUTED);
	draw_text(c, 548, 42, "*", 1, C_TEXT);
	snprintf(buf, sizeof(buf), "avg %d%%", m->avg_usage);
	draw_text(c, 582, 42, buf, 1, C_MUTED);
	draw_text(c, 664, 42, "*", 1, C_TEXT);
	snprintf(buf, sizeof(buf), "total spend $%.2f", m->total_spend);
	draw_text(c, 698, 42, buf, 1, C_MUTED);
	format_time(time_buf, sizeof(time_buf));
	draw_text_right(c, UI_W - 34, 42, time_buf, 1, C_AMBER);

	hline(c, 22, 76, UI_W - 44, C_LINE);

	draw_text(c, 34, 100, "SESSION", 1, C_MUTED);
	draw_text(c, 176, 100, "TOOL", 1, C_MUTED);
	draw_text(c, 286, 100, "MODE", 1, C_MUTED);
	draw_text(c, 374, 100, "MODEL", 1, C_MUTED);
	draw_text(c, 496, 100, "COST", 1, C_MUTED);
	draw_text(c, 590, 100, "PLAN USAGE LIMITS", 1, C_MUTED);
	draw_text(c, 792, 100, "RESETS IN", 1, C_MUTED);
	draw_text(c, 884, 100, "STATE", 1, C_MUTED);
	hline(c, 22, 128, UI_W - 44, C_LINE);

	if (m->count <= 0) {
		draw_text(c, 366, 196, "WAITING FOR TYPE-C DATA", 1, C_MUTED);
		return;
	}

	if (focus < 0 || focus >= m->count)
		focus = 0;

	if (focus > 0)
		draw_session_row(c, &m->sessions[focus - 1], 154, false);
	draw_session_row(c, &m->sessions[focus], 207, true);
	if (focus + 1 < m->count)
		draw_session_row(c, &m->sessions[focus + 1], 286, false);

	hline(c, 22, 326, UI_W - 44, C_LINE);
	draw_bottom_status(c, &m->sessions[focus]);
}

static const char *find_json_key(const char *line, const char *key)
{
	char pattern[80];
	const char *p;

	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	p = strstr(line, pattern);
	if (!p)
		return NULL;
	p += strlen(pattern);
	while (*p && isspace((unsigned char)*p))
		p++;
	if (*p != ':')
		return NULL;
	p++;
	while (*p && isspace((unsigned char)*p))
		p++;
	return p;
}

static bool json_get_string(const char *line, const char *key,
			    char *out, size_t out_sz)
{
	const char *p = find_json_key(line, key);
	size_t n = 0;

	if (!p || !out_sz)
		return false;
	if (*p == '"') {
		p++;
		while (*p && *p != '"' && n + 1 < out_sz) {
			if (*p == '\\' && p[1])
				p++;
			out[n++] = *p++;
		}
		out[n] = '\0';
		return true;
	}
	while (*p && *p != ',' && *p != '}' && !isspace((unsigned char)*p) &&
	       n + 1 < out_sz)
		out[n++] = *p++;
	out[n] = '\0';
	return n > 0;
}

static bool json_get_int(const char *line, const char *key, int *out)
{
	char tmp[32];

	if (!json_get_string(line, key, tmp, sizeof(tmp)))
		return false;
	*out = atoi(tmp);
	return true;
}

static bool json_get_float(const char *line, const char *key, float *out)
{
	char tmp[32];

	if (!json_get_string(line, key, tmp, sizeof(tmp)))
		return false;
	*out = strtof(tmp, NULL);
	return true;
}

static int find_session(const struct ui_model *m, const char *id)
{
	for (int i = 0; i < m->count; i++) {
		if (streq_ci(m->sessions[i].id, id) ||
		    streq_ci(m->sessions[i].session, id))
			return i;
	}
	return -1;
}

static int upsert_session(struct ui_model *m, const char *id)
{
	int idx;

	if (!id || !id[0])
		id = "session";
	idx = find_session(m, id);
	if (idx >= 0)
		return idx;
	if (m->count >= MAX_SESSIONS)
		return MAX_SESSIONS - 1;
	idx = m->count++;
	memset(&m->sessions[idx], 0, sizeof(m->sessions[idx]));
	safe_copy(m->sessions[idx].id, sizeof(m->sessions[idx].id), id);
	safe_copy(m->sessions[idx].session, sizeof(m->sessions[idx].session), id);
	safe_copy(m->sessions[idx].tool, sizeof(m->sessions[idx].tool), "Claude Code");
	safe_copy(m->sessions[idx].mode, sizeof(m->sessions[idx].mode), "PLAN");
	safe_copy(m->sessions[idx].model, sizeof(m->sessions[idx].model), "-");
	safe_copy(m->sessions[idx].reset, sizeof(m->sessions[idx].reset), "-");
	safe_copy(m->sessions[idx].state, sizeof(m->sessions[idx].state), "RUN");
	safe_copy(m->sessions[idx].task, sizeof(m->sessions[idx].task), "-");
	safe_copy(m->sessions[idx].now, sizeof(m->sessions[idx].now), "waiting for updates");
	return idx;
}

static void normalize_status(char *state, size_t state_sz, const char *status)
{
	if (streq_ci(status, "permission_needed") || streq_ci(status, "permission") ||
	    streq_ci(status, "wait"))
		safe_copy(state, state_sz, "WAIT");
	else if (streq_ci(status, "done") || streq_ci(status, "stop") ||
		 streq_ci(status, "session_end"))
		safe_copy(state, state_sz, "DONE");
	else if (streq_ci(status, "error"))
		safe_copy(state, state_sz, "ERR");
	else if (streq_ci(status, "idle"))
		safe_copy(state, state_sz, "IDLE");
	else
		safe_copy(state, state_sz, "RUN");
}

static void apply_session_fields(struct ui_session *s, const char *line)
{
	char tmp[192];
	int n;
	float f;

	if (json_get_string(line, "session", tmp, sizeof(tmp)) ||
	    json_get_string(line, "name", tmp, sizeof(tmp)))
		safe_copy(s->session, sizeof(s->session), tmp);
	if (json_get_string(line, "tool", tmp, sizeof(tmp)) ||
	    json_get_string(line, "source", tmp, sizeof(tmp)))
		safe_copy(s->tool, sizeof(s->tool), tool_label(tmp));
	if (json_get_string(line, "mode", tmp, sizeof(tmp)) ||
	    json_get_string(line, "permission_mode", tmp, sizeof(tmp)))
		safe_copy(s->mode, sizeof(s->mode), tmp);
	if (json_get_string(line, "model", tmp, sizeof(tmp)))
		safe_copy(s->model, sizeof(s->model), tmp);
	if (json_get_string(line, "reset", tmp, sizeof(tmp)) ||
	    json_get_string(line, "resets_in", tmp, sizeof(tmp)))
		safe_copy(s->reset, sizeof(s->reset), tmp);
	if (json_get_string(line, "state", tmp, sizeof(tmp)) ||
	    json_get_string(line, "status", tmp, sizeof(tmp)))
		normalize_status(s->state, sizeof(s->state), tmp);
	if (json_get_string(line, "task", tmp, sizeof(tmp)) ||
	    json_get_string(line, "last_message", tmp, sizeof(tmp)) ||
	    json_get_string(line, "tool_name", tmp, sizeof(tmp)))
		safe_copy(s->task, sizeof(s->task), tmp);
	if (json_get_string(line, "now", tmp, sizeof(tmp)) ||
	    json_get_string(line, "last_ai_output", tmp, sizeof(tmp)) ||
	    json_get_string(line, "tool_input", tmp, sizeof(tmp)))
		safe_copy(s->now, sizeof(s->now), tmp);
	if (json_get_float(line, "cost", &f) ||
	    json_get_float(line, "cost_usd", &f))
		s->cost = f;
	if (json_get_int(line, "usage", &n) ||
	    json_get_int(line, "context_pct", &n))
		s->usage = n;
}

static void apply_json_line(struct ui_model *m, const char *line)
{
	char type[48] = "";
	char id[64] = "";
	char tmp[192];
	int idx;
	int n;
	float f;

	json_get_string(line, "type", type, sizeof(type));

	if (streq_ci(type, "clear")) {
		memset(m, 0, sizeof(*m));
		safe_copy(m->window, sizeof(m->window), "5h rolling window");
		return;
	}

	if (streq_ci(type, "summary") || streq_ci(type, "overview") ||
	    streq_ci(type, "snapshot")) {
		if (json_get_int(line, "active", &n) ||
		    json_get_int(line, "active_count", &n))
			m->active_count = n;
		if (json_get_int(line, "avg", &n) ||
		    json_get_int(line, "avg_usage", &n))
			m->avg_usage = n;
		if (json_get_float(line, "total", &f) ||
		    json_get_float(line, "total_spend", &f))
			m->total_spend = f;
		if (json_get_string(line, "window", tmp, sizeof(tmp)))
			safe_copy(m->window, sizeof(m->window), tmp);
		if (json_get_string(line, "focus", tmp, sizeof(tmp))) {
			idx = find_session(m, tmp);
			if (idx >= 0)
				m->focus = idx;
		}
		return;
	}

	if (!json_get_string(line, "id", id, sizeof(id)) &&
	    !json_get_string(line, "session_id", id, sizeof(id)))
		json_get_string(line, "session", id, sizeof(id));
	idx = upsert_session(m, id);
	apply_session_fields(&m->sessions[idx], line);

	if (streq_ci(type, "SessionStart") || streq_ci(type, "session_start") ||
	    streq_ci(type, "init")) {
		safe_copy(m->sessions[idx].state, sizeof(m->sessions[idx].state), "RUN");
		m->focus = idx;
	} else if (streq_ci(type, "PreToolUse") || streq_ci(type, "permission") ||
		   streq_ci(type, "permission_request")) {
		safe_copy(m->sessions[idx].state, sizeof(m->sessions[idx].state), "WAIT");
		m->focus = idx;
	} else if (streq_ci(type, "PostToolUse") || streq_ci(type, "tool_use") ||
		   streq_ci(type, "UserPromptSubmit") || streq_ci(type, "message")) {
		safe_copy(m->sessions[idx].state, sizeof(m->sessions[idx].state), "RUN");
		m->focus = idx;
	} else if (streq_ci(type, "Stop") || streq_ci(type, "SessionEnd") ||
		   streq_ci(type, "session_end") || streq_ci(type, "exit")) {
		safe_copy(m->sessions[idx].state, sizeof(m->sessions[idx].state), "DONE");
		m->focus = idx;
	} else if (streq_ci(type, "Notification") &&
		   (contains_ci(line, "permission") || contains_ci(line, "tool_input"))) {
		safe_copy(m->sessions[idx].state, sizeof(m->sessions[idx].state), "WAIT");
		m->focus = idx;
	}
}

static enum pixel_format resolve_pixel_format(enum pixel_format req,
					      const struct fb_target *fb)
{
	if (req != PIXEL_FMT_AUTO)
		return req;
	if (fb->var.bits_per_pixel == 32 && !strncmp(fb->fix.id, "cvifb", 5))
		return PIXEL_FMT_ARGB8888;
	return PIXEL_FMT_FB_FIELDS;
}

static const char *pixel_format_name(enum pixel_format fmt)
{
	switch (fmt) {
	case PIXEL_FMT_FB_FIELDS:
		return "fb";
	case PIXEL_FMT_ARGB8888:
		return "argb8888";
	case PIXEL_FMT_ABGR8888:
		return "abgr8888";
	default:
		return "auto";
	}
}

static uint32_t pack_fb_fields_pixel(uint32_t rgb,
				     const struct fb_var_screeninfo *v,
				     uint8_t alpha)
{
	uint32_t r = (rgb >> 16) & 0xff;
	uint32_t g = (rgb >> 8) & 0xff;
	uint32_t b = rgb & 0xff;
	uint32_t a = alpha;
	uint32_t out = 0;
	uint32_t max;

	if (v->red.length) {
		max = (1u << v->red.length) - 1u;
		out |= ((r * max + 127u) / 255u) << v->red.offset;
	}
	if (v->green.length) {
		max = (1u << v->green.length) - 1u;
		out |= ((g * max + 127u) / 255u) << v->green.offset;
	}
	if (v->blue.length) {
		max = (1u << v->blue.length) - 1u;
		out |= ((b * max + 127u) / 255u) << v->blue.offset;
	}
	if (v->transp.length) {
		max = (1u << v->transp.length) - 1u;
		out |= ((a * max + 127u) / 255u) << v->transp.offset;
	}
	return out;
}

static uint32_t pack_fb_pixel(uint32_t rgb, const struct fb_target *fb)
{
	uint32_t r = (rgb >> 16) & 0xff;
	uint32_t g = (rgb >> 8) & 0xff;
	uint32_t b = rgb & 0xff;
	uint32_t a = fb->alpha;
	enum pixel_format fmt = resolve_pixel_format(fb->pixel_format, fb);

	if (fb->var.bits_per_pixel == 32) {
		if (fmt == PIXEL_FMT_ARGB8888)
			return (a << 24) | (r << 16) | (g << 8) | b;
		if (fmt == PIXEL_FMT_ABGR8888)
			return (a << 24) | (b << 16) | (g << 8) | r;
	}
	return pack_fb_fields_pixel(rgb, &fb->var, fb->alpha);
}

static enum rotation resolve_rotation(enum rotation req, int fb_w, int fb_h)
{
	if (req != ROT_AUTO)
		return req;
	if (fb_w == UI_H && fb_h == UI_W)
		return ROT_CW;
	return ROT_NONE;
}

static uint32_t sample_canvas(const struct canvas *c, int dst_x, int dst_y,
			      int dst_w, int dst_h, enum rotation rot)
{
	int sx;
	int sy;

	if (rot == ROT_CW) {
		sx = dst_y * c->w / dst_h;
		sy = c->h - 1 - (dst_x * c->h / dst_w);
	} else if (rot == ROT_CCW) {
		sx = c->w - 1 - (dst_y * c->w / dst_h);
		sy = dst_x * c->h / dst_w;
	} else {
		sx = dst_x * c->w / dst_w;
		sy = dst_y * c->h / dst_h;
	}
	if (sx < 0)
		sx = 0;
	if (sx >= c->w)
		sx = c->w - 1;
	if (sy < 0)
		sy = 0;
	if (sy >= c->h)
		sy = c->h - 1;
	return c->px[sy * c->w + sx];
}

static void fb_refresh(struct fb_target *fb)
{
	if (msync(fb->mem, fb->size, MS_SYNC) < 0 && !fb->warned_msync) {
		warnf("msync framebuffer failed: %s", strerror(errno));
		fb->warned_msync = true;
	}
	if (ioctl(fb->fd, FBIOPAN_DISPLAY, &fb->var) < 0 && !fb->warned_pan) {
		warnf("FBIOPAN_DISPLAY failed: %s", strerror(errno));
		fb->warned_pan = true;
	}
}

static void fb_blit(struct fb_target *fb, const struct canvas *c)
{
	int width = (int)fb->var.xres;
	int height = (int)fb->var.yres;
	int bpp = (int)fb->var.bits_per_pixel;
	int bytes = (bpp + 7) / 8;
	enum rotation rot = resolve_rotation(fb->rotate, width, height);

	for (int y = 0; y < height; y++) {
		uint8_t *row = fb->mem + (size_t)y * fb->fix.line_length;

		for (int x = 0; x < width; x++) {
			uint32_t rgb = sample_canvas(c, x, y, width, height, rot);
			uint32_t pix = pack_fb_pixel(rgb, fb);
			uint8_t *p = row + (size_t)x * bytes;

			if ((size_t)(p - fb->mem + bytes) > fb->size)
				continue;
			switch (bytes) {
			case 2:
				p[0] = (uint8_t)(pix & 0xff);
				p[1] = (uint8_t)((pix >> 8) & 0xff);
				break;
			case 3:
				p[0] = (uint8_t)(pix & 0xff);
				p[1] = (uint8_t)((pix >> 8) & 0xff);
				p[2] = (uint8_t)((pix >> 16) & 0xff);
				break;
			default:
				p[0] = (uint8_t)(pix & 0xff);
				p[1] = (uint8_t)((pix >> 8) & 0xff);
				p[2] = (uint8_t)((pix >> 16) & 0xff);
				p[3] = (uint8_t)((pix >> 24) & 0xff);
				break;
			}
		}
	}
	fb_refresh(fb);
}

static int fb_open_target(struct fb_target *fb, const char *path,
			  enum rotation rotate, enum pixel_format pixel_format,
			  uint8_t alpha)
{
	memset(fb, 0, sizeof(*fb));
	fb->fd = -1;
	fb->fd = open(path, O_RDWR);
	if (fb->fd < 0) {
		warnf("open %s failed: %s", path, strerror(errno));
		return -1;
	}
	if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->var) < 0 ||
	    ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->fix) < 0) {
		warnf("FBIOGET_* failed: %s", strerror(errno));
		close(fb->fd);
		return -1;
	}
	fb->size = (size_t)fb->fix.line_length * fb->var.yres;
	fb->mem = mmap(NULL, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fb->fd, 0);
	if (fb->mem == MAP_FAILED) {
		warnf("mmap framebuffer failed: %s", strerror(errno));
		close(fb->fd);
		return -1;
	}
	fb->rotate = rotate;
	fb->pixel_format = pixel_format;
	fb->alpha = alpha;
	warnf("fb %s: %ux%u %ubpp line=%u rotate=%s pixel=%s alpha=%u",
	      path, fb->var.xres,
	      fb->var.yres, fb->var.bits_per_pixel, fb->fix.line_length,
	      resolve_rotation(rotate, fb->var.xres, fb->var.yres) == ROT_CW ? "cw" :
	      resolve_rotation(rotate, fb->var.xres, fb->var.yres) == ROT_CCW ? "ccw" :
	      "none", pixel_format_name(resolve_pixel_format(pixel_format, fb)),
	      fb->alpha);
	return 0;
}

static void fb_close_target(struct fb_target *fb)
{
	if (fb->mem && fb->mem != MAP_FAILED)
		munmap(fb->mem, fb->size);
	if (fb->fd >= 0)
		close(fb->fd);
}

static int write_ppm(const char *path, const struct canvas *c)
{
	FILE *fp = fopen(path, "wb");

	if (!fp) {
		warnf("open %s failed: %s", path, strerror(errno));
		return -1;
	}
	fprintf(fp, "P6\n%d %d\n255\n", c->w, c->h);
	for (int y = 0; y < c->h; y++) {
		for (int x = 0; x < c->w; x++) {
			uint32_t rgb = c->px[y * c->w + x];
			uint8_t b[3] = {
				(uint8_t)((rgb >> 16) & 0xff),
				(uint8_t)((rgb >> 8) & 0xff),
				(uint8_t)(rgb & 0xff),
			};
			fwrite(b, 1, sizeof(b), fp);
		}
	}
	fclose(fp);
	return 0;
}

static int configure_serial_fd(int fd)
{
	struct termios tio;

	if (tcgetattr(fd, &tio) < 0)
		return -1;
	cfmakeraw(&tio);
	cfsetispeed(&tio, B115200);
	cfsetospeed(&tio, B115200);
	tio.c_cflag |= CLOCAL | CREAD;
	return tcsetattr(fd, TCSANOW, &tio);
}

static int open_input(const char *path)
{
	int fd;

	if (!path)
		return -1;
	if (strcmp(path, "-") == 0)
		fd = STDIN_FILENO;
	else
		fd = open(path, O_RDONLY | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		warnf("open input %s failed: %s", path, strerror(errno));
		return -1;
	}
	if (fd != STDIN_FILENO && configure_serial_fd(fd) < 0)
		warnf("serial setup for %s failed: %s", path, strerror(errno));
	return fd;
}

static void process_input_fd(int fd, struct ui_model *m, struct terminal *term,
			     enum app_view view)
{
	static char buf[LINE_BUF];
	static size_t len;
	char tmp[256];
	ssize_t rd;

	for (;;) {
		rd = read(fd, tmp, sizeof(tmp));
		if (rd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			warnf("input read failed: %s", strerror(errno));
			return;
		}
		if (rd == 0)
			return;
		for (ssize_t i = 0; i < rd; i++) {
			char ch = tmp[i];

			if (view == VIEW_TERMINAL) {
				terminal_process_byte(term, (uint8_t)ch);
				continue;
			}
			if (ch == '\r')
				continue;
			if (ch == '\n') {
				buf[len] = '\0';
				if (len > 0)
					apply_json_line(m, buf);
				len = 0;
			} else if (len + 1 < sizeof(buf)) {
				buf[len++] = ch;
			} else {
				len = 0;
			}
		}
	}
}

static enum rotation parse_rotation(const char *s)
{
	if (streq_ci(s, "none"))
		return ROT_NONE;
	if (streq_ci(s, "cw"))
		return ROT_CW;
	if (streq_ci(s, "ccw"))
		return ROT_CCW;
	return ROT_AUTO;
}

static enum pixel_format parse_pixel_format(const char *s)
{
	if (streq_ci(s, "fb") || streq_ci(s, "fb-fields"))
		return PIXEL_FMT_FB_FIELDS;
	if (streq_ci(s, "argb8888") || streq_ci(s, "argb"))
		return PIXEL_FMT_ARGB8888;
	if (streq_ci(s, "abgr8888") || streq_ci(s, "abgr"))
		return PIXEL_FMT_ABGR8888;
	return PIXEL_FMT_AUTO;
}

static int parse_view(const char *s, enum app_view *out)
{
	if (streq_ci(s, "terminal") || streq_ci(s, "term") ||
	    streq_ci(s, "vt100")) {
		*out = VIEW_TERMINAL;
		return 0;
	}
	if (streq_ci(s, "dashboard") || streq_ci(s, "panel") ||
	    streq_ci(s, "json")) {
		*out = VIEW_DASHBOARD;
		return 0;
	}
	return -1;
}

static int parse_int_arg(const char *s, int min, int max, int *out)
{
	char *end = NULL;
	long value;

	errno = 0;
	value = strtol(s, &end, 0);
	if (errno || !end || *end || value < min || value > max)
		return -1;
	*out = (int)value;
	return 0;
}

static void render_app(struct canvas *c, enum app_view view,
		       const struct ui_model *model, const struct terminal *term,
		       struct font_ctx *font)
{
	if (view == VIEW_TERMINAL)
		render_terminal(c, term, font);
	else
		render_dashboard(c, model);
}

static void usage(const char *argv0)
{
	printf("Usage: %s [options]\n", argv0);
	printf("  --fb PATH          framebuffer path (default /dev/fb0)\n");
	printf("  --input PATH       terminal bytes or newline JSON input, '-' for stdin\n");
	printf("  --view MODE        terminal/vt100 or dashboard/json (default terminal)\n");
	printf("  --font PATH        preferred TrueType/OpenType font path\n");
	printf("  --rotate MODE      auto, none, cw, ccw (default auto)\n");
	printf("  --pixel-format FMT auto, fb, argb8888, abgr8888 (default auto)\n");
	printf("  --alpha N          framebuffer alpha byte, 0..255 (default 255)\n");
	printf("  --dump-ppm PATH    render one frame to a PPM file\n");
	printf("  --once             draw one framebuffer frame, hold briefly, and exit\n");
	printf("  --hold MS          hold time for --once, 0..60000 (default 3000)\n");
	printf("  --no-mock          start with an empty UI model\n");
	printf("  --help             show this help\n");
}

int main(int argc, char **argv)
{
	const char *fb_path = "/dev/fb0";
	const char *input_path = NULL;
	const char *dump_path = NULL;
	const char *font_path = NULL;
	enum rotation rotate = ROT_AUTO;
	enum pixel_format pixel_format = PIXEL_FMT_AUTO;
	enum app_view view = VIEW_TERMINAL;
	bool once = false;
	bool mock = true;
	int alpha = 255;
	int once_hold_ms = 3000;
	struct canvas c = {
		.w = UI_W,
		.h = UI_H,
		.px = NULL,
	};
	struct ui_model model;
	struct terminal term;
	struct font_ctx font;
	struct fb_target fb;
	int input_fd = -1;
	int ret = 0;
	time_t next_input_retry = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--fb") && i + 1 < argc) {
			fb_path = argv[++i];
		} else if (!strcmp(argv[i], "--input") && i + 1 < argc) {
			input_path = argv[++i];
		} else if (!strcmp(argv[i], "--view") && i + 1 < argc) {
			if (parse_view(argv[++i], &view) < 0) {
				usage(argv[0]);
				return 2;
			}
		} else if (!strcmp(argv[i], "--font") && i + 1 < argc) {
			font_path = argv[++i];
		} else if (!strcmp(argv[i], "--rotate") && i + 1 < argc) {
			rotate = parse_rotation(argv[++i]);
		} else if (!strcmp(argv[i], "--pixel-format") && i + 1 < argc) {
			pixel_format = parse_pixel_format(argv[++i]);
		} else if (!strcmp(argv[i], "--alpha") && i + 1 < argc) {
			if (parse_int_arg(argv[++i], 0, 255, &alpha) < 0) {
				usage(argv[0]);
				return 2;
			}
		} else if (!strcmp(argv[i], "--dump-ppm") && i + 1 < argc) {
			dump_path = argv[++i];
		} else if (!strcmp(argv[i], "--once")) {
			once = true;
		} else if (!strcmp(argv[i], "--hold") && i + 1 < argc) {
			if (parse_int_arg(argv[++i], 0, 60000, &once_hold_ms) < 0) {
				usage(argv[0]);
				return 2;
			}
		} else if (!strcmp(argv[i], "--no-mock")) {
			mock = false;
		} else if (!strcmp(argv[i], "--help")) {
			usage(argv[0]);
			return 0;
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	c.px = calloc((size_t)c.w * c.h, sizeof(*c.px));
	if (!c.px) {
		warnf("canvas allocation failed");
		return 1;
	}

	if (mock)
		model_set_mock(&model);
	else {
		memset(&model, 0, sizeof(model));
		safe_copy(model.window, sizeof(model.window), "5h rolling window");
	}
	if (mock)
		terminal_seed_mock(&term);
	else
		terminal_reset(&term);
	if (!font_init(&font, font_path))
		warnf("freetype font unavailable; falling back to built-in 8x16 glyphs");

	render_app(&c, view, &model, &term, &font);
	if (dump_path) {
		ret = write_ppm(dump_path, &c);
		font_destroy(&font);
		free(c.px);
		return ret ? 1 : 0;
	}

	if (fb_open_target(&fb, fb_path, rotate, pixel_format,
			   (uint8_t)alpha) < 0) {
		font_destroy(&font);
		free(c.px);
		return 1;
	}

	input_fd = open_input(input_path);
	if (input_fd < 0 && input_path)
		next_input_retry = time(NULL) + 5;
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	while (!g_stop) {
		struct pollfd pfd;
		int timeout = 1000;

		render_app(&c, view, &model, &term, &font);
		fb_blit(&fb, &c);
		if (once) {
			if (once_hold_ms > 0)
				usleep((useconds_t)once_hold_ms * 1000);
			break;
		}

		if (input_fd < 0) {
			time_t now = time(NULL);

			if (input_path && now >= next_input_retry) {
				input_fd = open_input(input_path);
				next_input_retry = now + 5;
				if (input_fd >= 0)
					continue;
			}
			usleep((useconds_t)timeout * 1000);
			continue;
		}
		pfd.fd = input_fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, timeout) > 0 && (pfd.revents & POLLIN))
			process_input_fd(input_fd, &model, &term, view);
	}

	if (input_fd >= 0 && input_fd != STDIN_FILENO)
		close(input_fd);
	fb_close_target(&fb);
	font_destroy(&font);
	free(c.px);
	return ret;
}
