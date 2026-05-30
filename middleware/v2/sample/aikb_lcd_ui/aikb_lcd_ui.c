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

#include "kitty_graphics.h"

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
#define UI_PIXELS ((size_t)UI_W * (size_t)UI_H)
#define SPLASH_BYTES (UI_PIXELS * sizeof(uint32_t))
#define MAX_SESSIONS 8
#define LINE_BUF 1024
#define TERM_PAD_X 8
#define TERM_PAD_Y 8
#define TERM_STATUS_H 28
/*
 * Cell geometry is runtime-tunable. The visible LCD grid is still capped by
 * TERM_ROWS_VISIBLE_MAX, but the backing store is taller so fullscreen TUIs
 * created for a large Windows Terminal can be viewed around the active cursor
 * instead of showing only their blank top-left corner.
 */
#define TERM_CELL_W_MIN 8
#define TERM_CELL_H_MIN 16
#define TERM_COLS_VISIBLE_MAX ((UI_W - TERM_PAD_X * 2) / TERM_CELL_W_MIN)
#define TERM_COLS_MAX 200
#define TERM_ROWS_VISIBLE_MAX ((UI_H - TERM_STATUS_H - TERM_PAD_Y) / TERM_CELL_H_MIN)
#define TERM_ROWS_MAX 96
#define TERM_SCROLLBACK_ROWS 1024
#define TERM_SCROLL_LINES_PER_EVENT 3
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
	VIEW_PET,
	/* Diagnostic-only local picker. Normal SESSION handling is owned by
	 * the host daemon so the board cannot display stale built-in rows. */
	VIEW_SESSION_PICKER,
};

enum pet_mood {
	PET_IDLE,
	PET_ASKING,
	PET_CODING,
	PET_REVIEWING,
	PET_ERROR,
	PET_SLEEP,
};

enum pet_scene {
	PET_SCENE_ASKING,
	PET_SCENE_UPDATING,
	PET_SCENE_LISTENING,
	PET_SCENE_FAULT,
	PET_SCENE_STANDBY,
	PET_SCENE_COUNT,
};

/* Per-key UI action: when aikb_hid_input emits "KEY N DOWN", keep the
 * matching title and pick the pet scene that should decorate the next host
 * panel. Order is HID bit index, which mirrors aikb_hid_input's "KEY N"
 * naming (KEY 0 = bit 0). Encoder push is host-owned and is intentionally
 * not listed here, so it cannot locally flip VOICE into ASKING. */
struct key_action {
	const char       *event_line;
	const char       *label;
	enum pet_scene    scene;
};
static const struct key_action KEY_ACTIONS[] = {
	{"KEY 0 DOWN",   "REJECT",  PET_SCENE_FAULT},
	{"KEY 1 DOWN",   "VOICE",   PET_SCENE_LISTENING},
	{"KEY 2 DOWN",   "SESSION", PET_SCENE_ASKING},
	{"KEY 3 DOWN",   "REVIEW",  PET_SCENE_ASKING},
	{"KEY 4 DOWN",   "MODEL",   PET_SCENE_UPDATING},
	{"KEY 5 DOWN",   "MULTI",   PET_SCENE_ASKING},
	{"KEY 6 DOWN",   "CONFIRM", PET_SCENE_UPDATING},
};

/* Set true the first time the host pushes any terminal byte. Used by the
 * terminal view to fall back to a "WAITING FOR HOST TERMINAL STREAM" hint
 * before any data has arrived (e.g. user picked a session via SESSION
 * picker but the host hasn't streamed VT100 yet). */
static bool g_terminal_has_data;

enum pet_pose {
	PET_POSE_IDLE,
	PET_POSE_THINKING,
	PET_POSE_HAPPY,
	PET_POSE_CONFUSED,
	PET_POSE_SLEEPY,
	PET_POSE_COUNT,
};

enum pet_emotion {
	PET_EMOTION_CALM,
	PET_EMOTION_CURIOUS,
	PET_EMOTION_HAPPY,
	PET_EMOTION_CONFUSED,
	PET_EMOTION_SLEEPY,
};

enum term_state {
	TERM_GROUND,
	TERM_ESC,
	TERM_CSI,
	TERM_OSC,
	TERM_ST_STRING,
	TERM_STRING_ESC,
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

struct cell_preset {
	int w;
	int h;
};

static const struct cell_preset CELL_PRESETS[] = {
	{  8, 16 },
	{ 10, 20 },
	{ 12, 24 },
	{ 16, 32 },
};
#define CELL_PRESET_COUNT ((int)(sizeof(CELL_PRESETS) / sizeof(CELL_PRESETS[0])))

static int g_cell_w = 12;
static int g_cell_h = 24;
static int g_cols;
static int g_rows;

/*
 * Boot splash: when --splash points at a UI_W*UI_H raw 0x00RRGGBB buffer
 * (canvas-native uint32 layout), the render loop blits it instead of the
 * terminal/dashboard until the first byte arrives on the input FIFO. Lets the
 * panel show vibe-promo art while waiting for a wrapper to attach.
 */
static uint32_t *g_splash;
static bool g_show_splash;
static uint32_t *g_ui_shell;

/*
 * Animations: --boot-anim and --wait-anim each take an "AKIM" container
 * (see scripts/make_boot_anim.py). flags bit 0 (ANIM_FLAG_LOOP) makes the
 * sequence repeat forever; otherwise it plays once and the slot is released.
 *
 * Render priority: g_boot_anim (one-shot, plays first) > g_wait_anim (loops,
 * picks up after boot anim ends) > idle sleep > splash fallback >
 * terminal/dashboard.
 * Host bytes alone do not release these animations; a board key or explicit
 * view change does, so the host cannot steal the startup/sleep surface.
 */
#define ANIM_MAGIC_STR  "AKIM"
#define ANIM_FLAG_LOOP  0x1u
#define PET_AKIM_FLAG_ARGB8888 0x2u
#define IDLE_SLEEP_TIMEOUT_MS (180u * 1000u)
#ifndef PET_ASKING_AKIM_PATH
#define PET_ASKING_AKIM_PATH "/mnt/system/usr/share/aikb/pet/asking.akim"
#endif
#ifndef PET_UPDATING_AKIM_PATH
#define PET_UPDATING_AKIM_PATH "/mnt/system/usr/share/aikb/pet/updating.akim"
#endif
#ifndef PET_LISTENING_AKIM_PATH
#define PET_LISTENING_AKIM_PATH "/mnt/system/usr/share/aikb/pet/listening.akim"
#endif
#ifndef PET_FAULT_AKIM_PATH
#define PET_FAULT_AKIM_PATH "/mnt/system/usr/share/aikb/pet/fault.akim"
#endif
#ifndef PET_STANDBY_AKIM_PATH
#define PET_STANDBY_AKIM_PATH "/mnt/system/usr/share/aikb/pet/standby.akim"
#endif
struct anim_header {
	char     magic[4];
	uint32_t version;
	uint32_t frame_count;
	uint32_t frame_delay_ms;
	uint32_t width;
	uint32_t height;
	uint32_t flags;
};
struct anim_state {
	const uint8_t   *base;
	size_t           size;
	uint32_t         frame_count;
	uint32_t         frame_delay_ms;
	uint32_t         frame_idx;
	struct timespec  started_at;
	bool             active;
	bool             loop;
	bool             argb8888;
	const char      *label; /* for warnf */
};
static struct anim_state g_boot_anim = { .label = "boot-anim" };
static struct anim_state g_wait_anim = { .label = "wait-anim" };
static struct anim_state g_idle_sleep_anim = { .label = "idle-sleep" };
static struct anim_state g_pet_anims[PET_SCENE_COUNT] = {
	[PET_SCENE_ASKING] = { .label = "pet-asking" },
	[PET_SCENE_UPDATING] = { .label = "pet-updating" },
	[PET_SCENE_LISTENING] = { .label = "pet-listening" },
	[PET_SCENE_FAULT] = { .label = "pet-fault" },
	[PET_SCENE_STANDBY] = { .label = "pet-standby" },
};
static bool g_pet_anim_argb8888[PET_SCENE_COUNT];
static char g_pet_asset_root[256];
static char g_pet_asset_paths[PET_SCENE_COUNT][256];
static bool g_pet_force_fallback;
static uint64_t g_last_local_input_ms;
static bool g_idle_sleep_active;

static int find_cell_preset(int w, int h)
{
	for (int i = 0; i < CELL_PRESET_COUNT; i++) {
		if (CELL_PRESETS[i].w == w && CELL_PRESETS[i].h == h)
			return i;
	}
	return -1;
}

struct terminal {
	struct term_cell cell[TERM_ROWS_MAX][TERM_COLS_MAX];
	struct term_cell scrollback[TERM_SCROLLBACK_ROWS][TERM_COLS_MAX];
	int rows;
	int cols;
	int viewport_top;
	int scrollback_start;
	int scrollback_count;
	int scrollback_offset;
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
	bool clear_graphics;
	bool fullscreen_mode;
	bool pending_full_clear;
	bool pending_scrollback_clear;
	uint64_t diag_bytes;
	uint64_t diag_printable;
	uint64_t diag_full_clears;
	uint64_t diag_scrollback_rows;
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
	int face_index;
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
	int focus;        /* session currently shown in terminal/dashboard rows */
	int picker_idx;   /* session highlighted in VIEW_SESSION_PICKER */
	int active_count;
	int avg_usage;
	float total_spend;
	char window[48];
};

/* Board-owned session table populated from aikb_hid_input's --ctrl-out lines
 * (`session N state X`, `session N removed`). The picker view renders straight
 * out of this table; only sid + state are shown. */
#define MAX_BOARD_SESSIONS 16
enum board_session_state {
	BSS_CONNECTED = 0,
	BSS_DISCONNECTED,
	BSS_RUN,
	BSS_WAIT,
	BSS_DONE,
	BSS_ERROR,
};
/* Per-session token/turn/permission/meta state pushed from host (M4.2).
 * Strings are bounded so the struct stays under ~280B per slot;
 * truncation is done in aikb_hid_input's ctrl-out writer, not here. */
struct board_session_token {
	uint64_t input;
	uint64_t output;
	uint64_t cost_cents;
};

struct board_session_turn {
	char role[12];
	char text[64];
	uint64_t updated_ms;
};

struct board_session_perm {
	uint64_t req_id;
	char tool[24];
	char args[64];
	bool active;
};

struct board_session_meta {
	char kind[12];
	char hint[48];
	char cwd[64];
	char branch[24];
};

struct board_session {
	bool used;
	uint16_t sid;
	enum board_session_state state;
	struct board_session_token token;
	struct board_session_turn turn;
	struct board_session_perm perm;
	struct board_session_meta meta;
};
static struct board_session g_board_sessions[MAX_BOARD_SESSIONS];
static uint16_t g_lcd_selected_sid;   /* current highlight in picker */
static uint16_t g_lcd_active_sid;     /* sid the user last focused (terminal view) */
static int g_ui_ctrl_out_fd = -1;
static const char *g_ui_ctrl_out_path;

struct pet_animation_state {
	enum pet_pose pose;
	enum pet_emotion emotion;
	uint64_t pose_started_ms;
};

struct pet_state {
	enum pet_mood mood;
	enum pet_scene scene;
	struct pet_animation_state anim;
	uint32_t frame_index;
	uint64_t start_time_ms;
	uint64_t last_interaction_ms;
	char title[16];
	char message[128];
	int energy;
	int affection;
	int focus;
	int progress;
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
static const uint32_t C_GRUVBOX_BG = 0x1d2021;
static const uint32_t C_GRUVBOX_DARK1 = 0x3c3836;
static const uint32_t C_GRUVBOX_LINE = 0xebdbb2;
static const uint32_t C_GRUVBOX_TEXT = 0xfbf1c7;
static const uint32_t C_GRUVBOX_MUTED = 0xbdae93;
static const uint32_t C_GRUVBOX_YELLOW = 0xfabd2f;
static const uint32_t C_GRUVBOX_BLUE = 0x83a598;
static const uint32_t C_GRUVBOX_RED = 0xfb4934;

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

static uint64_t monotonic_now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return ((uint64_t)ts.tv_sec * 1000u) +
	       ((uint64_t)ts.tv_nsec / 1000000u);
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

static uint32_t ansi_256_color(int idx)
{
	static const uint8_t cube[6] = {0, 95, 135, 175, 215, 255};
	int r;
	int g;
	int b;

	if (idx < 0)
		idx = 0;
	if (idx < 8)
		return ansi_normal[idx];
	if (idx < 16)
		return ansi_bright[idx - 8];
	if (idx >= 16 && idx <= 231) {
		idx -= 16;
		r = idx / 36;
		g = (idx / 6) % 6;
		b = idx % 6;
		return ((uint32_t)cube[r] << 16) |
		       ((uint32_t)cube[g] << 8) |
		       (uint32_t)cube[b];
	}
	if (idx > 255)
		idx = 255;
	idx -= 232;
	r = 8 + idx * 10;
	return ((uint32_t)r << 16) | ((uint32_t)r << 8) | (uint32_t)r;
}

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

static bool is_box_drawing_cp(uint32_t cp)
{
	return cp == 0x2500 || cp == 0x2502 ||
	       cp == 0x256d || cp == 0x256e ||
	       cp == 0x2570 || cp == 0x256f;
}

static int term_cp_width(uint32_t cp)
{
	if (cp == 0)
		return 0;
	if (is_powerline_cp(cp) || is_box_drawing_cp(cp))
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

static struct term_cell term_clear_cell(void)
{
	struct term_cell cell;

	cell.ch = ' ';
	cell.fg = TERM_DEFAULT_FG;
	cell.bg = TERM_DEFAULT_BG;
	cell.flags = 0;
	return cell;
}

static int terminal_cols(const struct terminal *t)
{
	int cols = t && t->cols > 0 ? t->cols : TERM_COLS_MAX;

	if (cols > TERM_COLS_MAX)
		cols = TERM_COLS_MAX;
	if (cols < 1)
		cols = 1;
	return cols;
}

static void terminal_clear_rows(struct terminal *t, int start, int end)
{
	struct term_cell blank = term_clear_cell();
	int cols = terminal_cols(t);

	if (start < 0)
		start = 0;
	if (end >= t->rows)
		end = t->rows - 1;
	for (int y = start; y <= end; y++)
		for (int x = 0; x < cols; x++)
			t->cell[y][x] = blank;
}

static void terminal_clear_scrollback(struct terminal *t)
{
	t->scrollback_start = 0;
	t->scrollback_count = 0;
	t->scrollback_offset = 0;
}

static void terminal_clamp_scrollback(struct terminal *t)
{
	int max_offset = t->scrollback_count + t->viewport_top;

	if (max_offset < 0)
		max_offset = 0;
	if (t->scrollback_offset > max_offset)
		t->scrollback_offset = max_offset;
	if (t->scrollback_offset < 0)
		t->scrollback_offset = 0;
}

static void terminal_push_scrollback_row(struct terminal *t,
					 const struct term_cell *row)
{
	int slot;

	if (TERM_SCROLLBACK_ROWS <= 0 || !row)
		return;
	if (t->scrollback_count < TERM_SCROLLBACK_ROWS) {
		slot = (t->scrollback_start + t->scrollback_count) %
		       TERM_SCROLLBACK_ROWS;
		t->scrollback_count++;
	} else {
		slot = t->scrollback_start;
		t->scrollback_start =
			(t->scrollback_start + 1) % TERM_SCROLLBACK_ROWS;
	}
	memcpy(t->scrollback[slot], row, sizeof(t->scrollback[slot]));
	t->diag_scrollback_rows++;
	if (t->scrollback_offset > 0)
		t->scrollback_offset++;
	terminal_clamp_scrollback(t);
}

static void terminal_scroll_view(struct terminal *t, int dir)
{
	if (dir == 0)
		return;
	t->scrollback_offset += dir * TERM_SCROLL_LINES_PER_EVENT;
	terminal_clamp_scrollback(t);
}

static void terminal_update_viewport(struct terminal *t)
{
	int max_top = t->rows - g_rows;

	if (t->fullscreen_mode) {
		t->viewport_top = 0;
		return;
	}
	if (max_top < 0)
		max_top = 0;
	if (t->row < t->viewport_top)
		t->viewport_top = t->row;
	else if (t->row >= t->viewport_top + g_rows)
		t->viewport_top = t->row - g_rows + 1;
	if (t->viewport_top > max_top)
		t->viewport_top = max_top;
	if (t->viewport_top < 0)
		t->viewport_top = 0;
	terminal_clamp_scrollback(t);
}

static void terminal_ensure_row(struct terminal *t, int row)
{
	int old_rows;

	if (row < 0)
		return;
	if (row >= TERM_ROWS_MAX)
		row = TERM_ROWS_MAX - 1;
	if (t->rows <= 0)
		t->rows = g_rows;
	if (row < t->rows)
		return;
	old_rows = t->rows;
	t->rows = row + 1;
	terminal_clear_rows(t, old_rows, t->rows - 1);
	if (t->scroll_bottom < old_rows - 1)
		return;
	t->scroll_bottom = t->rows - 1;
}

static void terminal_apply_pending_full_clear(struct terminal *t)
{
	if (!t->pending_full_clear)
		return;
	t->pending_full_clear = false;
	terminal_clear_rows(t, 0, t->rows - 1);
	if (t->pending_scrollback_clear) {
		terminal_clear_scrollback(t);
		t->pending_scrollback_clear = false;
	}
	t->clear_graphics = true;
	t->diag_full_clears++;
}

static void terminal_reset(struct terminal *t)
{
	memset(t, 0, sizeof(*t));
	t->fg = TERM_DEFAULT_FG;
	t->bg = TERM_DEFAULT_BG;
	t->rows = g_rows;
	t->cols = TERM_COLS_MAX;
	t->viewport_top = 0;
	t->scroll_top = 0;
	t->scroll_bottom = t->rows - 1;
	t->state = TERM_GROUND;
	t->clear_graphics = true;
	t->fullscreen_mode = false;
	t->pending_full_clear = false;
	t->pending_scrollback_clear = false;
	terminal_clear_rows(t, 0, t->rows - 1);
	terminal_clear_scrollback(t);
}

static void terminal_scroll_up(struct terminal *t, int top, int bottom, int n)
{
	struct term_cell blank = term_clear_cell();
	int cols = terminal_cols(t);

	if (n <= 0)
		return;
	if (top < 0)
		top = 0;
	if (bottom >= t->rows)
		bottom = t->rows - 1;
	if (top > bottom)
		return;
	if (n > bottom - top + 1)
		n = bottom - top + 1;
	if (top == 0 && bottom == t->rows - 1)
		for (int y = 0; y < n; y++)
			terminal_push_scrollback_row(t, t->cell[y]);
	for (int y = top; y <= bottom - n; y++)
		memcpy(t->cell[y], t->cell[y + n], sizeof(t->cell[y]));
	for (int y = bottom - n + 1; y <= bottom; y++)
		for (int x = 0; x < cols; x++)
			t->cell[y][x] = blank;
}

static void terminal_scroll_down(struct terminal *t, int top, int bottom, int n)
{
	struct term_cell blank = term_clear_cell();
	int cols = terminal_cols(t);

	if (n <= 0)
		return;
	if (top < 0)
		top = 0;
	if (bottom >= t->rows)
		bottom = t->rows - 1;
	if (top > bottom)
		return;
	if (n > bottom - top + 1)
		n = bottom - top + 1;
	for (int y = bottom; y >= top + n; y--)
		memcpy(t->cell[y], t->cell[y - n], sizeof(t->cell[y]));
	for (int y = top; y < top + n; y++)
		for (int x = 0; x < cols; x++)
			t->cell[y][x] = blank;
}

static void terminal_newline(struct terminal *t)
{
	if (t->row == t->scroll_bottom)
		terminal_scroll_up(t, t->scroll_top, t->scroll_bottom, 1);
	else if (t->row < t->rows - 1)
		t->row++;
	terminal_update_viewport(t);
}

static void terminal_put_cp(struct terminal *t, uint32_t cp)
{
	int width;
	int cols;
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
	if (cp != ' ')
		terminal_apply_pending_full_clear(t);

	width = term_cp_width(cp);
	if (width <= 0)
		return;
	t->diag_printable++;
	terminal_ensure_row(t, t->row);
	cols = terminal_cols(t);
	if (t->col + width > cols) {
		t->col = 0;
		terminal_newline(t);
		terminal_ensure_row(t, t->row);
	}

	cell.ch = cp;
	cell.fg = t->fg;
	cell.bg = t->bg;
	cell.flags = (t->bold ? TC_BOLD : 0) | (t->inverse ? TC_INVERSE : 0) |
		     (width == 2 ? TC_WIDE : 0);
	t->cell[t->row][t->col] = cell;
	if (width == 2 && t->col + 1 < cols) {
		struct term_cell trail = cell;

		trail.ch = ' ';
		trail.flags = TC_TRAIL;
		t->cell[t->row][t->col + 1] = trail;
	}
	t->col += width;
	if (t->col >= cols) {
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

static void terminal_clamp_cursor(struct terminal *t)
{
	int cols = terminal_cols(t);

	if (t->row < 0)
		t->row = 0;
	if (t->row >= TERM_ROWS_MAX)
		t->row = TERM_ROWS_MAX - 1;
	terminal_ensure_row(t, t->row);
	if (t->row >= t->rows)
		t->row = t->rows - 1;
	if (t->col < 0)
		t->col = 0;
	if (t->col >= cols)
		t->col = cols - 1;
	terminal_update_viewport(t);
}

static void terminal_erase_chars(struct terminal *t, int n)
{
	struct term_cell blank = term_blank_cell(t);
	int cols = terminal_cols(t);

	if (n <= 0)
		n = 1;
	if (t->col + n > cols)
		n = cols - t->col;
	for (int x = 0; x < n; x++)
		t->cell[t->row][t->col + x] = blank;
}

static void terminal_insert_chars(struct terminal *t, int n)
{
	struct term_cell blank = term_blank_cell(t);
	int cols = terminal_cols(t);

	if (n <= 0)
		n = 1;
	if (n > cols - t->col)
		n = cols - t->col;
	for (int x = cols - 1; x >= t->col + n; x--)
		t->cell[t->row][x] = t->cell[t->row][x - n];
	for (int x = 0; x < n; x++)
		t->cell[t->row][t->col + x] = blank;
}

static void terminal_delete_chars(struct terminal *t, int n)
{
	struct term_cell blank = term_blank_cell(t);
	int cols = terminal_cols(t);

	if (n <= 0)
		n = 1;
	if (n > cols - t->col)
		n = cols - t->col;
	for (int x = t->col; x < cols - n; x++)
		t->cell[t->row][x] = t->cell[t->row][x + n];
	for (int x = cols - n; x < cols; x++)
		t->cell[t->row][x] = blank;
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
		} else if ((n == 38 || n == 48) && i + 2 < t->arg_count &&
			   t->args[i + 1] == 5) {
			uint32_t color = ansi_256_color(t->args[i + 2]);

			if (n == 38)
				t->fg = color;
			else
				t->bg = color;
			i += 2;
		}
	}
}

static bool terminal_has_private_mode(const struct terminal *t, int mode)
{
	if (!t->private_mode)
		return false;
	for (int i = 0; i < t->arg_count; i++) {
		if (t->args[i] == mode)
			return true;
	}
	return false;
}

static void terminal_csi_dispatch(struct terminal *t, uint8_t final)
{
	int n;

	switch (final) {
	case '@':
		terminal_insert_chars(t, terminal_arg(t, 0, 1));
		break;
	case 'A':
		t->row -= terminal_arg(t, 0, 1);
		terminal_clamp_cursor(t);
		break;
	case 'B':
	case 'e':
		t->row += terminal_arg(t, 0, 1);
		terminal_clamp_cursor(t);
		break;
	case 'C':
	case 'a':
		t->col += terminal_arg(t, 0, 1);
		terminal_clamp_cursor(t);
		break;
	case 'D':
		t->col -= terminal_arg(t, 0, 1);
		terminal_clamp_cursor(t);
		break;
	case 'E':
		t->row += terminal_arg(t, 0, 1);
		t->col = 0;
		terminal_clamp_cursor(t);
		break;
	case 'F':
		t->row -= terminal_arg(t, 0, 1);
		t->col = 0;
		terminal_clamp_cursor(t);
		break;
	case 'G':
	case '`':
		t->col = terminal_arg(t, 0, 1) - 1;
		terminal_clamp_cursor(t);
		break;
	case 'I':
		t->col += terminal_arg(t, 0, 1) * 8;
		terminal_clamp_cursor(t);
		break;
	case 'Z':
		t->col -= terminal_arg(t, 0, 1) * 8;
		terminal_clamp_cursor(t);
		break;
	case 'H':
	case 'f':
		t->row = terminal_arg(t, 0, 1) - 1;
		t->col = terminal_arg(t, 1, 1) - 1;
		terminal_clamp_cursor(t);
		break;
	case 'J':
		n = terminal_arg(t, 0, 0);
		if (n == 2 || n == 3) {
			t->pending_full_clear = true;
			if (n == 3)
				t->pending_scrollback_clear = true;
			t->row = 0;
			t->col = 0;
			t->viewport_top = 0;
			terminal_clamp_scrollback(t);
		} else if (n == 0) {
			struct term_cell blank = term_blank_cell(t);
			int cols = terminal_cols(t);

			for (int x = t->col; x < cols; x++)
				t->cell[t->row][x] = blank;
			terminal_clear_rows(t, t->row + 1, t->rows - 1);
		} else if (n == 1) {
			struct term_cell blank = term_blank_cell(t);
			int cols = terminal_cols(t);

			terminal_clear_rows(t, 0, t->row - 1);
			for (int x = 0; x <= t->col && x < cols; x++)
				t->cell[t->row][x] = blank;
		}
		break;
	case 'K': {
		struct term_cell blank = term_blank_cell(t);
		int cols = terminal_cols(t);

		n = terminal_arg(t, 0, 0);
		if (n == 2) {
			for (int x = 0; x < cols; x++)
				t->cell[t->row][x] = blank;
		} else if (n == 1) {
			for (int x = 0; x <= t->col && x < cols; x++)
				t->cell[t->row][x] = blank;
		} else {
			for (int x = t->col; x < cols; x++)
				t->cell[t->row][x] = blank;
		}
		break;
	}
	case 'P':
		terminal_delete_chars(t, terminal_arg(t, 0, 1));
		break;
	case 'S':
		terminal_scroll_up(t, t->scroll_top, t->scroll_bottom,
				   terminal_arg(t, 0, 1));
		break;
	case 'T':
		terminal_scroll_down(t, t->scroll_top, t->scroll_bottom,
				     terminal_arg(t, 0, 1));
		break;
	case 'X':
		terminal_erase_chars(t, terminal_arg(t, 0, 1));
		break;
	case 'd':
		t->row = terminal_arg(t, 0, 1) - 1;
		terminal_clamp_cursor(t);
		break;
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
	case 'b':
	case 'c':
	case 'g':
	case 'n':
	case 'p':
	case 'q':
		break;
	case 'h':
	case 'l':
		/* DEC private modes such as ?25 and ?1049 are display modes.
		 * The framebuffer view does not need to model them; treating
		 * them as no-ops keeps fullscreen TUIs from derailing parsing. */
		if (terminal_has_private_mode(t, 47) ||
		    terminal_has_private_mode(t, 1047) ||
		    terminal_has_private_mode(t, 1049)) {
			t->fullscreen_mode = final == 'h';
			t->viewport_top = 0;
		}
		break;
	case 'r':
		if (t->arg_count >= 2 && t->args[0] < t->args[1]) {
			t->scroll_top = terminal_arg(t, 0, 1) - 1;
			t->scroll_bottom = terminal_arg(t, 1, t->rows) - 1;
			if (t->scroll_top < 0)
				t->scroll_top = 0;
			terminal_ensure_row(t, t->scroll_bottom);
			if (t->scroll_bottom >= t->rows)
				t->scroll_bottom = t->rows - 1;
		} else {
			t->scroll_top = 0;
			t->scroll_bottom = t->rows - 1;
		}
		t->row = t->scroll_top;
		t->col = 0;
		terminal_clamp_cursor(t);
		break;
	case 's':
		t->saved_row = t->row;
		t->saved_col = t->col;
		break;
	case 'u':
		t->row = t->saved_row;
		t->col = t->saved_col;
		terminal_clamp_cursor(t);
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
		} else if (ch == 'P' || ch == 'X' || ch == '^' || ch == '_') {
			t->state = TERM_ST_STRING;
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
		} else if (ch == ';' || ch == ':') {
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
	case TERM_ST_STRING:
		if (ch == 0x07)
			t->state = TERM_GROUND;
		else if (ch == 0x1b)
			t->state = TERM_STRING_ESC;
		return;
	case TERM_STRING_ESC:
		if (ch == '\\')
			t->state = TERM_GROUND;
		else
			t->state = TERM_ST_STRING;
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
	t->diag_bytes++;
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
		"\x1b[38;2;163;190;140mHost CLI stream ready\x1b[0m  "
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
static int font_face_index_for_path(const char *path)
{
	const char *name;

	if (!path)
		return 0;
	name = strrchr(path, '/');
	name = name ? name + 1 : path;
	/*
	 * wqy-zenhei.ttc contains a proportional face at index 0 and a mono face
	 * at index 1. Terminal cells need the mono face so wide ASCII glyphs such
	 * as m/w/% do not crowd a fixed cell.
	 */
	if (strstr(name, "wqy-zenhei.ttc"))
		return 1;
	return 0;
}

static bool font_load_face(struct font_ctx *font, const char *path)
{
	int face_index;

	if (!path || !path[0])
		return false;
	if (access(path, R_OK) < 0)
		return false;
	if (!font->lib && FT_Init_FreeType(&font->lib))
		return false;
	if (font->face)
		FT_Done_Face(font->face);
	font->face = NULL;
	face_index = font_face_index_for_path(path);
	if (FT_New_Face(font->lib, path, face_index, &font->face)) {
		if (face_index == 0 ||
		    FT_New_Face(font->lib, path, 0, &font->face))
			return false;
		face_index = 0;
	}
	if (FT_Set_Pixel_Sizes(font->face, 0, g_cell_h))
		return false;
	font->ready = true;
	font->cell_w = g_cell_w;
	font->cell_h = g_cell_h;
	font->ascent = (int)(font->face->size->metrics.ascender >> 6);
	font->descent = (int)(font->face->size->metrics.descender >> 6);
	font->face_index = face_index;
	if (face_index > 0)
		snprintf(font->path, sizeof(font->path), "%s#%d", path,
			 face_index);
	else
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
	baseline = y + (g_cell_h - (font->ascent - font->descent)) / 2 +
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
			    dy >= y + g_cell_h)
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

static void recompute_term_geom(void)
{
	g_cols = (UI_W - TERM_PAD_X * 2) / g_cell_w;
	g_rows = (UI_H - TERM_STATUS_H - TERM_PAD_Y) / g_cell_h;
	if (g_cols > TERM_COLS_VISIBLE_MAX)
		g_cols = TERM_COLS_VISIBLE_MAX;
	if (g_rows > TERM_ROWS_VISIBLE_MAX)
		g_rows = TERM_ROWS_VISIBLE_MAX;
	if (g_cols < 1)
		g_cols = 1;
	if (g_rows < 1)
		g_rows = 1;
}

/*
 * Switch to a runtime cell preset. Re-anchors g_cell_w/h, g_cols/rows, FreeType
 * pixel size and ascent/descent, then clears the visible terminal cells so
 * stale glyphs do not bleed through after the geometry shrinks.
 */
static void apply_cell_size(struct font_ctx *font, struct terminal *t,
			    int new_w, int new_h)
{
	if (find_cell_preset(new_w, new_h) < 0) {
		warnf("ignoring unsupported cell size %dx%d", new_w, new_h);
		return;
	}
	g_cell_w = new_w;
	g_cell_h = new_h;
	recompute_term_geom();

#if AIKB_USE_FREETYPE
	if (font && font->face &&
	    FT_Set_Pixel_Sizes(font->face, 0, (FT_UInt)g_cell_h) == 0) {
		font->cell_w = g_cell_w;
		font->cell_h = g_cell_h;
		font->ascent = (int)(font->face->size->metrics.ascender >> 6);
		font->descent = (int)(font->face->size->metrics.descender >> 6);
	}
#else
	(void)font;
#endif

	t->rows = g_rows;
	t->cols = TERM_COLS_MAX;
	t->viewport_top = 0;
	if (t->row >= t->rows)
		t->row = t->rows - 1;
	if (t->col >= t->cols)
		t->col = t->cols - 1;
	t->scroll_top = 0;
	t->scroll_bottom = t->rows - 1;
	terminal_clear_rows(t, 0, t->rows - 1);
	terminal_clear_scrollback(t);
	t->clear_graphics = true;
}

static void draw_powerline_cp(struct canvas *c, int x, int y, uint32_t cp,
			      uint32_t fg, uint32_t bg)
{
	fill_rect(c, x, y, g_cell_w, g_cell_h, bg);
	if (cp == 0xe0b0) {
		for (int yy = 0; yy < g_cell_h; yy++) {
			int w = yy < g_cell_h / 2 ? yy + 1 : g_cell_h - yy;

			fill_rect(c, x, y + yy, w * g_cell_w / (g_cell_h / 2),
				  1, fg);
		}
	} else if (cp == 0xe0b2) {
		for (int yy = 0; yy < g_cell_h; yy++) {
			int w = yy < g_cell_h / 2 ? yy + 1 : g_cell_h - yy;
			int px = w * g_cell_w / (g_cell_h / 2);

			fill_rect(c, x + g_cell_w - px, y + yy, px, 1, fg);
		}
	} else if (cp == 0xe0b1) {
		vline(c, x + g_cell_w / 2, y + 2, g_cell_h - 4, fg);
	} else if (cp == 0xe0b3) {
		vline(c, x + g_cell_w / 2, y + 2, g_cell_h - 4, fg);
	}
}

static void draw_box_line_h(struct canvas *c, int x, int y, int w,
			    int cy, int thick, uint32_t fg)
{
	fill_rect(c, x, y + cy - thick / 2, w, thick, fg);
}

static void draw_box_line_v(struct canvas *c, int x, int y, int cx,
			    int h, int thick, uint32_t fg)
{
	fill_rect(c, x + cx - thick / 2, y, thick, h, fg);
}

static void draw_box_corner(struct canvas *c, int x, int y, uint32_t cp,
			    int cx, int cy, int thick, uint32_t fg)
{
	int px = x + cx - thick / 2;
	int py = y + cy - thick / 2;

	fill_rect(c, px, py, thick, thick, fg);
	if (cp == 0x256d) { /* ╭ */
		draw_box_line_h(c, x + cx, y, g_cell_w - cx, cy, thick, fg);
		draw_box_line_v(c, x, y + cy, cx, g_cell_h - cy, thick, fg);
	} else if (cp == 0x256e) { /* ╮ */
		draw_box_line_h(c, x, y, cx + 1, cy, thick, fg);
		draw_box_line_v(c, x, y + cy, cx, g_cell_h - cy, thick, fg);
	} else if (cp == 0x2570) { /* ╰ */
		draw_box_line_v(c, x, y, cx, cy + 1, thick, fg);
		draw_box_line_h(c, x + cx, y, g_cell_w - cx, cy, thick, fg);
	} else if (cp == 0x256f) { /* ╯ */
		draw_box_line_v(c, x, y, cx, cy + 1, thick, fg);
		draw_box_line_h(c, x, y, cx + 1, cy, thick, fg);
	}
}

static void draw_box_drawing_cp(struct canvas *c, int x, int y, uint32_t cp,
				uint32_t fg, uint32_t bg)
{
	int thick = g_cell_h >= 24 ? 2 : 1;
	int cx = g_cell_w / 2;
	int cy = g_cell_h / 2;

	fill_rect(c, x, y, g_cell_w, g_cell_h, bg);
	if (cp == 0x2500) { /* ─ */
		draw_box_line_h(c, x, y, g_cell_w, cy, thick, fg);
	} else if (cp == 0x2502) { /* │ */
		draw_box_line_v(c, x, y, cx, g_cell_h, thick, fg);
	} else {
		draw_box_corner(c, x, y, cp, cx, cy, thick, fg);
	}
}

static void draw_unknown_cp(struct canvas *c, int x, int y, int w,
			    uint32_t fg, uint32_t bg)
{
	fill_rect(c, x, y, w, g_cell_h, bg);
	stroke_rect(c, x + 1, y + 2, w - 2, g_cell_h - 4, fg);
	hline(c, x + 3, y + g_cell_h / 2, w - 6, fg);
}

static void draw_terminal_cell(struct canvas *c, struct font_ctx *font,
			       const struct term_cell *cell, int x, int y)
{
	uint32_t fg = cell->fg;
	uint32_t bg = cell->bg;
	int cell_w = (cell->flags & TC_WIDE) ? g_cell_w * 2 : g_cell_w;

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
	if (is_box_drawing_cp(cell->ch)) {
		draw_box_drawing_cp(c, x, y, cell->ch, fg, bg);
		return;
	}
	fill_rect(c, x, y, cell_w, g_cell_h, bg);
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

static const struct board_session *terminal_active_session(void)
{
	if (g_lcd_active_sid == 0)
		return NULL;
	for (int i = 0; i < MAX_BOARD_SESSIONS; i++) {
		if (g_board_sessions[i].used &&
		    g_board_sessions[i].sid == g_lcd_active_sid)
			return &g_board_sessions[i];
	}
	return NULL;
}

static const char *terminal_session_kind_label(const struct board_session *s)
{
	if (!s)
		return "No SID";
	if (streq_ci(s->meta.kind, "claude"))
		return "Claude";
	if (streq_ci(s->meta.kind, "codex"))
		return "Codex";
	if (streq_ci(s->meta.kind, "vscode"))
		return "VS Code";
	if (streq_ci(s->meta.kind, "cursor"))
		return "Cursor";
	if (streq_ci(s->meta.kind, "browser"))
		return "Browser";
	if (streq_ci(s->meta.kind, "terminal") ||
	    strstr(s->meta.hint, "Terminal"))
		return "Terminal";
	return "Session";
}

static void terminal_status_segment(char *buf, size_t buf_sz)
{
	const struct board_session *s = terminal_active_session();

	if (!s) {
		snprintf(buf, buf_sz, " No SID ");
		return;
	}
	snprintf(buf, buf_sz, " %s #%u ", terminal_session_kind_label(s),
		 s->sid);
}

static bool terminal_row_has_text(const struct terminal *term, int row)
{
	int cols = terminal_cols(term);

	if (row < 0 || row >= term->rows)
		return false;
	for (int col = 0; col < cols; col++) {
		const struct term_cell *cell = &term->cell[row][col];

		if ((cell->flags & TC_TRAIL) == 0 && cell->ch != ' ')
			return true;
	}
	return false;
}

static const struct term_cell *terminal_virtual_row(const struct terminal *term,
						    int row)
{
	int idx;

	if (row < 0)
		return NULL;
	if (row < term->scrollback_count) {
		idx = (term->scrollback_start + row) % TERM_SCROLLBACK_ROWS;
		return term->scrollback[idx];
	}
	row -= term->scrollback_count;
	if (row >= term->rows)
		return NULL;
	return term->cell[row];
}

static int terminal_non_empty_cells(const struct terminal *term)
{
	int count = 0;
	int cols = terminal_cols(term);

	for (int row = 0; row < term->rows; row++) {
		for (int col = 0; col < cols; col++) {
			const struct term_cell *cell = &term->cell[row][col];

			if ((cell->flags & TC_TRAIL) == 0 && cell->ch != ' ')
				count++;
		}
	}
	return count;
}

static bool terminal_viewport_has_text(const struct terminal *term, int top)
{
	for (int row = 0; row < g_rows; row++) {
		if (terminal_row_has_text(term, top + row))
			return true;
	}
	return false;
}

static int terminal_effective_viewport_top(const struct terminal *term)
{
	int top = term->viewport_top;
	int max_top = term->rows - g_rows;

	if (max_top < 0)
		max_top = 0;
	if (top < 0)
		top = 0;
	if (top > max_top)
		top = max_top;
	if (terminal_viewport_has_text(term, top))
		return top;
	for (int row = 0; row < term->rows; row++) {
		if (!terminal_row_has_text(term, row))
			continue;
		top = row;
		if (top > max_top)
			top = max_top;
		return top;
	}
	return term->viewport_top;
}

static int terminal_virtual_view_top(const struct terminal *term)
{
	int total_rows = term->scrollback_count + term->rows;
	int max_top = total_rows - g_rows;
	int live_top;
	int top;

	if (max_top < 0)
		max_top = 0;
	live_top = term->scrollback_count + terminal_effective_viewport_top(term);
	if (live_top > max_top)
		live_top = max_top;
	top = live_top - term->scrollback_offset;
	if (top < 0)
		top = 0;
	if (top > max_top)
		top = max_top;
	return top;
}

static void render_terminal(struct canvas *c, const struct terminal *term,
			    struct font_ctx *font,
			    const struct kitty_graphics *kitty)
{
	char time_buf[16];
	char session_buf[32];
	int status_y = UI_H - TERM_STATUS_H;
	int view_top = terminal_virtual_view_top(term);
	int cursor_row = term->scrollback_count + term->row;
	int x = 0;
	time_t now = time(NULL);
	struct tm tm_now;

	canvas_clear(c, TERM_DEFAULT_BG);
	for (int row = 0; row < g_rows; row++) {
		int src_row = view_top + row;
		const struct term_cell *cells = terminal_virtual_row(term, src_row);

		if (!cells)
			break;
		for (int col = 0; col < g_cols; col++) {
			draw_terminal_cell(c, font, &cells[col],
					   TERM_PAD_X + col * g_cell_w,
					   TERM_PAD_Y + row * g_cell_h);
		}
	}
	kitty_graphics_render(kitty, c->w, c->h, c->px);
	if ((now & 1) == 0 &&
	    cursor_row >= view_top &&
	    cursor_row < view_top + g_rows) {
		int cx = TERM_PAD_X + term->col * g_cell_w;
		int cy = TERM_PAD_Y + (cursor_row - view_top) * g_cell_h;

		fill_rect(c, cx, cy + g_cell_h - 3, g_cell_w, 2,
			  0x9cdcfe);
	}

	fill_rect(c, 0, status_y, UI_W, TERM_STATUS_H, TERM_STATUS_BG);
	draw_powerline_segment(c, &x, status_y, TERM_STATUS_H,
			       TERM_STATUS_BLUE, TERM_STATUS_GREEN, " AIKB ");
	draw_powerline_segment(c, &x, status_y, TERM_STATUS_H,
			       TERM_STATUS_GREEN, TERM_STATUS_AMBER, " VT100 ");
	terminal_status_segment(session_buf, sizeof(session_buf));
	draw_powerline_segment(c, &x, status_y, TERM_STATUS_H,
			       TERM_STATUS_AMBER, TERM_STATUS_BG, session_buf);
	if (term->scrollback_offset > 0) {
		char scroll_buf[24];

		snprintf(scroll_buf, sizeof(scroll_buf), " -%d ",
			 term->scrollback_offset);
		draw_powerline_segment(c, &x, status_y, TERM_STATUS_H,
				       TERM_STATUS_AMBER, TERM_STATUS_BG,
				       scroll_buf);
	}
	localtime_r(&now, &tm_now);
	snprintf(time_buf, sizeof(time_buf), "%02d:%02d", tm_now.tm_hour,
		 tm_now.tm_min);
	draw_text_right(c, UI_W - 12, status_y + 7, time_buf, 1, 0xd8dee9);
	if (font && font->ready)
		draw_text_fit(c, x + 8, status_y + 7, 360, font->path, 1,
			      0x8aa1b1);
}

static bool parse_pet_mood(const char *s, enum pet_mood *out)
{
	if (streq_ci(s, "IDLE")) {
		*out = PET_IDLE;
		return true;
	}
	if (streq_ci(s, "ASKING")) {
		*out = PET_ASKING;
		return true;
	}
	if (streq_ci(s, "CODING")) {
		*out = PET_CODING;
		return true;
	}
	if (streq_ci(s, "REVIEWING")) {
		*out = PET_REVIEWING;
		return true;
	}
	if (streq_ci(s, "ERROR")) {
		*out = PET_ERROR;
		return true;
	}
	if (streq_ci(s, "SLEEP")) {
		*out = PET_SLEEP;
		return true;
	}
	return false;
}

static bool parse_pet_scene(const char *s, enum pet_scene *out)
{
	if (streq_ci(s, "ASKING")) {
		*out = PET_SCENE_ASKING;
		return true;
	}
	if (streq_ci(s, "UPDATING")) {
		*out = PET_SCENE_UPDATING;
		return true;
	}
	if (streq_ci(s, "LISTENING") || streq_ci(s, "LISTEN") ||
	    streq_ci(s, "VOICE")) {
		*out = PET_SCENE_LISTENING;
		return true;
	}
	if (streq_ci(s, "FAULT") || streq_ci(s, "ERROR")) {
		*out = PET_SCENE_FAULT;
		return true;
	}
	if (streq_ci(s, "STANDBY")) {
		*out = PET_SCENE_STANDBY;
		return true;
	}
	return false;
}

static bool parse_pet_pose(const char *s, enum pet_pose *out)
{
	if (streq_ci(s, "IDLE")) {
		*out = PET_POSE_IDLE;
		return true;
	}
	if (streq_ci(s, "THINKING") || streq_ci(s, "THINK")) {
		*out = PET_POSE_THINKING;
		return true;
	}
	if (streq_ci(s, "HAPPY")) {
		*out = PET_POSE_HAPPY;
		return true;
	}
	if (streq_ci(s, "CONFUSED")) {
		*out = PET_POSE_CONFUSED;
		return true;
	}
	if (streq_ci(s, "SLEEPY") || streq_ci(s, "CALM")) {
		*out = PET_POSE_SLEEPY;
		return true;
	}
	return false;
}

static enum pet_emotion pet_emotion_from_pose(enum pet_pose pose)
{
	switch (pose) {
	case PET_POSE_THINKING:
		return PET_EMOTION_CURIOUS;
	case PET_POSE_HAPPY:
		return PET_EMOTION_HAPPY;
	case PET_POSE_CONFUSED:
		return PET_EMOTION_CONFUSED;
	case PET_POSE_SLEEPY:
		return PET_EMOTION_SLEEPY;
	case PET_POSE_IDLE:
	default:
		return PET_EMOTION_CALM;
	}
}

static void pet_set_pose(struct pet_state *p, enum pet_pose pose)
{
	p->anim.pose = pose;
	p->anim.emotion = pet_emotion_from_pose(pose);
	p->anim.pose_started_ms = monotonic_now_ms();
}

static enum pet_scene pet_scene_from_mood(enum pet_mood mood)
{
	switch (mood) {
	case PET_CODING:
	case PET_REVIEWING:
		return PET_SCENE_UPDATING;
	case PET_ERROR:
		return PET_SCENE_FAULT;
	case PET_SLEEP:
		return PET_SCENE_STANDBY;
	case PET_IDLE:
	case PET_ASKING:
	default:
		break;
	}
	return PET_SCENE_ASKING;
}

static enum pet_mood pet_mood_from_scene(enum pet_scene scene)
{
	switch (scene) {
	case PET_SCENE_UPDATING:
		return PET_CODING;
	case PET_SCENE_LISTENING:
		return PET_IDLE;
	case PET_SCENE_FAULT:
		return PET_ERROR;
	case PET_SCENE_STANDBY:
		return PET_SLEEP;
	case PET_SCENE_ASKING:
	default:
		return PET_ASKING;
	}
}

static enum pet_pose pet_pose_from_scene(enum pet_scene scene)
{
	switch (scene) {
	case PET_SCENE_LISTENING:
	case PET_SCENE_UPDATING:
		return PET_POSE_THINKING;
	case PET_SCENE_FAULT:
		return PET_POSE_CONFUSED;
	case PET_SCENE_STANDBY:
		return PET_POSE_SLEEPY;
	case PET_SCENE_ASKING:
	default:
		return PET_POSE_THINKING;
	}
}

static const char *pet_scene_default_message(enum pet_scene scene)
{
	switch (scene) {
	case PET_SCENE_UPDATING:
		return "agent task is running";
	case PET_SCENE_LISTENING:
		return "tiny dino is listening to your words";
	case PET_SCENE_FAULT:
		return "system fault detected";
	case PET_SCENE_STANDBY:
		return "all systems nominal";
	case PET_SCENE_ASKING:
	default:
		return "waiting for your confirmation";
	}
}

static void pet_set_message(struct pet_state *p, const char *msg)
{
	safe_copy(p->message, sizeof(p->message), msg);
}

static void pet_set_title(struct pet_state *p, const char *title)
{
	safe_copy(p->title, sizeof(p->title), title);
}

static void pet_set_scene(struct pet_state *p, enum pet_scene scene,
			  const char *msg)
{
	p->scene = scene;
	p->mood = pet_mood_from_scene(scene);
	pet_set_pose(p, pet_pose_from_scene(scene));
	pet_set_title(p, NULL);
	pet_set_message(p, msg ? msg : pet_scene_default_message(p->scene));
	p->last_interaction_ms = monotonic_now_ms();
}

static void pet_set_scene_title(struct pet_state *p, enum pet_scene scene,
				const char *title, const char *msg)
{
	pet_set_scene(p, scene, msg);
	pet_set_title(p, title);
}

static void pet_set_mood(struct pet_state *p, enum pet_mood mood,
			 const char *msg)
{
	p->mood = mood;
	p->scene = pet_scene_from_mood(mood);
	pet_set_pose(p, pet_pose_from_scene(p->scene));
	pet_set_title(p, NULL);
	pet_set_message(p, msg ? msg : pet_scene_default_message(p->scene));
}

static int pet_clamp_pct(int v)
{
	if (v < 0)
		return 0;
	if (v > 100)
		return 100;
	return v;
}

static void pet_init(struct pet_state *p)
{
	memset(p, 0, sizeof(*p));
	p->mood = PET_ASKING;
	p->scene = PET_SCENE_ASKING;
	p->anim.pose = PET_POSE_THINKING;
	p->anim.emotion = PET_EMOTION_CURIOUS;
	p->start_time_ms = monotonic_now_ms();
	p->last_interaction_ms = p->start_time_ms;
	p->anim.pose_started_ms = p->start_time_ms;
	p->energy = 72;
	p->affection = 54;
	p->focus = 66;
	p->progress = 58;
	pet_set_message(p, pet_scene_default_message(p->scene));
}

static void pet_touch(struct pet_state *p)
{
	p->last_interaction_ms = monotonic_now_ms();
	if (p->affection < 100)
		p->affection += 5;
	if (p->energy > 2)
		p->energy -= 2;
	p->energy = pet_clamp_pct(p->energy);
	p->affection = pet_clamp_pct(p->affection);
	pet_set_mood(p, PET_IDLE, "touch received");
}

static void pet_feed(struct pet_state *p)
{
	p->last_interaction_ms = monotonic_now_ms();
	if (p->energy < 100)
		p->energy += 10;
	if (p->affection < 100)
		p->affection += 2;
	p->energy = pet_clamp_pct(p->energy);
	p->affection = pet_clamp_pct(p->affection);
	pet_set_mood(p, PET_IDLE, "energy restored");
}

static void pet_cycle_mood(struct pet_state *p, int delta)
{
	(void)delta;
	pet_set_scene(p, PET_SCENE_ASKING, NULL);
}

static void pet_apply_command(struct pet_state *p, const char *line)
{
	enum pet_mood mood;
	enum pet_scene scene;
	char arg[32];
	const char *msg;
	int progress;

	if (strncmp(line, "PET ", 4) != 0)
		return;
	line += 4;
	while (*line == ' ')
		line++;

	if (sscanf(line, "MOOD %31s", arg) == 1) {
		if (parse_pet_mood(arg, &mood))
			pet_set_mood(p, mood, "mood updated from input");
		return;
	}
	if (sscanf(line, "SCENE %31s", arg) == 1) {
		if (parse_pet_scene(arg, &scene))
			pet_set_scene(p, scene, NULL);
		return;
	}
	if (sscanf(line, "POSE %31s", arg) == 1) {
		enum pet_pose pose;

		if (parse_pet_pose(arg, &pose))
			pet_set_pose(p, pose);
		return;
	}
	if (sscanf(line, "PROGRESS %d", &progress) == 1) {
		p->progress = pet_clamp_pct(progress);
		return;
	}
	if (strncmp(line, "MESSAGE ", 8) == 0) {
		msg = line + 8;
		while (*msg == ' ')
			msg++;
		pet_set_message(p, msg);
		return;
	}
	if (streq_ci(line, "TOUCH")) {
		pet_touch(p);
		return;
	}
	if (streq_ci(line, "FEED")) {
		pet_feed(p);
		return;
	}
	if (streq_ci(line, "WORK_START")) {
		if (p->focus < 100)
			p->focus += 8;
		p->focus = pet_clamp_pct(p->focus);
		pet_set_mood(p, PET_CODING, "work started");
		return;
	}
	if (streq_ci(line, "WORK_DONE")) {
		if (p->energy > 5)
			p->energy -= 5;
		p->energy = pet_clamp_pct(p->energy);
		pet_set_mood(p, PET_IDLE, "work done");
		return;
	}
	if (streq_ci(line, "TEST_FAIL")) {
		pet_set_mood(p, PET_ERROR, "test failed");
		return;
	}
}

static void pet_apply_event_line(struct pet_state *p, const char *line)
{
	if (streq_ci(line, "KEY 0 DOWN")) {
		pet_set_scene(p, PET_SCENE_FAULT, "rejected");
		return;
	}
	if (streq_ci(line, "KEY 1 DOWN")) {
		pet_set_scene(p, PET_SCENE_LISTENING, "speak now");
		return;
	}
	if (streq_ci(line, "KEY 2 DOWN")) {
		pet_cycle_mood(p, 1);
		return;
	}
	if (streq_ci(line, "ENC +1")) {
		pet_cycle_mood(p, 1);
		return;
	}
	if (streq_ci(line, "ENC -1")) {
		pet_cycle_mood(p, -1);
		return;
	}
	if (streq_ci(line, "ENC_BTN DOWN")) {
		if (p->affection < 100)
			p->affection += 4;
		p->affection = pet_clamp_pct(p->affection);
		pet_set_mood(p, PET_ASKING, "menu ready");
		p->last_interaction_ms = monotonic_now_ms();
	}
}

static void draw_pet_background(struct canvas *c, uint32_t frame)
{
	(void)frame;

	canvas_clear(c, 0x151514);
	fill_rect(c, 0, 0, UI_W, 56, 0x10100f);
	fill_rect(c, 0, 346, UI_W, UI_H - 346, 0x10100f);
	fill_rect(c, 0, 0, 4, UI_H, 0x0b0b0a);
	fill_rect(c, UI_W - 4, 0, 4, UI_H, 0x0b0b0a);
}

static void draw_pet_bullet(struct canvas *c, int x, int y, int size,
			    uint32_t color)
{
	fill_rect(c, x + 2, y, size - 4, size, color);
	fill_rect(c, x, y + 2, size, size - 4, color);
}

static int pet_font_text_w(struct font_ctx *font, const char *s,
			   int cell_w, int fallback_scale)
{
	return (int)strlen(s) * (font && font->ready && cell_w > 0 ? cell_w :
		VIDEO_FONT_WIDTH * fallback_scale);
}

static void draw_pet_font_text(struct canvas *c, struct font_ctx *font,
			       int x, int y, const char *s, int cell_w,
			       int fallback_scale, uint32_t color)
{
	for (; *s; s++) {
		if (font && font->ready &&
		    font_draw_cp(c, font, x, y, cell_w,
				 (unsigned char)*s, color, C_GRUVBOX_BG)) {
			x += cell_w;
			continue;
		}
		draw_char(c, x, y, (unsigned char)*s, fallback_scale, color);
		x += VIDEO_FONT_WIDTH * fallback_scale;
	}
}

static void draw_pet_font_center(struct canvas *c, struct font_ctx *font,
				 int cx, int y, const char *s, int cell_w,
				 int fallback_scale, uint32_t color)
{
	draw_pet_font_text(c, font,
			   cx - pet_font_text_w(font, s, cell_w,
						fallback_scale) / 2,
			   y, s, cell_w, fallback_scale, color);
}

static void draw_pet_icon_map(struct canvas *c, int x, int y,
			      const char *const *map, int rows,
			      bool mirror, uint32_t color)
{
	for (int row = 0; row < rows; row++) {
		int cols = (int)strlen(map[row]);

		for (int col = 0; col < cols; col++) {
			int sx = mirror ? cols - 1 - col : col;

			if (map[row][sx] == '.')
				continue;
			fill_rect(c, x + col * 2, y + row * 2, 2, 2, color);
		}
	}
}

static void draw_pet_rotate_icon(struct canvas *c, int x, int y, bool right,
				 uint32_t color)
{
	static const char *const map[] = {
		"...######...",
		"..##....##..",
		".##......##.",
		"##........#.",
		"##......####",
		"##.....#####",
		"##......####",
		"##........#.",
		".##......##.",
		"..##....##..",
		"...######...",
	};

	draw_pet_icon_map(c, x, y + 1, map, (int)(sizeof(map) / sizeof(map[0])),
			  !right, color);
}

static void draw_pet_down_icon(struct canvas *c, int x, int y, uint32_t color)
{
	static const char *const map[] = {
		".....###.....",
		"....#####....",
		"....#####....",
		"....#####....",
		"....#####....",
		"....#####....",
		"....#####....",
		"..#########..",
		"...#######...",
		"....#####....",
		".....###.....",
		"......#......",
	};

	draw_pet_icon_map(c, x, y, map, (int)(sizeof(map) / sizeof(map[0])),
			  false, color);
}

#define PET_DINO_W 72
#define PET_DINO_H 57
#define PET_STAGE_X 190
#define PET_STAGE_Y 64
#define PET_STAGE_W 580
#define PET_STAGE_H 192

static const char *const PET_DINO_MAP[PET_DINO_H] = {
	"........................................................................",
	"........................................................................",
	".........................................oodgggcccggcd..................",
	".....................................oodglhhhhhbbbbbbhlddd..............",
	"...................................odlhbhlllgggggggllbbbbbhgo...........",
	".................................dghlccggggggggggggggllllhbbhl..........",
	"................................obhlgggggggggggggggggggggllhbbg.........",
	"...............................gcggggglggggggggggggggggggggggbbd........",
	"............................dddgcggggdoodggggggggggggggggggoolbho.......",
	".........................oooodggggggdodggggggggggggggggggggoolhyg.......",
	".........................cbd.gggggggdggllgggggggggggggggggggggghbo......",
	".........................ogooggggggggggdodgggggggggggggggggggggcho......",
	"......................ogcgodggggggggggodhoogggggggggggggggggggglho......",
	".....................dbbcg.dgggggggggg.gho.dggggggggggggggggggggg.......",
	"......................ocgo.gggggggggggo....dggggggggggggggggggggg.......",
	"........................o.oggggggggggggd..dggggggggggggggddooooo........",
	"........................doogggggggggccccggggggggggggggdoodddggco........",
	"......................occoogggggggglhhhgggggggggggggd.odglggggg.........",
	".....................gbbcoogggggggglhccggggggggggdoooggggdddddo.........",
	".....................dccco.ddggggggggggggggggggggddggggggolhd...........",
	"........................do.odggggggggggggggggggggggggggggoglhlo.........",
	".........................oooddgggggggggggggggggggggggggdd..dlbhd........",
	"........................ocg.oddgggggggggggggggggggggggdo.ogdgcbho.......",
	".......................cbcgd.ddggggggggggggggggggdgddo..ogggggglbo......",
	".......................gcccdoooddggggggggggggggcggccgoo.oggggggcbho.....",
	"...........................odo.ddddddgggggdgggchhhcccgd..odggggglbg.....",
	"............................doo.oddddgggggggllhbbbhhhcc.ooogggggghbo....",
	"..........................dgbdooodddggggggggchhbbbbbhhhoodooggggglho....",
	".........................cbbco.dddggggggggggcbbbbbbbbbbgoddodggggcl.....",
	".........................odcgooddgggghhgglggcbbbbbbbbbbhooddggggggg.....",
	"...........................od.doogggghbhgdgglbbbbbbbbbbbloddggggggo.....",
	".......................ddgdo.od.ggggggghhgodlbbbbbbbbbbbhoodgggggo......",
	"......................cbbcgo.dgoggggggggchhdgbbbbbbbbbbbboodddgdo.......",
	"......................dccgd.dggooggggggggcbgdbbbbbbbbbbbboodoooo........",
	".......................ocd.odggd.odgggggggldgbbbbbbbbbbbbd..............",
	"...................ocgdoooodggggd.oggggggdoghbbbbbbbbbbbbo..............",
	"...................obbbd.odggggggd.oodddgodhbbbbbbbbbbbbbo..............",
	"....................dccd.ggggggddddoo.oodhbbbbbbbbbbbbbbbooo............",
	"............oo...oc.oddodggggggggggggodchbbbbbbbbbbbbbbhbooll...........",
	"............cho..dcg..odgggggdggggghblogbbbbbbbbbbbbbbbhhoobbd..........",
	"............dgco..oo.dggggggodggggglchlocbbbbbbbbbbbbbbccodclhg.........",
	"............odggddodgggggggdogggggggllhgohbbbbbbbbbbbbhcd.dgcbl.........",
	".............dggglggggggggdo.ggggggggcbhocbbbbbbbbbbbhco.dggglho........",
	"..............dggggggggggddo.dgggggggchloccbbbbbbbbhhccooddgglco........",
	"...............oggddgggdddgo.dggggggggccogchbhhhhbhccgoddddggggo........",
	"................odggcccggccco.dgggggggggocccccccccccd.oddddgggd.........",
	"o..................ogccccccccoodgggggggdoccccccccdgoodddddddddo..........",
	"do....................odgdddddoddgggggd..odddgddo...odddddddd...........",
	"dcd..........................dgddddddo..............oodddddo.............",
	"ogo.oco......................gggdooo................oddddddooo...........",
	"ocdoddo......................dgggddggdo.............odddgggclcld.........",
	"odogd.......................ogggggccglhgo...........odddgghblgchgo.o....",
	"ddddddddddddggdddgdooodooooooddgggbbdcbcbd.ooooooooooddddgghbddbbdoddddd",
	"ooooooodooddddddddoooddoooooooddddccogcchd..oooooooooooooddgcoogcooooooo",
	"........................................................................",
	"........................................................................",
	"........................................................................",
};

static const char *const PET_AKIM_PATHS[PET_SCENE_COUNT] = {
	[PET_SCENE_ASKING] = PET_ASKING_AKIM_PATH,
	[PET_SCENE_UPDATING] = PET_UPDATING_AKIM_PATH,
	[PET_SCENE_LISTENING] = PET_LISTENING_AKIM_PATH,
	[PET_SCENE_FAULT] = PET_FAULT_AKIM_PATH,
	[PET_SCENE_STANDBY] = PET_STANDBY_AKIM_PATH,
};

struct pet_scene_asset_manifest {
	enum pet_scene scene;
	const char *state;
	const char *resource_path;
	uint32_t expected_frames;
	uint32_t frame_duration_ms;
};

struct pet_pose_range {
	const char *state;
	uint32_t first;
	uint32_t count;
};

struct pet_character {
	const char *name;
	const struct pet_pose_range *pose_ranges;
	int pose_count;
};

static const struct pet_pose_range PET_CORE_POSE_RANGES[PET_POSE_COUNT] = {
	[PET_POSE_IDLE] = { "idle", 0, 8 },
	[PET_POSE_THINKING] = { "thinking", 8, 6 },
	[PET_POSE_HAPPY] = { "happy", 14, 6 },
	[PET_POSE_CONFUSED] = { "asking", 20, 6 },
	[PET_POSE_SLEEPY] = { "sleep", 26, 6 },
};

static const struct pet_scene_asset_manifest PET_ASSET_MANIFEST[PET_SCENE_COUNT] = {
	[PET_SCENE_ASKING] = {
		.scene = PET_SCENE_ASKING,
		.state = "asking",
		.resource_path = PET_ASKING_AKIM_PATH,
		.expected_frames = 32,
		.frame_duration_ms = 120,
	},
	[PET_SCENE_UPDATING] = {
		.scene = PET_SCENE_UPDATING,
		.state = "updating",
		.resource_path = PET_UPDATING_AKIM_PATH,
		.expected_frames = 0,
		.frame_duration_ms = 120,
	},
	[PET_SCENE_LISTENING] = {
		.scene = PET_SCENE_LISTENING,
		.state = "listening",
		.resource_path = PET_LISTENING_AKIM_PATH,
		.expected_frames = 0,
		.frame_duration_ms = 120,
	},
	[PET_SCENE_FAULT] = {
		.scene = PET_SCENE_FAULT,
		.state = "fault",
		.resource_path = PET_FAULT_AKIM_PATH,
		.expected_frames = 0,
		.frame_duration_ms = 120,
	},
	[PET_SCENE_STANDBY] = {
		.scene = PET_SCENE_STANDBY,
		.state = "standby",
		.resource_path = PET_STANDBY_AKIM_PATH,
		.expected_frames = 0,
		.frame_duration_ms = 120,
	},
};

static const struct pet_character PET_CORE_CHARACTER = {
	.name = "vibe-coding-companion",
	.pose_ranges = PET_CORE_POSE_RANGES,
	.pose_count = PET_POSE_COUNT,
};

static const struct pet_scene_asset_manifest *pet_asset_manifest(enum pet_scene scene)
{
	if (scene < 0 || scene >= PET_SCENE_COUNT)
		return NULL;
	return &PET_ASSET_MANIFEST[scene];
}

static const char *pet_scene_name(enum pet_scene scene);
static const char *pet_pose_name(enum pet_pose pose);

static const char *pet_asset_path(enum pet_scene scene)
{
	if (scene < 0 || scene >= PET_SCENE_COUNT)
		return NULL;
	if (g_pet_asset_paths[scene][0])
		return g_pet_asset_paths[scene];
	return PET_AKIM_PATHS[scene];
}

static void pet_set_asset_root(const char *root)
{
	size_t len;

	if (!root || !root[0])
		return;
	safe_copy(g_pet_asset_root, sizeof(g_pet_asset_root), root);
	len = strlen(g_pet_asset_root);
	while (len > 1 && g_pet_asset_root[len - 1] == '/') {
		g_pet_asset_root[len - 1] = '\0';
		len--;
	}
	for (int i = 0; i < PET_SCENE_COUNT; i++) {
		const char *base = strrchr(PET_AKIM_PATHS[i], '/');
		size_t root_len = strlen(g_pet_asset_root);
		size_t base_len;
		char *dst = g_pet_asset_paths[i];

		base = base ? base + 1 : PET_AKIM_PATHS[i];
		base_len = strlen(base);
		if (root_len + 1u + base_len >= sizeof(g_pet_asset_paths[i])) {
			warnf("pet asset root too long, ignoring %s", base);
			dst[0] = '\0';
			continue;
		}
		memcpy(dst, g_pet_asset_root, root_len);
		dst[root_len] = '/';
		memcpy(dst + root_len + 1u, base, base_len + 1u);
	}
}

static uint32_t pet_dino_color(char ch)
{
	switch (ch) {
	case 'o':
		return 0x2b2519;
	case 'd':
		return 0x414222;
	case 'g':
		return 0x5d5a2f;
	case 'l':
		return 0x7e733d;
	case 'h':
		return 0xb1904c;
	case 'b':
		return 0xd7973e;
	case 'y':
		return C_GRUVBOX_YELLOW;
	case 'c':
		return 0xd65d0e;
	case 'w':
		return C_GRUVBOX_TEXT;
	default:
		return 0;
	}
}

static void draw_pet_dino_fallback(struct canvas *c, int ox, int oy, int scale)
{
	for (int y = 0; y < PET_DINO_H; y++) {
		const char *row = PET_DINO_MAP[y];

		for (int x = 0; x < PET_DINO_W; x++) {
			uint32_t color = pet_dino_color(row[x]);

			if (!color)
				continue;
			fill_rect(c, ox + x * scale, oy + y * scale,
				  scale, scale, color);
		}
	}
}

static void draw_pet_akim_frame(struct canvas *c, const struct anim_state *a,
				bool argb8888, int box_x, int box_y,
				int box_w, int box_h)
{
	struct anim_header hdr;
	const uint8_t *frame;
	size_t frame_bytes;
	int scale;
	int dst_w;
	int dst_h;
	int dx;
	int dy;

	if (!a->active || !a->base)
		return;
	memcpy(&hdr, a->base, sizeof(hdr));
	if (!hdr.width || !hdr.height)
		return;
	frame_bytes = (size_t)hdr.width * hdr.height * 4u;
	frame = a->base + sizeof(struct anim_header) +
		(size_t)a->frame_idx * frame_bytes;

	/* If the sprite is bigger than the box (e.g. 320x170 standby sprite
	 * placed into a 160x90 dashboard slot), shrink with nearest sampling
	 * to fit while preserving aspect. Aspect-fit chooses the dimension
	 * that hits the box edge first. */
	if ((int)hdr.width > box_w || (int)hdr.height > box_h) {
		int fit_w = box_w;
		int fit_h = (int)hdr.height * box_w / (int)hdr.width;
		if (fit_h > box_h) {
			fit_h = box_h;
			fit_w = (int)hdr.width * box_h / (int)hdr.height;
		}
		int sdx = box_x + (box_w - fit_w) / 2;
		int sdy = box_y + (box_h - fit_h) / 2;
		for (int yi = 0; yi < fit_h; yi++) {
			int sy = yi * (int)hdr.height / fit_h;
			for (int xi = 0; xi < fit_w; xi++) {
				int sx = xi * (int)hdr.width / fit_w;
				const uint8_t *p = frame + ((size_t)sy * hdr.width + sx) * 4u;
				uint8_t r, g, b, alpha;
				if (argb8888) {
					b = p[0]; g = p[1]; r = p[2]; alpha = p[3];
				} else {
					r = p[0]; g = p[1]; b = p[2]; alpha = p[3];
				}
				if (!alpha)
					continue;
				int px_x = sdx + xi;
				int px_y = sdy + yi;
				if ((unsigned)px_x >= (unsigned)c->w ||
				    (unsigned)px_y >= (unsigned)c->h)
					continue;
				uint32_t src = ((uint32_t)r << 16) |
					       ((uint32_t)g << 8) | b;
				if (alpha == 255)
					c->px[px_y * c->w + px_x] = src;
				else
					c->px[px_y * c->w + px_x] =
						blend_rgb(c->px[px_y * c->w + px_x],
							  src, alpha);
			}
		}
		return;
	}

	scale = box_w / (int)hdr.width;
	if (box_h / (int)hdr.height < scale)
		scale = box_h / (int)hdr.height;
	if (scale < 1)
		scale = 1;
	dst_w = (int)hdr.width * scale;
	dst_h = (int)hdr.height * scale;
	dx = box_x + (box_w - dst_w) / 2;
	dy = box_y + (box_h - dst_h) / 2;

	for (uint32_t y = 0; y < hdr.height; y++) {
		for (uint32_t x = 0; x < hdr.width; x++) {
			const uint8_t *px = frame + ((size_t)y * hdr.width + x) * 4u;
			uint8_t r;
			uint8_t g;
			uint8_t b;
			uint8_t alpha;
			int tx = dx + (int)x * scale;
			int ty = dy + (int)y * scale;

			if (argb8888) {
				b = px[0];
				g = px[1];
				r = px[2];
				alpha = px[3];
			} else {
				r = px[0];
				g = px[1];
				b = px[2];
				alpha = px[3];
			}
			if (!alpha)
				continue;
			if (alpha == 255) {
				fill_rect(c, tx, ty, scale, scale,
					  ((uint32_t)r << 16) |
					  ((uint32_t)g << 8) | b);
			} else {
				for (int yy = 0; yy < scale; yy++) {
					for (int xx = 0; xx < scale; xx++) {
						int px_x = tx + xx;
						int px_y = ty + yy;
						uint32_t src;

						if ((unsigned)px_x >=
						    (unsigned)c->w ||
						    (unsigned)px_y >=
						    (unsigned)c->h)
							continue;
						src = ((uint32_t)r << 16) |
						      ((uint32_t)g << 8) | b;
						c->px[px_y * c->w + px_x] =
							blend_rgb(c->px[px_y * c->w + px_x],
								  src, alpha);
					}
				}
			}
		}
	}
}

static bool draw_pet_scene_akim(struct canvas *c, enum pet_scene scene,
				uint32_t frame, int box_x, int box_y,
				int box_w, int box_h)
{
	struct anim_state *a;
	uint32_t step;

	if (scene < 0 || scene >= PET_SCENE_COUNT)
		return false;
	a = &g_pet_anims[scene];
	if (!a->active || !a->frame_count || !a->frame_delay_ms)
		return false;
	step = (uint32_t)(((uint64_t)frame * 83u) / a->frame_delay_ms);
	a->frame_idx = step % a->frame_count;
	draw_pet_akim_frame(c, a, g_pet_anim_argb8888[scene], box_x, box_y,
			    box_w, box_h);
	return true;
}

static bool pet_asking_akim_is_single_pose(const struct anim_state *a)
{
	const struct pet_scene_asset_manifest *manifest =
		&PET_ASSET_MANIFEST[PET_SCENE_ASKING];

	return a && a->active && a->frame_count &&
		manifest->expected_frames &&
		a->frame_count != manifest->expected_frames;
}

static bool pet_render_character_akim(struct canvas *c,
				      const struct pet_state *p,
				      uint32_t frame, int box_x, int box_y,
				      int box_w, int box_h)
{
	const struct pet_pose_range *range;
	struct anim_state *a = &g_pet_anims[PET_SCENE_ASKING];
	uint32_t first;
	uint32_t count;
	uint32_t local;

	if (!a->active || !a->frame_count || !a->frame_delay_ms)
		return false;
	if (p->anim.pose >= PET_POSE_COUNT)
		return false;
	range = &PET_CORE_CHARACTER.pose_ranges[p->anim.pose];
	first = range->first;
	count = range->count;
	if (p->anim.pose == PET_POSE_CONFUSED &&
	    pet_asking_akim_is_single_pose(a)) {
		first = 0;
		count = a->frame_count;
	}
	if (!count || first >= a->frame_count)
		return false;
	local = (uint32_t)(((uint64_t)frame * 83u) / a->frame_delay_ms);
	a->frame_idx = first + (local % count);
	if (a->frame_idx >= a->frame_count)
		a->frame_idx = first;
	draw_pet_akim_frame(c, a, g_pet_anim_argb8888[PET_SCENE_ASKING],
			    box_x, box_y, box_w, box_h);
	return true;
}

static bool pet_pose_wants_question(enum pet_pose pose)
{
	return pose == PET_POSE_THINKING || pose == PET_POSE_CONFUSED;
}

static void draw_pet_question_mark(struct canvas *c, uint32_t frame)
{
	int bob = (int)((frame / 8u) & 1u);

	draw_text(c, 595, 106 + bob, "?", 8, C_GRUVBOX_YELLOW);
}

static void pet_update_animation(struct pet_state *p, uint64_t now_ms)
{
	if (p->scene == PET_SCENE_ASKING) {
		uint64_t age = now_ms - p->anim.pose_started_ms;

		if (p->anim.pose == PET_POSE_THINKING && age > 12000u)
			pet_set_pose(p, PET_POSE_IDLE);
	}
}

static void pet_render_character(struct canvas *c, const struct pet_state *p,
				 uint32_t frame, int box_x, int box_y,
				 int box_w, int box_h)
{
	if (!g_pet_force_fallback &&
	    pet_render_character_akim(c, p, frame, box_x, box_y, box_w, box_h)) {
		if (pet_pose_wants_question(p->anim.pose) &&
		    !pet_asking_akim_is_single_pose(
			    &g_pet_anims[PET_SCENE_ASKING]))
			draw_pet_question_mark(c, frame);
		return;
	}

	switch (p->anim.pose) {
	case PET_POSE_HAPPY:
		draw_pet_dino_fallback(c, 318, 92, 3);
		draw_text(c, 620, 112, "!", 7, C_GRUVBOX_YELLOW);
		break;
	case PET_POSE_CONFUSED:
		draw_pet_dino_fallback(c, 318, 92, 3);
		draw_pet_question_mark(c, frame);
		break;
	case PET_POSE_SLEEPY:
		draw_pet_dino_fallback(c, 330, 96, 2);
		draw_text(c, 600, 104, "z", 3, C_GRUVBOX_MUTED);
		break;
	case PET_POSE_THINKING:
	case PET_POSE_IDLE:
	default:
		draw_pet_dino_fallback(c, 318, 88, 3);
		if (pet_pose_wants_question(p->anim.pose))
			draw_pet_question_mark(c, frame);
		break;
	}
}

static const char *pet_header_left(enum pet_scene scene)
{
	switch (scene) {
	case PET_SCENE_UPDATING:
		return "RUN";
	case PET_SCENE_LISTENING:
		return "VOICE";
	case PET_SCENE_FAULT:
		return "FAULT";
	case PET_SCENE_STANDBY:
		return "STANDBY";
	case PET_SCENE_ASKING:
	default:
		return "ASKING";
	}
}

static const char *pet_header_state(enum pet_scene scene)
{
	switch (scene) {
	case PET_SCENE_UPDATING:
		return "RUN";
	case PET_SCENE_LISTENING:
		return "VOICE";
	case PET_SCENE_STANDBY:
		return "STANDBY";
	case PET_SCENE_ASKING:
	default:
		return "ASKING";
	}
}

static const char *pet_scene_title(enum pet_scene scene)
{
	switch (scene) {
	case PET_SCENE_UPDATING:
		return "[ RUNNING ]";
	case PET_SCENE_LISTENING:
		return "[ VOICE INPUT ]";
	case PET_SCENE_FAULT:
		return "[ OVERHEAT ]";
	case PET_SCENE_STANDBY:
		return "[ NO ACTIVE SESSION ]";
	case PET_SCENE_ASKING:
	default:
		return "[  ASKING  ]";
	}
}

static int pet_scene_title_scale(enum pet_scene scene)
{
	(void)scene;
	return 3;
}

static const char *pet_footer_hint(enum pet_scene scene)
{
	switch (scene) {
	case PET_SCENE_UPDATING:
		return "agent task is running";
	case PET_SCENE_LISTENING:
		return "speak now";
	case PET_SCENE_FAULT:
		return "press to confirm";
	default:
		return "press any key";
	}
}

static void draw_pet_scene_visual(struct canvas *c, const struct pet_state *p,
				  uint32_t frame)
{
	int box_x = PET_STAGE_X;
	int box_y = PET_STAGE_Y;
	int box_w = PET_STAGE_W;
	int box_h = PET_STAGE_H;

	if (p->scene != PET_SCENE_ASKING &&
	    !g_pet_force_fallback &&
	    draw_pet_scene_akim(c, p->scene, frame, box_x, box_y, box_w, box_h))
		return;
	pet_render_character(c, p, frame, box_x, box_y, box_w, box_h);
}

static void draw_pet_header(struct canvas *c, struct font_ctx *font,
			    const struct pet_state *p, int seconds)
{
	enum pet_scene scene = p->scene;
	char time_buf[16];
	const char *left = p->title[0] ? p->title : pet_header_left(scene);

	draw_pet_font_text(c, font, 22, 16, left, 12, 2, C_GRUVBOX_YELLOW);
	snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d", seconds / 3600,
		 (seconds / 60) % 60, seconds % 60);

	if (scene == PET_SCENE_FAULT) {
		draw_pet_font_text(c, font, 570, 16, time_buf, 12, 2,
				   C_GRUVBOX_YELLOW);
		draw_pet_bullet(c, 718, 29, 9, C_GRUVBOX_YELLOW);
		draw_pet_font_text(c, font, 744, 16, "1 alert", 12, 2,
				   C_GRUVBOX_RED);
		draw_pet_bullet(c, 872, 29, 9, C_GRUVBOX_RED);
		draw_pet_font_text(c, font, 898, 16, "fault", 12, 2,
				   C_GRUVBOX_RED);
	} else {
		draw_pet_font_text(c, font, 548, 16, pet_header_state(scene),
				   12, 2, C_GRUVBOX_YELLOW);
		draw_pet_bullet(c, 656, 29, 9, C_GRUVBOX_YELLOW);
		draw_pet_font_text(c, font, 680, 16, time_buf, 12, 2,
				   C_GRUVBOX_YELLOW);
		draw_pet_bullet(c, 822, 29, 9, C_GRUVBOX_YELLOW);
		draw_pet_font_text(c, font, 852, 16, "23 C", 12, 2,
				   C_GRUVBOX_BLUE);
	}

	hline(c, 22, 54, UI_W - 44, C_GRUVBOX_LINE);
	hline(c, 22, 55, UI_W - 44, C_GRUVBOX_DARK1);
}

static void draw_pet_footer(struct canvas *c, struct font_ctx *font,
			    enum pet_scene scene)
{
	const char *hint = pet_footer_hint(scene);

	hline(c, 22, 344, UI_W - 44, C_GRUVBOX_LINE);
	hline(c, 22, 345, UI_W - 44, C_GRUVBOX_DARK1);
	draw_pet_font_text(c, font, 58, 360, "ROTATE VIEW", 12, 2,
			   C_GRUVBOX_TEXT);
	draw_pet_font_text(c, font, 326, 360, "|", 12, 2,
			   C_GRUVBOX_MUTED);
	draw_pet_font_center(c, font, UI_W / 2, 360, hint, 12, 2,
			     C_GRUVBOX_TEXT);
	draw_pet_font_text(c, font, 690, 360, "|", 12, 2,
			   C_GRUVBOX_MUTED);
	draw_pet_font_text(c, font, 798, 360, "MENU", 12, 2,
			    C_GRUVBOX_TEXT);
}

static void render_pet(struct canvas *c, struct font_ctx *font,
		       struct pet_state *p)
{
	uint64_t now_ms = monotonic_now_ms();
	uint32_t elapsed = (uint32_t)(now_ms - p->start_time_ms);
	uint32_t frame = elapsed / 83u;
	int seconds = (int)(elapsed / 1000u);

	p->frame_index = frame;
	pet_update_animation(p, now_ms);

	draw_pet_background(c, frame);
	draw_pet_header(c, font, p, seconds);
	draw_pet_scene_visual(c, p, frame);

	hline(c, 326, 258, 308, C_DIM);
	hline(c, 356, 254, 248, C_LINE2);
	draw_pet_font_center(c, font, UI_W / 2, 280, pet_scene_title(p->scene),
			     16, pet_scene_title_scale(p->scene),
			     p->scene == PET_SCENE_FAULT ?
			     C_GRUVBOX_RED : C_GRUVBOX_YELLOW);

	draw_pet_footer(c, font, p->scene);
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
	safe_copy(m->window, sizeof(m->window), "host data pending");
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

/* Pet sprite overlay: always painted last (after the active view) so the
 * mascot lives on its own layer and stays in the bottom-left corner of
 * every screen — dashboard, picker and terminal. The UI underneath keeps
 * its own original layout (centred, full-width) and is allowed to be
 * partially occluded; the pet box is sized to be visible without forcing
 * the UI to redesign around it. VIEW_PET paints its own bigger pet, so
 * render_frame skips this overlay there. */
static void render_pet_overlay(struct canvas *c, const struct pet_state *p)
{
	uint64_t now_ms = monotonic_now_ms();
	uint32_t frame = (uint32_t)((now_ms - p->start_time_ms) / 83u);
	(void)draw_pet_scene_akim(c, p->scene, frame,
				  16, UI_H - 220, 320, 204);
}

static void render_dashboard(struct canvas *c, const struct ui_model *m,
			     struct font_ctx *font,
			     const struct pet_state *p)
{
	int focus = m->focus;
	uint64_t now_ms = monotonic_now_ms();
	int seconds = (int)((now_ms - p->start_time_ms) / 1000u);

	/* Solid black canvas + video-style header / footer (same look the
	 * source mp4s use): persistent title, status+clock+temp, and
	 * minimal footer actions. */
	canvas_clear(c, C_GRUVBOX_BG);
	fill_rect(c, 0, 0, UI_W, UI_H, 0x141312);
	draw_pet_header(c, font, p, seconds);
	draw_pet_footer(c, font, p->scene);

	/* Body band lives between the header and footer rules (~y=60..340). */
	if (m->count <= 0) {
		const char *msg = "WAITING FOR HOST DATA";
		int mw = text_w(msg, 2);
		draw_text(c, (UI_W - mw) / 2, 188, msg, 2, C_GRUVBOX_MUTED);
		return;
	}

	if (focus < 0 || focus >= m->count)
		focus = 0;

	/* Three-row session view: focus-1 above, focus highlighted, focus+1
	 * below. Same draw_session_row helper as before — keeps the column
	 * spacing the original layout designed. */
	if (focus > 0)
		draw_session_row(c, &m->sessions[focus - 1], 100, false);
	draw_session_row(c, &m->sessions[focus], 168, true);
	if (focus + 1 < m->count)
		draw_session_row(c, &m->sessions[focus + 1], 252, false);
}

/* ---------------------------------------------------------------- board session table */

static const char *board_session_state_str(enum board_session_state s)
{
	switch (s) {
	case BSS_CONNECTED:    return "CONNECTED";
	case BSS_DISCONNECTED: return "DISCONNECTED";
	case BSS_RUN:          return "RUN";
	case BSS_WAIT:         return "WAIT";
	case BSS_DONE:         return "DONE";
	case BSS_ERROR:        return "ERROR";
	default:               return "?";
	}
}

static int board_session_find_idx(uint16_t sid)
{
	for (int i = 0; i < MAX_BOARD_SESSIONS; i++) {
		if (g_board_sessions[i].used && g_board_sessions[i].sid == sid)
			return i;
	}
	return -1;
}

static int board_session_alloc_idx(void)
{
	for (int i = 0; i < MAX_BOARD_SESSIONS; i++) {
		if (!g_board_sessions[i].used)
			return i;
	}
	return -1;
}

static int board_session_count_live(void)
{
	int n = 0;
	for (int i = 0; i < MAX_BOARD_SESSIONS; i++) {
		if (g_board_sessions[i].used)
			n++;
	}
	return n;
}

static bool board_session_state_from_name(const char *name,
					  enum board_session_state *out)
{
	if (streq_ci(name, "connected"))    { *out = BSS_CONNECTED; return true; }
	if (streq_ci(name, "disconnected")) { *out = BSS_DISCONNECTED; return true; }
	if (streq_ci(name, "run"))          { *out = BSS_RUN; return true; }
	if (streq_ci(name, "wait"))         { *out = BSS_WAIT; return true; }
	if (streq_ci(name, "done"))         { *out = BSS_DONE; return true; }
	if (streq_ci(name, "error"))        { *out = BSS_ERROR; return true; }
	return false;
}

static void board_session_upsert(uint16_t sid, enum board_session_state state)
{
	int idx = board_session_find_idx(sid);
	bool is_new = false;

	if (idx < 0) {
		idx = board_session_alloc_idx();
		is_new = true;
	}
	if (idx < 0)
		return;
	g_board_sessions[idx].used = true;
	g_board_sessions[idx].sid = sid;
	g_board_sessions[idx].state = state;
	if (is_new)
		g_lcd_selected_sid = sid;
	if (state != BSS_WAIT)
		memset(&g_board_sessions[idx].perm, 0,
		       sizeof(g_board_sessions[idx].perm));
}

static void board_session_remove(uint16_t sid)
{
	int idx = board_session_find_idx(sid);
	if (idx < 0)
		return;
	memset(&g_board_sessions[idx], 0, sizeof(g_board_sessions[idx]));
	if (g_lcd_selected_sid == sid)
		g_lcd_selected_sid = 0;
	if (g_lcd_active_sid == sid)
		g_lcd_active_sid = 0;
}

static void board_session_update_token(uint16_t sid, uint64_t input,
				       uint64_t output, uint64_t cost_cents)
{
	int idx = board_session_find_idx(sid);
	if (idx < 0)
		return;
	g_board_sessions[idx].token.input = input;
	g_board_sessions[idx].token.output = output;
	g_board_sessions[idx].token.cost_cents = cost_cents;
}

static void board_session_update_turn(uint16_t sid, const char *role,
				      const char *text)
{
	int idx = board_session_find_idx(sid);
	if (idx < 0)
		return;
	snprintf(g_board_sessions[idx].turn.role,
		 sizeof(g_board_sessions[idx].turn.role), "%s", role ? role : "");
	snprintf(g_board_sessions[idx].turn.text,
		 sizeof(g_board_sessions[idx].turn.text), "%s", text ? text : "");
	g_board_sessions[idx].turn.updated_ms = monotonic_now_ms();
}

static void board_session_update_perm(uint16_t sid, uint64_t req_id,
				      const char *tool, const char *args)
{
	int idx = board_session_find_idx(sid);
	if (idx < 0)
		return;
	g_board_sessions[idx].perm.req_id = req_id;
	snprintf(g_board_sessions[idx].perm.tool,
		 sizeof(g_board_sessions[idx].perm.tool), "%s", tool ? tool : "");
	snprintf(g_board_sessions[idx].perm.args,
		 sizeof(g_board_sessions[idx].perm.args), "%s", args ? args : "");
	g_board_sessions[idx].perm.active = true;
}

static void board_session_clear_perm(uint16_t sid, uint64_t req_id)
{
	int idx = board_session_find_idx(sid);
	if (idx < 0)
		return;
	if (g_board_sessions[idx].perm.req_id != req_id)
		return;
	memset(&g_board_sessions[idx].perm, 0,
	       sizeof(g_board_sessions[idx].perm));
}

static void board_session_update_meta(uint16_t sid, const char *kind,
				      const char *cwd, const char *branch)
{
	int idx = board_session_find_idx(sid);
	if (idx < 0)
		return;
	snprintf(g_board_sessions[idx].meta.kind,
		 sizeof(g_board_sessions[idx].meta.kind), "%s", kind ? kind : "");
	snprintf(g_board_sessions[idx].meta.cwd,
		 sizeof(g_board_sessions[idx].meta.cwd), "%s", cwd ? cwd : "");
	snprintf(g_board_sessions[idx].meta.branch,
		 sizeof(g_board_sessions[idx].meta.branch), "%s",
		 branch ? branch : "");
}

static void board_session_update_hint(uint16_t sid, const char *hint)
{
	int idx = board_session_find_idx(sid);
	if (idx < 0)
		return;
	safe_copy(g_board_sessions[idx].meta.hint,
		  sizeof(g_board_sessions[idx].meta.hint), hint);
}

static const char *path_tail(const char *path)
{
	const char *slash;
	const char *backslash;

	if (!path || !path[0])
		return "";
	slash = strrchr(path, '/');
	backslash = strrchr(path, '\\');
	if (!slash || (backslash && backslash > slash))
		slash = backslash;
	return slash ? slash + 1 : path;
}

/* Return next live sid in iteration order, or 0 if there are none. */
static uint16_t board_session_pick_first(void)
{
	for (int i = 0; i < MAX_BOARD_SESSIONS; i++) {
		if (g_board_sessions[i].used)
			return g_board_sessions[i].sid;
	}
	return 0;
}

static uint16_t board_session_step(uint16_t cur, int delta)
{
	uint16_t sids[MAX_BOARD_SESSIONS];
	int n = 0;
	for (int i = 0; i < MAX_BOARD_SESSIONS; i++) {
		if (g_board_sessions[i].used)
			sids[n++] = g_board_sessions[i].sid;
	}
	if (n == 0)
		return 0;
	if (n == 1)
		return sids[0];
	/* Locate current in the ordered list; if not found, start at 0. */
	int idx = 0;
	for (int i = 0; i < n; i++) {
		if (sids[i] == cur) {
			idx = i;
			break;
		}
	}
	idx = (idx + delta) % n;
	if (idx < 0)
		idx += n;
	return sids[idx];
}

/* ---------------------------------------------------------------- ui-ctrl-out */

/* Open the FIFO non-blocking, lazily, creating it if missing. */
static int open_ui_ctrl_out(const char *path)
{
	struct stat st;
	int fd;

	if (!path)
		return -1;
	if (stat(path, &st) < 0 && errno == ENOENT) {
		if (mkfifo(path, 0600) != 0 && errno != EEXIST)
			return -1;
	}
	fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	return fd;
}

static void ui_ctrl_emit(const char *line)
{
	size_t len;
	size_t off = 0;

	if (!g_ui_ctrl_out_path || !line)
		return;
	len = strlen(line);
	if (g_ui_ctrl_out_fd < 0) {
		g_ui_ctrl_out_fd = open_ui_ctrl_out(g_ui_ctrl_out_path);
		if (g_ui_ctrl_out_fd < 0)
			return;
	}
	while (off < len) {
		ssize_t n = write(g_ui_ctrl_out_fd, line + off, len - off);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			close(g_ui_ctrl_out_fd);
			g_ui_ctrl_out_fd = -1;
			return;
		}
		if (n == 0)
			return;
		off += (size_t)n;
	}
}

static void ui_ctrl_emit_view(const char *view_name)
{
	char line[32];
	snprintf(line, sizeof(line), "view %s\n", view_name);
	ui_ctrl_emit(line);
}

static void ui_ctrl_emit_select(uint16_t sid)
{
	char line[32];
	snprintf(line, sizeof(line), "select %u\n", sid);
	ui_ctrl_emit(line);
}

static void ui_ctrl_emit_focus(uint16_t sid)
{
	char line[32];
	snprintf(line, sizeof(line), "focus %u\n", sid);
	ui_ctrl_emit(line);
}

static void ui_ctrl_emit_permission(uint16_t sid, uint64_t req_id,
				    const char *decision)
{
	char line[80];
	snprintf(line, sizeof(line), "permission %u reqid=%llu decision=%s\n",
		 sid, (unsigned long long)req_id, decision);
	ui_ctrl_emit(line);
}

/* Picker has its own chrome: top label "SESSION" + clock + live count, bottom
 * shows encoder/confirm/reject hints. Deliberately does not reuse
 * draw_pet_header so the pet-scene label (ASKING/UPDATING/...) cannot leak
 * into the picker view. */
static void draw_picker_header(struct canvas *c, struct font_ctx *font,
			       int seconds, int live_count)
{
	char time_buf[16];
	char count_buf[24];

	draw_pet_font_text(c, font, 22, 16, "SESSION", 12, 2, C_GRUVBOX_YELLOW);

	snprintf(count_buf, sizeof(count_buf), "%d live", live_count);
	draw_pet_font_text(c, font, 548, 16, count_buf, 12, 2,
			   live_count > 0 ? C_GRUVBOX_YELLOW : C_GRUVBOX_MUTED);
	draw_pet_bullet(c, 692, 29, 9, C_GRUVBOX_YELLOW);

	snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d",
		 seconds / 3600, (seconds / 60) % 60, seconds % 60);
	draw_pet_font_text(c, font, 716, 16, time_buf, 12, 2,
			   C_GRUVBOX_YELLOW);

	hline(c, 22, 54, UI_W - 44, C_GRUVBOX_LINE);
	hline(c, 22, 55, UI_W - 44, C_GRUVBOX_DARK1);
}

static void draw_picker_footer(struct canvas *c, struct font_ctx *font,
			       bool permission_mode)
{
	hline(c, 22, 344, UI_W - 44, C_GRUVBOX_LINE);
	hline(c, 22, 345, UI_W - 44, C_GRUVBOX_DARK1);
	draw_pet_rotate_icon(c, 42, 362, false, C_GRUVBOX_YELLOW);
	draw_pet_rotate_icon(c, 88, 362, true, C_GRUVBOX_YELLOW);
	draw_pet_font_text(c, font, 140, 360, "select sid", 12, 2,
			   C_GRUVBOX_TEXT);
	draw_pet_font_text(c, font, 332, 360, "|", 12, 2, C_GRUVBOX_MUTED);
	draw_pet_down_icon(c, 392, 362, C_GRUVBOX_YELLOW);
	draw_pet_font_text(c, font, 432, 360,
			   permission_mode ? "confirm allow" : "confirm focus",
			   12, 2, C_GRUVBOX_TEXT);
	draw_pet_font_text(c, font, 690, 360, "|", 12, 2, C_GRUVBOX_MUTED);
	draw_pet_font_text(c, font, 742, 360,
			   permission_mode ? "reject deny" : "reject = back",
			   12, 2,
			   C_GRUVBOX_TEXT);
}

/* Picker view: KEY 2 (SESSION) jumps here. Renders sid + state plus compact
 * host-pushed agent metadata. The
 * picker_idx member of ui_model is no longer used — selection lives in
 * g_lcd_selected_sid, and the list comes from g_board_sessions[]. */
static void render_session_picker(struct canvas *c, const struct ui_model *m,
				  struct font_ctx *font,
				  const struct pet_state *p)
{
	uint64_t now_ms = monotonic_now_ms();
	int seconds = (int)((now_ms - p->start_time_ms) / 1000u);
	struct board_session *rows[MAX_BOARD_SESSIONS];
	int count = 0;

	(void)m;

	for (int i = 0; i < MAX_BOARD_SESSIONS; i++) {
		if (g_board_sessions[i].used)
			rows[count++] = &g_board_sessions[i];
	}

	canvas_clear(c, C_GRUVBOX_BG);
	fill_rect(c, 0, 0, UI_W, UI_H, 0x141312);
	draw_picker_header(c, font, seconds, count);

	const char *banner = "[ SELECT SESSION ]";
	int bw = text_w(banner, 2);
	draw_text(c, (UI_W - bw) / 2, 78, banner, 2, C_GRUVBOX_YELLOW);

	if (count == 0) {
		const char *msg = "NO HOST SESSIONS YET";
		int mw = text_w(msg, 2);
		draw_text(c, (UI_W - mw) / 2, 192, msg, 2, C_GRUVBOX_MUTED);
		return;
	}

	int picker_idx = 0;
	for (int i = 0; i < count; i++) {
		if (rows[i]->sid == g_lcd_selected_sid) {
			picker_idx = i;
			break;
		}
	}
	draw_picker_footer(c, font, rows[picker_idx]->perm.active);

	const int rows_visible = 3;
	int start = picker_idx - rows_visible / 2;
	int max_start = count - rows_visible;
	if (max_start < 0)
		max_start = 0;
	if (start < 0)
		start = 0;
	if (start > max_start)
		start = max_start;

	for (int i = 0; i < rows_visible && (start + i) < count; i++) {
		int idx = start + i;
		int row_y = 130 + i * 60;
		bool sel = (idx == picker_idx);
		const struct board_session *bs = rows[idx];
		char sid_label[24];
		char meta_label[160];
		char detail_label[144];
		const char *state_label =
			board_session_state_str(bs->state);
		uint32_t state_color = sel ? C_GRUVBOX_YELLOW : C_GRUVBOX_MUTED;
		if (bs->state == BSS_DISCONNECTED || bs->state == BSS_ERROR)
			state_color = sel ? C_GRUVBOX_RED : 0x8a3a25;

		snprintf(sid_label, sizeof(sid_label), "SID %u", bs->sid);
		if (bs->meta.hint[0] || bs->meta.kind[0] ||
		    bs->meta.cwd[0] || bs->meta.branch[0]) {
			const char *kind = bs->meta.kind[0] ? bs->meta.kind : "agent";
			const char *name = bs->meta.hint[0] ? bs->meta.hint : kind;
			const char *cwd = path_tail(bs->meta.cwd);
			if (bs->meta.branch[0] && cwd[0])
				snprintf(meta_label, sizeof(meta_label), "%s  %s  %s",
					 name, cwd, bs->meta.branch);
			else if (cwd[0])
				snprintf(meta_label, sizeof(meta_label), "%s  %s",
					 name, cwd);
			else
				snprintf(meta_label, sizeof(meta_label), "%s", name);
		} else {
			snprintf(meta_label, sizeof(meta_label), "waiting for agent meta");
		}

		if (bs->perm.active) {
			snprintf(detail_label, sizeof(detail_label), "PERM #%llu %s %s",
				 (unsigned long long)bs->perm.req_id,
				 bs->perm.tool, bs->perm.args);
		} else if (bs->turn.text[0]) {
			snprintf(detail_label, sizeof(detail_label), "%s: %s",
				 bs->turn.role[0] ? bs->turn.role : "turn",
				 bs->turn.text);
		} else if (bs->token.input || bs->token.output || bs->token.cost_cents) {
			snprintf(detail_label, sizeof(detail_label),
				 "TOK in=%llu out=%llu cost=%llu",
				 (unsigned long long)bs->token.input,
				 (unsigned long long)bs->token.output,
				 (unsigned long long)bs->token.cost_cents);
		} else {
			snprintf(detail_label, sizeof(detail_label), "no activity yet");
		}

		if (sel) {
			fill_rect(c, 60, row_y - 10, UI_W - 120, 54, 0x1a0e02);
			stroke_round_rect(c, 60, row_y - 8,
					  UI_W - 120, 48, 6, C_GRUVBOX_YELLOW);
		}
		draw_text_fit(c, 96, row_y, 132, sid_label, 2,
			      sel ? C_GRUVBOX_YELLOW : C_GRUVBOX_TEXT);
		draw_text_fit(c, 246, row_y + 1, 420, meta_label, 1,
			      sel ? C_GRUVBOX_TEXT : C_GRUVBOX_MUTED);
		draw_text_fit(c, 728, row_y, 132, state_label, 2,
			      state_color);
		draw_text_fit(c, 246, row_y + 23, 560, detail_label, 1,
			      bs->perm.active ? C_GRUVBOX_RED :
			      (sel ? C_GRUVBOX_YELLOW : C_GRUVBOX_MUTED));
	}
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
	safe_copy(m->sessions[idx].tool, sizeof(m->sessions[idx].tool), "Host");
	safe_copy(m->sessions[idx].mode, sizeof(m->sessions[idx].mode), "CLI");
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

/*
 * Specialized hot path for the only configuration this product actually ships:
 * 32-bpp ARGB8888 framebuffer rotated 90deg CW from the 960x412 landscape
 * canvas. Skips per-pixel sample_canvas() / pack_fb_pixel() (which together do
 * a multiply, two divisions, four bounds checks, and a switch per pixel) and
 * collapses the inner loop to load-OR-store. Drops fb_blit from ~hundreds of
 * ms per frame to a few ms, which is what the user sees as "instant" frame
 * transitions instead of "缓缓刷出".
 */
static bool fb_blit_fast_argb_cw(struct fb_target *fb, const struct canvas *c)
{
	uint32_t a_const;

	if (fb->var.bits_per_pixel != 32)
		return false;
	if (resolve_pixel_format(fb->pixel_format, fb) != PIXEL_FMT_ARGB8888)
		return false;
	if (resolve_rotation(fb->rotate, fb->var.xres, fb->var.yres) != ROT_CW)
		return false;
	if ((int)fb->var.xres != c->h || (int)fb->var.yres != c->w)
		return false;

	a_const = (uint32_t)fb->alpha << 24;
	for (int cy = 0; cy < c->h; cy++) {
		const uint32_t *src = c->px + (size_t)cy * c->w;
		size_t fb_x_off = (size_t)(c->h - 1 - cy) * 4u;
		for (int cx = 0; cx < c->w; cx++) {
			uint32_t rgb = src[cx];
			uint8_t *dst = fb->mem + (size_t)cx * fb->fix.line_length
				       + fb_x_off;
			*(uint32_t *)dst = a_const | (rgb & 0x00ffffffu);
		}
	}
	fb_refresh(fb);
	return true;
}

static void fb_blit(struct fb_target *fb, const struct canvas *c)
{
	int width;
	int height;
	int bpp;
	int bytes;
	enum rotation rot;

	if (fb_blit_fast_argb_cw(fb, c))
		return;

	width = (int)fb->var.xres;
	height = (int)fb->var.yres;
	bpp = (int)fb->var.bits_per_pixel;
	bytes = (bpp + 7) / 8;
	rot = resolve_rotation(fb->rotate, width, height);

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

static int pet_write_qa_dump(const char *path, const struct canvas *c,
			     const struct pet_state *p)
{
	const int box_x = PET_STAGE_X;
	const int box_y = PET_STAGE_Y;
	const int box_w = PET_STAGE_W;
	const int box_h = PET_STAGE_H;
	const struct pet_scene_asset_manifest *manifest =
		pet_asset_manifest(p->scene);
	const struct anim_state *a = &g_pet_anims[p->scene];
	FILE *fp = fopen(path, "w");
	int min_x = box_x + box_w;
	int min_y = box_y + box_h;
	int max_x = box_x - 1;
	int max_y = box_y - 1;
	int visible = 0;
	int dark = 0;
	int bright = 0;

	if (!fp) {
		warnf("open %s failed: %s", path, strerror(errno));
		return -1;
	}
	for (int y = box_y; y < box_y + box_h; y++) {
		for (int x = box_x; x < box_x + box_w; x++) {
			uint32_t rgb = c->px[y * c->w + x];
			int r = (int)((rgb >> 16) & 0xff);
			int g = (int)((rgb >> 8) & 0xff);
			int b = (int)(rgb & 0xff);
			int luma = r * 3 + g * 4 + b;

			if (luma > 130) {
				visible++;
				if (x < min_x)
					min_x = x;
				if (x > max_x)
					max_x = x;
				if (y < min_y)
					min_y = y;
				if (y > max_y)
					max_y = y;
			}
			if (luma < 70)
				dark++;
			if (luma > 900)
				bright++;
		}
	}

	fprintf(fp, "AIKB pet character QA dump\n");
	fprintf(fp, "scene=%s pose=%s character=%s\n",
		pet_scene_name(p->scene), pet_pose_name(p->anim.pose),
		PET_CORE_CHARACTER.name);
	fprintf(fp, "asset_root=%s\n", g_pet_asset_root[0] ?
		g_pet_asset_root : "(compile-time paths)");
	fprintf(fp, "resource=%s\n", pet_asset_path(p->scene));
	fprintf(fp, "asset_loaded=%s fallback_forced=%s\n",
		(a->active && a->base) ? "yes" : "no",
		g_pet_force_fallback ? "yes" : "no");
	if (manifest) {
		fprintf(fp, "manifest state=%s expected_frames=%u frame_duration_ms=%u\n",
			manifest->state, manifest->expected_frames,
			manifest->frame_duration_ms);
	}
	fprintf(fp, "stage_box=%d,%d %dx%d\n", box_x, box_y, box_w, box_h);
	if (visible) {
		fprintf(fp, "rendered_bbox=%d,%d %dx%d visible_pixels=%d\n",
			min_x, min_y, max_x - min_x + 1, max_y - min_y + 1,
			visible);
	} else {
		fprintf(fp, "rendered_bbox=none visible_pixels=0\n");
	}
	fprintf(fp, "stage_dark_pixels=%d stage_bright_pixels=%d\n", dark, bright);

	if (a->active && a->base) {
		struct anim_header hdr;
		const uint8_t *frame;
		size_t transparent = 0;
		size_t semitransparent = 0;
		size_t opaque = 0;
		size_t edges = 0;
		size_t pixels;
		int src_min_x = 0;
		int src_min_y = 0;
		int src_max_x = -1;
		int src_max_y = -1;
		int scale;
		int dst_w;
		int dst_h;
		int dx;
		int dy;

		memcpy(&hdr, a->base, sizeof(hdr));
		pixels = (size_t)hdr.width * hdr.height;
		frame = a->base + sizeof(struct anim_header) +
			(size_t)a->frame_idx * pixels * 4u;
		src_min_x = (int)hdr.width;
		src_min_y = (int)hdr.height;
		for (uint32_t y = 0; y < hdr.height; y++) {
			for (uint32_t x = 0; x < hdr.width; x++) {
				const uint8_t *px = frame + ((size_t)y * hdr.width + x) * 4u;
				uint8_t alpha = px[3];

				if (!alpha) {
					transparent++;
				} else {
					if ((int)x < src_min_x)
						src_min_x = (int)x;
					if ((int)x > src_max_x)
						src_max_x = (int)x;
					if ((int)y < src_min_y)
						src_min_y = (int)y;
					if ((int)y > src_max_y)
						src_max_y = (int)y;
					if (alpha == 255)
						opaque++;
					else
						semitransparent++;
				}
				if (x > 0) {
					const uint8_t *left = px - 4;

					if (px[0] != left[0] || px[1] != left[1] ||
					    px[2] != left[2] || px[3] != left[3])
						edges++;
				}
				if (y > 0) {
					const uint8_t *up = px - (size_t)hdr.width * 4u;

					if (px[0] != up[0] || px[1] != up[1] ||
					    px[2] != up[2] || px[3] != up[3])
						edges++;
				}
			}
		}
		fprintf(fp, "akim=%ux%u frames=%u delay_ms=%u format=%s frame=%u\n",
			hdr.width, hdr.height, hdr.frame_count,
			hdr.frame_delay_ms,
			g_pet_anim_argb8888[p->scene] ? "ARGB8888" : "RGBA",
			a->frame_idx);
		fprintf(fp, "alpha transparent=%zu semitransparent=%zu opaque=%zu\n",
			transparent, semitransparent, opaque);
		scale = box_w / (int)hdr.width;
		if (box_h / (int)hdr.height < scale)
			scale = box_h / (int)hdr.height;
		if (scale < 1)
			scale = 1;
		dst_w = (int)hdr.width * scale;
		dst_h = (int)hdr.height * scale;
		dx = box_x + (box_w - dst_w) / 2;
		dy = box_y + (box_h - dst_h) / 2;
		if (src_max_x >= src_min_x && src_max_y >= src_min_y) {
			fprintf(fp, "asset_alpha_bbox=%d,%d %dx%d\n",
				src_min_x, src_min_y,
				src_max_x - src_min_x + 1,
				src_max_y - src_min_y + 1);
			fprintf(fp, "asset_rendered_bbox=%d,%d %dx%d scale=%d\n",
				dx + src_min_x * scale, dy + src_min_y * scale,
				(src_max_x - src_min_x + 1) * scale,
				(src_max_y - src_min_y + 1) * scale, scale);
		}
		fprintf(fp, "clarity_edge_changes=%zu\n", edges);
	} else {
		fprintf(fp, "fallback_map=%dx%d\n", PET_DINO_W, PET_DINO_H);
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

static int load_raw_canvas(const char *path, uint32_t **slot,
			   const char *label, bool show_after_load)
{
	struct stat st;
	uint32_t *buf;
	size_t got = 0;
	int fd;

	if (!path)
		return -1;
	if (stat(path, &st) < 0) {
		warnf("%s %s: stat failed: %s", label, path, strerror(errno));
		return -1;
	}
	if ((size_t)st.st_size != SPLASH_BYTES) {
		warnf("%s %s: size %lld != %zu (expect %dx%d 0x00RRGGBB)",
		      label, path, (long long)st.st_size, SPLASH_BYTES,
		      UI_W, UI_H);
		return -1;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		warnf("%s %s: open failed: %s", label, path, strerror(errno));
		return -1;
	}
	buf = malloc(SPLASH_BYTES);
	if (!buf) {
		warnf("%s %s: malloc %zu failed", label, path, SPLASH_BYTES);
		close(fd);
		return -1;
	}
	while (got < SPLASH_BYTES) {
		ssize_t n = read(fd, (uint8_t *)buf + got, SPLASH_BYTES - got);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			warnf("%s %s: read failed: %s", label, path, strerror(errno));
			free(buf);
			close(fd);
			return -1;
		}
		if (n == 0)
			break;
		got += (size_t)n;
	}
	close(fd);
	if (got != SPLASH_BYTES) {
		warnf("%s %s: short read %zu/%zu", label, path, got,
		      SPLASH_BYTES);
		free(buf);
		return -1;
	}
	free(*slot);
	*slot = buf;
	if (show_after_load)
		g_show_splash = true;
	warnf("%s %s loaded (%dx%d, %zu bytes)", label, path, UI_W, UI_H,
	      SPLASH_BYTES);
	return 0;
}

static int load_splash(const char *path)
{
	return load_raw_canvas(path, &g_splash, "splash", true);
}

static int load_ui_shell(const char *path)
{
	return load_raw_canvas(path, &g_ui_shell, "ui shell", false);
}

static long monotonic_elapsed_ms(const struct timespec *since)
{
	struct timespec now;
	long sec;
	long ms;

	clock_gettime(CLOCK_MONOTONIC, &now);
	sec = (long)(now.tv_sec - since->tv_sec);
	ms = (long)((now.tv_nsec - since->tv_nsec) / 1000000L);
	return sec * 1000L + ms;
}

static void anim_release(struct anim_state *a)
{
	if (a->base) {
		munmap((void *)a->base, a->size);
		a->base = NULL;
	}
	a->size = 0;
	a->frame_count = 0;
	a->frame_idx = 0;
	a->active = false;
	a->loop = false;
	a->argb8888 = false;
}

static void release_intro_surfaces(void)
{
	if (g_boot_anim.active)
		anim_release(&g_boot_anim);
	if (g_wait_anim.active)
		anim_release(&g_wait_anim);
	if (g_show_splash)
		g_show_splash = false;
}

static bool view_allows_idle_sleep(enum app_view view)
{
	return view != VIEW_SESSION_PICKER && view != VIEW_TERMINAL;
}

static void idle_sleep_touch(void)
{
	g_last_local_input_ms = monotonic_now_ms();
	g_idle_sleep_active = false;
	if (g_idle_sleep_anim.base) {
		g_idle_sleep_anim.frame_idx = 0;
		g_idle_sleep_anim.active = true;
		clock_gettime(CLOCK_MONOTONIC, &g_idle_sleep_anim.started_at);
	}
}

static void idle_sleep_update(enum app_view view)
{
	uint64_t now;

	if (!view_allows_idle_sleep(view) || !g_idle_sleep_anim.base) {
		g_idle_sleep_active = false;
		return;
	}
	now = monotonic_now_ms();
	if (g_last_local_input_ms == 0)
		g_last_local_input_ms = now;
	if (now - g_last_local_input_ms < IDLE_SLEEP_TIMEOUT_MS) {
		g_idle_sleep_active = false;
		return;
	}
	if (!g_idle_sleep_active) {
		g_idle_sleep_active = true;
		g_idle_sleep_anim.frame_idx = 0;
		g_idle_sleep_anim.active = true;
		clock_gettime(CLOCK_MONOTONIC, &g_idle_sleep_anim.started_at);
	}
}

static int anim_load(struct anim_state *a, const char *path)
{
	struct stat st;
	void *map;
	struct anim_header hdr;
	size_t expected;
	int fd;

	if (!path)
		return -1;
	if (stat(path, &st) < 0) {
		warnf("%s %s: stat failed: %s", a->label, path, strerror(errno));
		return -1;
	}
	if ((size_t)st.st_size < sizeof(hdr)) {
		warnf("%s %s: file too small (%lld < %zu)", a->label, path,
		      (long long)st.st_size, sizeof(hdr));
		return -1;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		warnf("%s %s: open failed: %s", a->label, path, strerror(errno));
		return -1;
	}
	map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		warnf("%s %s: mmap failed: %s", a->label, path, strerror(errno));
		return -1;
	}
	memcpy(&hdr, map, sizeof(hdr));
	if (memcmp(hdr.magic, ANIM_MAGIC_STR, 4) != 0) {
		warnf("%s %s: bad magic %02x %02x %02x %02x", a->label, path,
		      (uint8_t)hdr.magic[0], (uint8_t)hdr.magic[1],
		      (uint8_t)hdr.magic[2], (uint8_t)hdr.magic[3]);
		munmap(map, (size_t)st.st_size);
		return -1;
	}
	if (hdr.version != 1 ||
	    (hdr.flags & ~(ANIM_FLAG_LOOP | PET_AKIM_FLAG_ARGB8888)) != 0) {
		warnf("%s %s: version=%u flags=0x%x not supported", a->label,
		      path, hdr.version, hdr.flags);
		munmap(map, (size_t)st.st_size);
		return -1;
	}
	if (hdr.width != (uint32_t)UI_W || hdr.height != (uint32_t)UI_H) {
		warnf("%s %s: %ux%u != canvas %dx%d", a->label, path, hdr.width,
		      hdr.height, UI_W, UI_H);
		munmap(map, (size_t)st.st_size);
		return -1;
	}
	if (hdr.frame_count == 0 || hdr.frame_delay_ms == 0) {
		warnf("%s %s: empty (count=%u delay=%u)", a->label, path,
		      hdr.frame_count, hdr.frame_delay_ms);
		munmap(map, (size_t)st.st_size);
		return -1;
	}
	expected = sizeof(hdr) + (size_t)hdr.frame_count * SPLASH_BYTES;
	if ((size_t)st.st_size != expected) {
		warnf("%s %s: size %lld != expected %zu", a->label, path,
		      (long long)st.st_size, expected);
		munmap(map, (size_t)st.st_size);
		return -1;
	}
	a->base = (const uint8_t *)map;
	a->size = (size_t)st.st_size;
	a->frame_count = hdr.frame_count;
	a->frame_delay_ms = hdr.frame_delay_ms;
	a->frame_idx = 0;
	a->loop = (hdr.flags & ANIM_FLAG_LOOP) != 0;
	a->argb8888 = (hdr.flags & PET_AKIM_FLAG_ARGB8888) != 0;
	a->active = true;
	clock_gettime(CLOCK_MONOTONIC, &a->started_at);
	warnf("%s %s loaded (%u frames * %u ms%s, %s, %zu bytes)", a->label, path,
	      a->frame_count, a->frame_delay_ms, a->loop ? ", looping" : "",
	      a->argb8888 ? "ARGB8888" : "RGBA",
	      a->size);
	return 0;
}

static int pet_akim_load(struct anim_state *a,
			 const struct pet_scene_asset_manifest *manifest,
			 bool *argb8888)
{
	struct stat st;
	void *map;
	struct anim_header hdr;
	size_t frame_bytes;
	size_t expected;
	const char *path;
	int fd;

	if (!manifest)
		return -1;
	path = pet_asset_path(manifest->scene);
	if (!path)
		return -1;
	if (stat(path, &st) < 0) {
		if (errno != ENOENT)
			warnf("%s %s: stat failed: %s", a->label, path,
			      strerror(errno));
		return -1;
	}
	if ((size_t)st.st_size < sizeof(hdr)) {
		warnf("%s %s: file too small (%lld < %zu)", a->label, path,
		      (long long)st.st_size, sizeof(hdr));
		return -1;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		warnf("%s %s: open failed: %s", a->label, path, strerror(errno));
		return -1;
	}
	map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		warnf("%s %s: mmap failed: %s", a->label, path, strerror(errno));
		return -1;
	}
	memcpy(&hdr, map, sizeof(hdr));
	if (memcmp(hdr.magic, ANIM_MAGIC_STR, 4) != 0) {
		warnf("%s %s: bad magic %02x %02x %02x %02x", a->label, path,
		      (uint8_t)hdr.magic[0], (uint8_t)hdr.magic[1],
		      (uint8_t)hdr.magic[2], (uint8_t)hdr.magic[3]);
		munmap(map, (size_t)st.st_size);
		return -1;
	}
	if (hdr.version != 1 ||
	    (hdr.flags & ~(ANIM_FLAG_LOOP | PET_AKIM_FLAG_ARGB8888)) != 0) {
		warnf("%s %s: version=%u flags=0x%x not supported", a->label,
		      path, hdr.version, hdr.flags);
		munmap(map, (size_t)st.st_size);
		return -1;
	}
	if (hdr.width == 0 || hdr.height == 0 ||
	    hdr.width > (uint32_t)UI_W || hdr.height > (uint32_t)UI_H ||
	    hdr.frame_count == 0 || hdr.frame_delay_ms == 0) {
		warnf("%s %s: invalid %ux%u count=%u delay=%u", a->label,
		      path, hdr.width, hdr.height, hdr.frame_count,
		      hdr.frame_delay_ms);
		munmap(map, (size_t)st.st_size);
		return -1;
	}
	frame_bytes = (size_t)hdr.width * hdr.height * 4u;
	expected = sizeof(hdr) + (size_t)hdr.frame_count * frame_bytes;
	if ((size_t)st.st_size != expected) {
		warnf("%s %s: size %lld != expected %zu", a->label, path,
		      (long long)st.st_size, expected);
		munmap(map, (size_t)st.st_size);
		return -1;
	}
	a->base = (const uint8_t *)map;
	a->size = (size_t)st.st_size;
	a->frame_count = hdr.frame_count;
	a->frame_delay_ms = hdr.frame_delay_ms;
	a->frame_idx = 0;
	a->loop = true;
	a->active = true;
	*argb8888 = (hdr.flags & PET_AKIM_FLAG_ARGB8888) != 0;
	clock_gettime(CLOCK_MONOTONIC, &a->started_at);
	if (manifest->expected_frames &&
	    hdr.frame_count < manifest->expected_frames) {
		warnf("%s %s: %u frames < manifest expected %u",
		      a->label, path, hdr.frame_count,
		      manifest->expected_frames);
	}
	if (manifest->frame_duration_ms &&
	    hdr.frame_delay_ms != manifest->frame_duration_ms) {
		warnf("%s %s: frame delay %u ms differs from manifest %u ms",
		      a->label, path, hdr.frame_delay_ms,
		      manifest->frame_duration_ms);
	}
	warnf("%s %s loaded (%ux%u, %u frames * %u ms, %s, %zu bytes)",
	      a->label, path, hdr.width, hdr.height, a->frame_count,
	      a->frame_delay_ms, *argb8888 ? "ARGB8888" : "RGBA",
	      a->size);
	return 0;
}

static const char *pet_scene_name(enum pet_scene scene)
{
	const struct pet_scene_asset_manifest *manifest = pet_asset_manifest(scene);

	return manifest ? manifest->state : "unknown";
}

static const char *pet_pose_name(enum pet_pose pose)
{
	if (pose >= 0 && pose < PET_POSE_COUNT)
		return PET_CORE_POSE_RANGES[pose].state;
	return "unknown";
}

static void anim_blit(const struct anim_state *a, struct canvas *c)
{
	const uint8_t *frame = a->base + sizeof(struct anim_header) +
			       (size_t)a->frame_idx * SPLASH_BYTES;
	if (!a->argb8888) {
		memcpy(c->px, frame, SPLASH_BYTES);
		return;
	}
	for (size_t i = 0; i < (size_t)UI_W * UI_H; i++) {
		const uint8_t *p = frame + i * 4u;

		c->px[i] = ((uint32_t)p[2] << 16) |
			   ((uint32_t)p[1] << 8) |
			   (uint32_t)p[0];
	}
}

/*
 * Advance frame_idx based on monotonic time since started_at. Returns ms-to-
 * next-frame so the caller can shrink the poll timeout. Releases the slot when
 * a non-looping animation reaches the end; returns -1 in that case.
 */
static long anim_advance(struct anim_state *a)
{
	long elapsed;
	uint32_t step;

	elapsed = monotonic_elapsed_ms(&a->started_at);
	if (elapsed < 0)
		elapsed = 0;
	step = (uint32_t)(elapsed / a->frame_delay_ms);
	if (a->loop) {
		a->frame_idx = step % a->frame_count;
	} else if (step >= a->frame_count) {
		anim_release(a);
		return -1;
	} else {
		a->frame_idx = step;
	}
	return (long)a->frame_delay_ms -
	       (elapsed % a->frame_delay_ms);
}

static int open_input(const char *path)
{
	struct stat st;
	bool is_fifo = false;
	bool is_char = false;
	int fd;

	if (!path)
		return -1;
	if (strcmp(path, "-") == 0) {
		fd = STDIN_FILENO;
		is_char = true;
	} else {
		if (stat(path, &st) == 0) {
			is_fifo = S_ISFIFO(st.st_mode);
			is_char = S_ISCHR(st.st_mode);
		}
		if (is_fifo)
			fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		else
			fd = open(path, O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	}
	if (fd < 0) {
		warnf("open input %s failed: %s", path, strerror(errno));
		return -1;
	}
	if (fd != STDIN_FILENO && is_char && configure_serial_fd(fd) < 0)
		warnf("serial setup for %s failed: %s", path, strerror(errno));
	return fd;
}

struct terminal_emit_ctx {
	struct terminal *term;
	struct kitty_graphics *kitty;
};

static void terminal_emit_byte(void *ctx, uint8_t byte)
{
	struct terminal_emit_ctx *emit = ctx;

	terminal_process_byte(emit->term, byte);
	if (emit->term->clear_graphics) {
		kitty_graphics_clear(emit->kitty);
		emit->term->clear_graphics = false;
	}
}

static struct kitty_graphics_metrics kitty_metrics_for_term(
	const struct terminal *term)
{
	struct kitty_graphics_metrics m;

	m.pad_x = TERM_PAD_X;
	m.pad_y = TERM_PAD_Y;
	m.cell_w = g_cell_w;
	m.cell_h = g_cell_h;
	m.cols = g_cols;
	m.rows = g_rows;
	m.cursor_col = term->col;
	m.cursor_row = term->row;
	return m;
}

static bool pet_input_is_command(const char *buf, ssize_t len)
{
	ssize_t i = 0;

	while (i < len && (buf[i] == '\r' || buf[i] == '\n' || buf[i] == ' '))
		i++;
	return i + 4 <= len && !memcmp(buf + i, "PET ", 4);
}

static const char *app_view_name(enum app_view view)
{
	switch (view) {
	case VIEW_TERMINAL:
		return "terminal";
	case VIEW_DASHBOARD:
		return "dashboard";
	case VIEW_PET:
		return "pet";
	case VIEW_SESSION_PICKER:
		return "picker";
	}
	return "unknown";
}

static void feed_terminal_bytes(struct terminal *term, struct kitty_graphics *kitty,
				const char *buf, ssize_t len)
{
	if (len > 0)
		g_terminal_has_data = true;
	struct terminal_emit_ctx emit_ctx = {
		.term = term,
		.kitty = kitty,
	};

	for (ssize_t i = 0; i < len; i++) {
		struct kitty_graphics_metrics metrics = kitty_metrics_for_term(term);

		kitty_graphics_feed_byte(kitty, (uint8_t)buf[i],
					 terminal_emit_byte, &emit_ctx,
					 &metrics);
	}
}

static void process_input_fd(int fd, struct ui_model *m, struct terminal *term,
			     struct kitty_graphics *kitty, struct pet_state *pet,
			     enum app_view *view)
{
	static char buf[LINE_BUF];
	static size_t len;
	static uint64_t diag_input_reads;
	static uint64_t diag_input_bytes;
	static uint64_t diag_pet_drop_reads;
	static uint64_t diag_pet_drop_bytes;
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
		diag_input_reads++;
		diag_input_bytes += (uint64_t)rd;
		if (*view == VIEW_TERMINAL) {
			int before_cells = terminal_non_empty_cells(term);
			uint64_t before_printable = term->diag_printable;
			uint64_t before_clears = term->diag_full_clears;

			feed_terminal_bytes(term, kitty, tmp, rd);
			warnf("diag input view=terminal bytes=%zd reads=%llu total_bytes=%llu state=%d row=%d col=%d viewport=%d effective=%d scroll_offset=%d history=%d cells=%d->%d printable_delta=%llu printable_total=%llu clears_delta=%llu clears_total=%llu has_data=%d",
			      rd,
			      (unsigned long long)diag_input_reads,
			      (unsigned long long)diag_input_bytes,
			      term->state, term->row, term->col,
			      term->viewport_top,
			      terminal_effective_viewport_top(term),
			      term->scrollback_offset,
			      term->scrollback_count,
			      before_cells, terminal_non_empty_cells(term),
			      (unsigned long long)(term->diag_printable -
						   before_printable),
			      (unsigned long long)term->diag_printable,
			      (unsigned long long)(term->diag_full_clears -
						   before_clears),
			      (unsigned long long)term->diag_full_clears,
			      g_terminal_has_data ? 1 : 0);
			continue;
		}
		if (*view == VIEW_PET && !pet_input_is_command(tmp, rd)) {
			diag_pet_drop_reads++;
			diag_pet_drop_bytes += (uint64_t)rd;
			warnf("diag input view=%s bytes=%zd action=drop_non_pet drops=%llu drop_bytes=%llu total_reads=%llu total_bytes=%llu",
			      app_view_name(*view), rd,
			      (unsigned long long)diag_pet_drop_reads,
			      (unsigned long long)diag_pet_drop_bytes,
			      (unsigned long long)diag_input_reads,
			      (unsigned long long)diag_input_bytes);
			continue;
		}
		for (ssize_t i = 0; i < rd; i++) {
			char ch = tmp[i];

			if (ch == '\r')
				continue;
			if (ch == '\n') {
				buf[len] = '\0';
				if (len > 0) {
					if (*view == VIEW_PET)
						pet_apply_command(pet, buf);
					else
						apply_json_line(m, buf);
				}
				len = 0;
			} else if (len + 1 < sizeof(buf)) {
				buf[len++] = ch;
			} else {
				len = 0;
			}
		}
	}
}

static void process_event_fd(int fd, struct pet_state *pet, struct terminal *term,
			     enum app_view *view)
{
	static char buf[LINE_BUF];
	static size_t len;
	char tmp[128];
	ssize_t rd;

	for (;;) {
		rd = read(fd, tmp, sizeof(tmp));
		if (rd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			warnf("event input read failed: %s", strerror(errno));
			return;
		}
		if (rd == 0)
			return;
		for (ssize_t i = 0; i < rd; i++) {
			char ch = tmp[i];

			if (ch == '\r')
				continue;
			if (ch != '\n') {
				if (len + 1 < sizeof(buf))
					buf[len++] = ch;
				else
					len = 0;
				continue;
			}

			buf[len] = '\0';
			if (len == 0) {
				continue;
			}
			idle_sleep_touch();

			/* Encoder rotation: in picker view, step the highlighted
			 * sid and tell aikb_hid_input via ui-ctrl-out. In any
			 * other view, encoder events are forwarded to the host
			 * (the HID send path stamps g_active_sid on them). */
			if (!strncmp(buf, "ENC ", 4)) {
				if (*view == VIEW_SESSION_PICKER) {
					int dir = 0;
					if (!strncmp(buf, "ENC +", 5))
						dir = +1;
					else if (!strncmp(buf, "ENC -", 5))
						dir = -1;
					if (dir != 0 && board_session_count_live() > 0) {
						g_lcd_selected_sid =
							board_session_step(g_lcd_selected_sid, dir);
						ui_ctrl_emit_select(g_lcd_selected_sid);
					}
				} else if (*view == VIEW_TERMINAL && term) {
					int dir = 0;
					if (!strncmp(buf, "ENC +", 5))
						dir = +1;
					else if (!strncmp(buf, "ENC -", 5))
						dir = -1;
					terminal_scroll_view(term, dir);
					warnf("diag terminal scroll dir=%d offset=%d history=%d viewport=%d",
					      dir, term->scrollback_offset,
					      term->scrollback_count,
					      term->viewport_top);
				}
				len = 0;
				continue;
			}

			if (*view == VIEW_TERMINAL && term &&
			    streq_ci(buf, "ENC_BTN DOWN")) {
				term->scrollback_offset = 0;
				warnf("diag terminal scroll bottom history=%d viewport=%d",
				      term->scrollback_count, term->viewport_top);
				len = 0;
				continue;
			}

			/* ENC_BTN / CONFIRM (KEY 6) in the picker with a live
			 * selection: focus that sid and switch to terminal.
			 * Without a selection, ENC_BTN does nothing here and
			 * CONFIRM falls through to its generic KEY_ACTIONS
			 * handler below (pet + UPDATING scene). */
			if (*view == VIEW_SESSION_PICKER &&
			    g_lcd_selected_sid != 0 &&
			    streq_ci(buf, "KEY 0 DOWN")) {
				int idx = board_session_find_idx(g_lcd_selected_sid);
				if (idx >= 0 && g_board_sessions[idx].perm.active) {
					uint64_t req_id =
						g_board_sessions[idx].perm.req_id;
					ui_ctrl_emit_permission(g_lcd_selected_sid,
								req_id, "deny");
					board_session_clear_perm(g_lcd_selected_sid,
								 req_id);
					len = 0;
					continue;
				}
			}
			if (*view == VIEW_SESSION_PICKER &&
			    g_lcd_selected_sid != 0 &&
			    (streq_ci(buf, "ENC_BTN DOWN") ||
			     streq_ci(buf, "KEY 6 DOWN"))) {
				int idx = board_session_find_idx(g_lcd_selected_sid);
				if (idx >= 0 && g_board_sessions[idx].perm.active) {
					uint64_t req_id =
						g_board_sessions[idx].perm.req_id;
					ui_ctrl_emit_permission(g_lcd_selected_sid,
								req_id, "allow");
					board_session_clear_perm(g_lcd_selected_sid,
								 req_id);
					len = 0;
					continue;
				}
			}
			if (*view == VIEW_SESSION_PICKER &&
			    g_lcd_selected_sid != 0 &&
			    (streq_ci(buf, "ENC_BTN DOWN") ||
			     streq_ci(buf, "KEY 6 DOWN"))) {
				ui_ctrl_emit_focus(g_lcd_selected_sid);
				g_lcd_active_sid = g_lcd_selected_sid;
				if (term)
					terminal_reset(term);
				*view = VIEW_TERMINAL;
				ui_ctrl_emit_view("terminal");
				release_intro_surfaces();
				warnf("diag picker focus sid=%u view=%s",
				      g_lcd_active_sid, app_view_name(*view));
				len = 0;
				continue;
			}
			if (*view == VIEW_SESSION_PICKER &&
			    streq_ci(buf, "ENC_BTN DOWN")) {
				/* No sid selected — ENC_BTN is the picker
				 * confirm key only; nothing else to do. */
				len = 0;
				continue;
			}

			/* SESSION opens the board-local picker. Stateless: we
			 * do not remember which view we came from, so any
			 * subsequent product key (including REJECT) just runs
			 * its normal action from the picker. */
			if (streq_ci(buf, "KEY 2 DOWN")) {
				if (*view != VIEW_SESSION_PICKER) {
					if (g_lcd_selected_sid == 0 ||
					    board_session_find_idx(g_lcd_selected_sid) < 0)
						g_lcd_selected_sid =
							board_session_pick_first();
					*view = VIEW_SESSION_PICKER;
					ui_ctrl_emit_view("picker");
					if (g_lcd_selected_sid != 0)
						ui_ctrl_emit_select(g_lcd_selected_sid);
					release_intro_surfaces();
				}
				len = 0;
				continue;
			}

			/* Any remaining product key (KEY 0/1/3/4/5/6) jumps to
			 * the pet view with its matching scene + overlay. This
			 * fires from any starting view, including the picker
			 * — so users can exit the picker straight into the pet
			 * view via any product key, not only SESSION/REJECT.
			 *
			 * KEY 2 is filtered out because SESSION is handled
			 * above (it opens the picker, not the pet). */
			for (size_t k = 0;
			     k < sizeof(KEY_ACTIONS) / sizeof(KEY_ACTIONS[0]);
			     k++) {
				const struct key_action *act = &KEY_ACTIONS[k];

				if (!streq_ci(buf, act->event_line))
					continue;
				if (!strcmp(act->event_line, "KEY 2 DOWN"))
					break;
				if (*view == VIEW_SESSION_PICKER) {
					/* Leaving the picker: tell hid_input
					 * so the HID send path stops treating
					 * outgoing key events as picker-local. */
					ui_ctrl_emit_view("terminal");
				}
				*view = VIEW_PET;
				pet_set_scene_title(pet, act->scene,
						    act->label, NULL);
				release_intro_surfaces();
				break;
			}
			len = 0;
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
	if (streq_ci(s, "pet") || streq_ci(s, "desktop-pet")) {
		*out = VIEW_PET;
		return 0;
	}
	if (streq_ci(s, "picker") || streq_ci(s, "session") ||
	    streq_ci(s, "sessions")) {
		*out = VIEW_SESSION_PICKER;
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

static int parse_cell_size(const char *s, int *out_w, int *out_h)
{
	const char *sep = strchr(s, 'x');
	long w;
	long h;
	char *end = NULL;

	if (!sep || sep == s)
		sep = strchr(s, 'X');
	if (!sep || sep == s)
		return -1;
	errno = 0;
	w = strtol(s, &end, 10);
	if (errno || end != sep)
		return -1;
	errno = 0;
	h = strtol(sep + 1, &end, 10);
	if (errno || !end || *end || w <= 0 || h <= 0 || w > 255 || h > 255)
		return -1;
	if (find_cell_preset((int)w, (int)h) < 0)
		return -1;
	*out_w = (int)w;
	*out_h = (int)h;
	return 0;
}

static void copy_range(char *dst, size_t dst_sz, const char *begin,
		       const char *end)
{
	size_t n;

	if (!dst_sz)
		return;
	if (!begin)
		begin = "";
	if (!end || end < begin)
		end = begin + strlen(begin);
	while (end > begin && isspace((unsigned char)end[-1]))
		end--;
	while (*begin && begin < end && isspace((unsigned char)*begin))
		begin++;
	n = (size_t)(end - begin);
	if (n >= dst_sz)
		n = dst_sz - 1;
	memcpy(dst, begin, n);
	dst[n] = '\0';
}

static bool process_session_ctrl_line(const char *line)
{
	unsigned sid_val;
	int rest_off = 0;
	const char *rest;

	if (sscanf(line, "session %u %n", &sid_val, &rest_off) != 1 ||
	    sid_val == 0 || rest_off <= 0)
		return false;
	rest = line + rest_off;

	if (!strncmp(rest, "state ", 6)) {
		enum board_session_state st;
		char state_name[24];

		if (sscanf(rest + 6, "%23s", state_name) == 1 &&
		    board_session_state_from_name(state_name, &st))
			board_session_upsert((uint16_t)sid_val, st);
		return true;
	}
	if (!strncmp(rest, "removed", 7)) {
		board_session_remove((uint16_t)sid_val);
		return true;
	}
	if (!strncmp(rest, "hint ", 5)) {
		board_session_update_hint((uint16_t)sid_val, rest + 5);
		return true;
	}
	if (!strncmp(rest, "token ", 6)) {
		unsigned long long input, output, cost;

		if (sscanf(rest + 6, "in=%llu out=%llu cost=%llu",
			   &input, &output, &cost) == 3) {
			board_session_update_token((uint16_t)sid_val,
						   (uint64_t)input,
						   (uint64_t)output,
						   (uint64_t)cost);
		}
		return true;
	}
	if (!strncmp(rest, "turn ", 5)) {
		char role[sizeof(g_board_sessions[0].turn.role)];
		int text_off = 0;

		if (sscanf(rest + 5, "role=%11s text:%n", role, &text_off) == 1 &&
		    text_off > 0) {
			board_session_update_turn((uint16_t)sid_val, role,
						  rest + 5 + text_off);
		}
		return true;
	}
	if (!strncmp(rest, "permission ", 11)) {
		unsigned long long req_id;
		char tool[sizeof(g_board_sessions[0].perm.tool)];
		int args_off = 0;

		if (sscanf(rest + 11, "reqid=%llu tool=%23s args:%n",
			   &req_id, tool, &args_off) == 2 && args_off > 0) {
			board_session_update_perm((uint16_t)sid_val,
						  (uint64_t)req_id, tool,
						  rest + 11 + args_off);
		}
		return true;
	}
	if (!strncmp(rest, "meta ", 5)) {
		const char *kind_begin = strstr(rest, "kind=");
		const char *cwd_begin = strstr(rest, " cwd=");
		const char *branch_begin = strstr(rest, " branch=");
		char kind[sizeof(g_board_sessions[0].meta.kind)];
		char cwd[sizeof(g_board_sessions[0].meta.cwd)];
		char branch[sizeof(g_board_sessions[0].meta.branch)];

		if (kind_begin && cwd_begin && branch_begin &&
		    kind_begin < cwd_begin && cwd_begin < branch_begin) {
			kind_begin += 5;
			cwd_begin += 5;
			branch_begin += 8;
			copy_range(kind, sizeof(kind), kind_begin, cwd_begin - 5);
			copy_range(cwd, sizeof(cwd), cwd_begin, branch_begin - 8);
			copy_range(branch, sizeof(branch), branch_begin, NULL);
			board_session_update_meta((uint16_t)sid_val, kind, cwd,
						  branch);
		}
		return true;
	}
	return false;
}

static void process_ctrl_fd(int fd, struct font_ctx *font, struct terminal *t,
			    enum app_view *view)
{
	static char buf[LINE_BUF];
	static size_t len;
	char tmp[128];
	ssize_t rd;

	for (;;) {
		rd = read(fd, tmp, sizeof(tmp));
		if (rd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			warnf("ctrl read failed: %s", strerror(errno));
			return;
		}
		if (rd == 0)
			return;
		for (ssize_t i = 0; i < rd; i++) {
			char ch = tmp[i];

			if (ch == '\r')
				continue;
			if (ch == '\n') {
				int w;
				int h;
				enum app_view next_view;

				buf[len] = '\0';
				if (len > 0 &&
				    sscanf(buf, "cell %d %d", &w, &h) == 2)
					apply_cell_size(font, t, w, h);
				else if (len > 5 &&
					 !strncmp(buf, "view ", 5) &&
					 parse_view(buf + 5, &next_view) == 0) {
					*view = next_view;
					release_intro_surfaces();
					warnf("diag ctrl view=%s",
					      app_view_name(*view));
				} else {
					(void)process_session_ctrl_line(buf);
				}
				len = 0;
			} else if (len + 1 < sizeof(buf)) {
				buf[len++] = ch;
			} else {
				len = 0;
			}
		}
	}
}

static void render_terminal_waiting_overlay(struct canvas *c,
					    const struct ui_model *model)
{
	const char *msg = "WAITING FOR HOST TERMINAL STREAM";
	int W = c->w;
	int H = c->h;
	int tw = text_w(msg, 2);
	int box_w = tw + 80;
	if (box_w > W - 60)
		box_w = W - 60;
	int box_h = 96;
	int box_x = (W - box_w) / 2;
	int box_y = (H - box_h) / 2;

	fill_rect(c, box_x, box_y, box_w, box_h, 0x080807);
	stroke_round_rect(c, box_x, box_y, box_w, box_h, 6, 0xf4b83d);
	int tx = (W - tw) / 2;
	int ty = box_y + 18;
	draw_text(c, tx, ty, msg, 2, 0xffc233);

	if (model->count > 0) {
		char buf[96];
		int idx = model->focus;
		if (idx < 0 || idx >= model->count)
			idx = 0;
		const struct ui_session *s = &model->sessions[idx];
		snprintf(buf, sizeof(buf), "session: %s",
			 s->id[0] ? s->id : "-");
		int sw = text_w(buf, 1);
		draw_text(c, (W - sw) / 2, ty + 50, buf, 1, 0xa77a3d);
	} else {
		const char *sub = "no sessions registered yet";
		int sw = text_w(sub, 1);
		draw_text(c, (W - sw) / 2, ty + 50, sub, 1, 0xa77a3d);
	}
}

static void render_app(struct canvas *c, enum app_view view,
		       const struct ui_model *model, const struct terminal *term,
		       struct font_ctx *font, const struct kitty_graphics *kitty,
		       struct pet_state *pet)
{
	if (view == VIEW_TERMINAL) {
		render_terminal(c, term, font, kitty);
		if (!g_terminal_has_data)
			render_terminal_waiting_overlay(c, model);
	} else if (view == VIEW_DASHBOARD) {
		render_dashboard(c, model, font, pet);
	} else if (view == VIEW_SESSION_PICKER) {
		render_session_picker(c, model, font, pet);
	} else {
		render_pet(c, font, pet);
	}
}

static void render_frame(struct canvas *c, enum app_view view,
			 const struct ui_model *model,
			 const struct terminal *term, struct font_ctx *font,
			 const struct kitty_graphics *kitty,
			 struct pet_state *pet)
{
	if (g_boot_anim.active && g_boot_anim.base) {
		anim_blit(&g_boot_anim, c);
		return;
	}
	if (g_wait_anim.active && g_wait_anim.base) {
		anim_blit(&g_wait_anim, c);
		return;
	}
	if (g_idle_sleep_active && g_idle_sleep_anim.active &&
	    g_idle_sleep_anim.base) {
		anim_blit(&g_idle_sleep_anim, c);
		return;
	}
	if (g_show_splash && g_splash) {
		memcpy(c->px, g_splash, SPLASH_BYTES);
		return;
	}
	render_app(c, view, model, term, font, kitty, pet);
	/* No bottom-left pet overlay: action keys swap the view to VIEW_PET
	 * (mascot centred, video-style header/footer); dashboard / picker /
	 * terminal show data layouts without an extra mascot layer. */
}

static void usage(const char *argv0)
{
	printf("Usage: %s [options]\n", argv0);
	printf("  --fb PATH          framebuffer path (default /dev/fb0)\n");
	printf("  --input PATH       terminal bytes or newline JSON input, '-' for stdin\n");
	printf("  --view MODE        terminal/vt100, dashboard/json, pet, or picker (default terminal)\n");
	printf("  --event-input PATH local button/encoder event FIFO for pet view\n");
	printf("  --font PATH        preferred TrueType/OpenType font path\n");
	printf("  --cell WxH         initial terminal cell size; one of 8x16, 10x20, 12x24, 16x32\n");
	printf("  --ctrl PATH        runtime control FIFO; lines like \"cell 12 24\" or \"view terminal\"\n");
	printf("  --ui-ctrl-out PATH ui-ctrl FIFO toward aikb_hid_input: emits \"view picker|terminal\", \"select N\", \"focus N\"\n");
	printf("  --splash PATH      raw 960x412 0x00RRGGBB splash shown until first input byte\n");
	printf("  --ui-shell PATH    raw 960x412 dashboard shell background\n");
	printf("  --boot-anim PATH   AKIM container (scripts/make_boot_anim.py) played once before splash\n");
	printf("  --wait-anim PATH   AKIM container with LOOP flag, replays until first input byte\n");
	printf("  --sleep-anim PATH  AKIM container shown after 3 minutes without local keys outside session views\n");
	printf("  --pet-asset-root PATH directory containing pet AKIM files such as asking.akim\n");
	printf("  --pet-scene SCENE  initial pet scene for dumps/tests\n");
	printf("  --pet-pose POSE    initial pet pose: idle/thinking/happy/confused/sleepy\n");
	printf("  --pet-progress N   initial pet progress, 0..100\n");
	printf("  --pet-force-fallback draw the C fallback pet even if AKIM assets load\n");
	printf("  --rotate MODE      auto, none, cw, ccw (default auto)\n");
	printf("  --pixel-format FMT auto, fb, argb8888, abgr8888 (default auto)\n");
	printf("  --alpha N          framebuffer alpha byte, 0..255 (default 255)\n");
	printf("  --dump-ppm PATH    render one frame to a PPM file\n");
	printf("  --pet-qa-dump PATH write pet character asset/bbox/color QA text\n");
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
	const char *ctrl_path = NULL;
	const char *event_input_path = NULL;
	const char *splash_path = NULL;
	const char *ui_shell_path = NULL;
	const char *boot_anim_path = NULL;
	const char *wait_anim_path = NULL;
	const char *sleep_anim_path = NULL;
	const char *pet_asset_root = NULL;
	const char *pet_qa_dump_path = NULL;
	enum rotation rotate = ROT_AUTO;
	enum pixel_format pixel_format = PIXEL_FMT_AUTO;
	enum app_view view = VIEW_TERMINAL;
	bool once = false;
	bool mock = true;
	int alpha = 255;
	int once_hold_ms = 3000;
	int initial_cell_w = g_cell_w;
	int initial_cell_h = g_cell_h;
	enum pet_scene initial_pet_scene = PET_SCENE_ASKING;
	enum pet_pose initial_pet_pose = PET_POSE_THINKING;
	int initial_pet_progress = 58;
	bool initial_pet_pose_set = false;
	struct canvas c = {
		.w = UI_W,
		.h = UI_H,
		.px = NULL,
	};
	struct ui_model model;
	struct terminal term;
	struct pet_state pet;
	struct font_ctx font;
	struct fb_target fb;
	struct kitty_graphics *kitty = NULL;
	int input_fd = -1;
	int ctrl_fd = -1;
	int event_fd = -1;
	int ret = 0;
	time_t next_input_retry = 0;
	time_t next_ctrl_retry = 0;
	time_t next_event_retry = 0;

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
		} else if (!strcmp(argv[i], "--cell") && i + 1 < argc) {
			if (parse_cell_size(argv[++i], &initial_cell_w,
					    &initial_cell_h) < 0) {
				usage(argv[0]);
				return 2;
			}
		} else if (!strcmp(argv[i], "--ctrl") && i + 1 < argc) {
			ctrl_path = argv[++i];
		} else if (!strcmp(argv[i], "--ui-ctrl-out") && i + 1 < argc) {
			g_ui_ctrl_out_path = argv[++i];
		} else if (!strcmp(argv[i], "--event-input") && i + 1 < argc) {
			event_input_path = argv[++i];
		} else if (!strcmp(argv[i], "--splash") && i + 1 < argc) {
			splash_path = argv[++i];
		} else if (!strcmp(argv[i], "--ui-shell") && i + 1 < argc) {
			ui_shell_path = argv[++i];
		} else if (!strcmp(argv[i], "--boot-anim") && i + 1 < argc) {
			boot_anim_path = argv[++i];
		} else if (!strcmp(argv[i], "--wait-anim") && i + 1 < argc) {
			wait_anim_path = argv[++i];
		} else if (!strcmp(argv[i], "--sleep-anim") && i + 1 < argc) {
			sleep_anim_path = argv[++i];
		} else if (!strcmp(argv[i], "--pet-asset-root") && i + 1 < argc) {
			pet_asset_root = argv[++i];
		} else if (!strcmp(argv[i], "--pet-scene") && i + 1 < argc) {
			if (!parse_pet_scene(argv[++i], &initial_pet_scene)) {
				usage(argv[0]);
				return 2;
			}
		} else if (!strcmp(argv[i], "--pet-pose") && i + 1 < argc) {
			if (!parse_pet_pose(argv[++i], &initial_pet_pose)) {
				usage(argv[0]);
				return 2;
			}
			initial_pet_pose_set = true;
		} else if (!strcmp(argv[i], "--pet-progress") && i + 1 < argc) {
			if (parse_int_arg(argv[++i], 0, 100,
					  &initial_pet_progress) < 0) {
				usage(argv[0]);
				return 2;
			}
		} else if (!strcmp(argv[i], "--pet-force-fallback")) {
			g_pet_force_fallback = true;
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
		} else if (!strcmp(argv[i], "--pet-qa-dump") && i + 1 < argc) {
			pet_qa_dump_path = argv[++i];
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
	kitty = kitty_graphics_create();
	if (!kitty) {
		warnf("kitty graphics allocation failed");
		free(c.px);
		return 1;
	}

	if (splash_path)
		load_splash(splash_path);
	if (ui_shell_path)
		load_ui_shell(ui_shell_path);
	if (boot_anim_path)
		anim_load(&g_boot_anim, boot_anim_path);
	if (wait_anim_path)
		anim_load(&g_wait_anim, wait_anim_path);
	if (sleep_anim_path)
		anim_load(&g_idle_sleep_anim, sleep_anim_path);
	if (pet_asset_root)
		pet_set_asset_root(pet_asset_root);
	for (int i = 0; i < PET_SCENE_COUNT; i++)
		pet_akim_load(&g_pet_anims[i], pet_asset_manifest((enum pet_scene)i),
			      &g_pet_anim_argb8888[i]);

	g_cell_w = initial_cell_w;
	g_cell_h = initial_cell_h;
	recompute_term_geom();

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
	pet_init(&pet);
	pet_set_scene(&pet, initial_pet_scene, NULL);
	if (initial_pet_pose_set)
		pet_set_pose(&pet, initial_pet_pose);
	pet.progress = initial_pet_progress;
	g_last_local_input_ms = monotonic_now_ms();
	if (!font_init(&font, font_path))
		warnf("freetype font unavailable; falling back to built-in 8x16 glyphs");

	if (term.clear_graphics) {
		kitty_graphics_clear(kitty);
		term.clear_graphics = false;
	}

	if (dump_path && ctrl_path) {
		ctrl_fd = open_input(ctrl_path);
		if (ctrl_fd >= 0) {
			process_ctrl_fd(ctrl_fd, &font, &term, &view);
			if (ctrl_fd != STDIN_FILENO)
				close(ctrl_fd);
			ctrl_fd = -1;
		}
	}
	render_app(&c, view, &model, &term, &font, kitty, &pet);
	if (pet_qa_dump_path && pet_write_qa_dump(pet_qa_dump_path, &c, &pet) < 0)
		ret = 1;
	if (dump_path) {
		if (write_ppm(dump_path, &c) < 0)
			ret = 1;
		kitty_graphics_destroy(kitty);
		font_destroy(&font);
		anim_release(&g_idle_sleep_anim);
		for (int i = 0; i < PET_SCENE_COUNT; i++)
			anim_release(&g_pet_anims[i]);
		free(c.px);
		return ret ? 1 : 0;
	}

	if (fb_open_target(&fb, fb_path, rotate, pixel_format,
			   (uint8_t)alpha) < 0) {
		kitty_graphics_destroy(kitty);
		font_destroy(&font);
		free(c.px);
		return 1;
	}

	input_fd = open_input(input_path);
	if (input_fd < 0 && input_path)
		next_input_retry = time(NULL) + 5;
	if (ctrl_path) {
		ctrl_fd = open_input(ctrl_path);
		if (ctrl_fd < 0)
			next_ctrl_retry = time(NULL) + 5;
	}
	if (event_input_path) {
		event_fd = open_input(event_input_path);
		if (event_fd < 0)
			next_event_retry = time(NULL) + 5;
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	if (g_boot_anim.active)
		clock_gettime(CLOCK_MONOTONIC, &g_boot_anim.started_at);
	if (g_wait_anim.active)
		clock_gettime(CLOCK_MONOTONIC, &g_wait_anim.started_at);

	while (!g_stop) {
		struct pollfd pfds[3];
		nfds_t nfds = 0;
		int input_idx = -1;
		int ctrl_idx = -1;
		int event_idx = -1;
		int timeout = 1000;
		time_t now;

		idle_sleep_update(view);
		if (g_boot_anim.active) {
			long ms = anim_advance(&g_boot_anim);
			if (!g_boot_anim.active && g_wait_anim.active)
				clock_gettime(CLOCK_MONOTONIC,
					      &g_wait_anim.started_at);
			if (ms > 0 && ms < timeout)
				timeout = (int)ms;
		}
		if (g_wait_anim.active) {
			long ms = anim_advance(&g_wait_anim);
			if (ms > 0 && ms < timeout)
				timeout = (int)ms;
		}
		if (g_idle_sleep_active && g_idle_sleep_anim.active) {
			long ms = anim_advance(&g_idle_sleep_anim);
			if (ms > 0 && ms < timeout)
				timeout = (int)ms;
		}

		if (view == VIEW_PET && timeout > 83)
			timeout = 83;

		render_frame(&c, view, &model, &term, &font, kitty, &pet);
		fb_blit(&fb, &c);
		if (once) {
			if (once_hold_ms > 0)
				usleep((useconds_t)once_hold_ms * 1000);
			break;
		}

		now = time(NULL);
		if (input_fd < 0 && input_path && now >= next_input_retry) {
			input_fd = open_input(input_path);
			next_input_retry = now + 5;
		}
		if (ctrl_fd < 0 && ctrl_path && now >= next_ctrl_retry) {
			ctrl_fd = open_input(ctrl_path);
			next_ctrl_retry = now + 5;
		}
		if (event_fd < 0 && event_input_path && now >= next_event_retry) {
			event_fd = open_input(event_input_path);
			next_event_retry = now + 5;
		}

		if (input_fd >= 0) {
			input_idx = (int)nfds;
			pfds[nfds].fd = input_fd;
			pfds[nfds].events = POLLIN;
			pfds[nfds].revents = 0;
			nfds++;
		}
		if (ctrl_fd >= 0) {
			ctrl_idx = (int)nfds;
			pfds[nfds].fd = ctrl_fd;
			pfds[nfds].events = POLLIN;
			pfds[nfds].revents = 0;
			nfds++;
		}
		if (event_fd >= 0) {
			event_idx = (int)nfds;
			pfds[nfds].fd = event_fd;
			pfds[nfds].events = POLLIN;
			pfds[nfds].revents = 0;
			nfds++;
		}

		if (nfds == 0) {
			usleep((useconds_t)timeout * 1000);
			continue;
		}

		if (poll(pfds, nfds, timeout) <= 0)
			continue;
		if (ctrl_idx >= 0 && (pfds[ctrl_idx].revents & POLLIN))
			process_ctrl_fd(ctrl_fd, &font, &term, &view);
		if (input_idx >= 0 && (pfds[input_idx].revents & POLLIN))
			process_input_fd(input_fd, &model, &term, kitty, &pet,
					 &view);
		if (event_idx >= 0 && (pfds[event_idx].revents & POLLIN))
			process_event_fd(event_fd, &pet, &term, &view);
		if (term.clear_graphics) {
			kitty_graphics_clear(kitty);
			term.clear_graphics = false;
		}
	}

	if (input_fd >= 0 && input_fd != STDIN_FILENO)
		close(input_fd);
	if (ctrl_fd >= 0 && ctrl_fd != STDIN_FILENO)
		close(ctrl_fd);
	if (event_fd >= 0 && event_fd != STDIN_FILENO)
		close(event_fd);
	fb_close_target(&fb);
	kitty_graphics_destroy(kitty);
	font_destroy(&font);
	free(c.px);
	free(g_splash);
	g_splash = NULL;
	free(g_ui_shell);
	g_ui_shell = NULL;
	g_show_splash = false;
	anim_release(&g_boot_anim);
	anim_release(&g_wait_anim);
	anim_release(&g_idle_sleep_anim);
	for (int i = 0; i < PET_SCENE_COUNT; i++)
		anim_release(&g_pet_anims[i]);
	return ret;
}
