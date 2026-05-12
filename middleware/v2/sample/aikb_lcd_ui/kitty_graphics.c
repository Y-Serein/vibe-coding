// SPDX-License-Identifier: GPL-2.0+
/*
 * Minimal Kitty graphics protocol filter for aikb_lcd_ui.
 *
 * Supported subset:
 *   ESC _ G <control>; <base64-png-payload> ESC \
 *   a=T, f=100, t=d, m=0/1, q=2, c/r, i/p
 */

#define _DEFAULT_SOURCE

#include "kitty_graphics.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define KG_CONTROL_MAX 512
#define KG_MAX_BASE64 (2u * 1024u * 1024u)
#define KG_MAX_PLACEMENTS 8

enum kg_state {
	KG_NORMAL,
	KG_ESC,
	KG_APC_PREFIX,
	KG_BODY,
	KG_BODY_ESC,
	KG_SKIP_APC,
	KG_SKIP_APC_ESC,
};

struct kg_params {
	char action;
	int format;
	char transmit;
	int more;
	int quiet;
	int cols;
	int rows;
	int image_id;
	int placement_id;
};

struct kg_accum {
	bool active;
	bool drop;
	struct kg_params params;
	char *b64;
	size_t len;
	size_t cap;
};

struct kg_placement {
	bool active;
	int image_id;
	int placement_id;
	int x;
	int y;
	int w;
	int h;
	uint32_t *argb;
};

struct kitty_graphics {
	enum kg_state state;
	bool in_payload;
	bool drop_seq;
	char control[KG_CONTROL_MAX];
	size_t control_len;
	char *payload;
	size_t payload_len;
	size_t payload_cap;
	struct kg_accum accum;
	struct kg_placement placements[KG_MAX_PLACEMENTS];
	int next_slot;
};

static void warnf(const char *msg)
{
	fprintf(stderr, "aikb_lcd_ui: kitty_graphics: %s\n", msg);
}

static void kg_params_init(struct kg_params *p)
{
	memset(p, 0, sizeof(*p));
	p->more = 0;
	p->quiet = 0;
	p->cols = 0;
	p->rows = 0;
	p->image_id = 0;
	p->placement_id = 0;
}

static int parse_int(const char *s)
{
	char *end = NULL;
	long v;

	if (!s || !*s)
		return 0;
	v = strtol(s, &end, 10);
	if (!end || *end)
		return 0;
	if (v < 0)
		return 0;
	if (v > 0x7fffffffL)
		return 0x7fffffff;
	return (int)v;
}

static void parse_control(const char *control, struct kg_params *p)
{
	const char *s = control;

	kg_params_init(p);
	while (*s) {
		const char *key = s;
		const char *eq = strchr(s, '=');
		const char *comma = strchr(s, ',');
		size_t val_len;
		char value[32];

		if (!comma)
			comma = s + strlen(s);
		if (!eq || eq > comma || eq == key || eq + 1 >= comma) {
			s = *comma ? comma + 1 : comma;
			continue;
		}

		val_len = (size_t)(comma - eq - 1);
		if (val_len >= sizeof(value))
			val_len = sizeof(value) - 1;
		memcpy(value, eq + 1, val_len);
		value[val_len] = '\0';

		switch (*key) {
		case 'a':
			p->action = value[0];
			break;
		case 'f':
			p->format = parse_int(value);
			break;
		case 't':
			p->transmit = value[0];
			break;
		case 'm':
			p->more = parse_int(value) ? 1 : 0;
			break;
		case 'q':
			p->quiet = parse_int(value);
			break;
		case 'c':
			p->cols = parse_int(value);
			break;
		case 'r':
			p->rows = parse_int(value);
			break;
		case 'i':
			p->image_id = parse_int(value);
			break;
		case 'p':
			p->placement_id = parse_int(value);
			break;
		default:
			break;
		}
		s = *comma ? comma + 1 : comma;
	}
}

static bool supported_params(const struct kg_params *p)
{
	return p->action == 'T' && p->format == 100 && p->transmit == 'd';
}

static bool grow_buf(char **buf, size_t *cap, size_t need)
{
	char *new_buf;
	size_t new_cap = *cap ? *cap : 4096;

	while (new_cap < need) {
		if (new_cap > KG_MAX_BASE64 / 2u)
			new_cap = KG_MAX_BASE64;
		else
			new_cap *= 2u;
		if (new_cap < need && new_cap == KG_MAX_BASE64)
			return false;
	}
	new_buf = realloc(*buf, new_cap);
	if (!new_buf)
		return false;
	*buf = new_buf;
	*cap = new_cap;
	return true;
}

static void seq_reset(struct kitty_graphics *kg)
{
	kg->in_payload = false;
	kg->drop_seq = false;
	kg->control_len = 0;
	kg->payload_len = 0;
	if (kg->control[0])
		kg->control[0] = '\0';
}

static void accum_reset(struct kg_accum *a)
{
	free(a->b64);
	memset(a, 0, sizeof(*a));
}

static void placement_release(struct kg_placement *p)
{
	free(p->argb);
	memset(p, 0, sizeof(*p));
}

static void append_seq_byte(struct kitty_graphics *kg, uint8_t b)
{
	if (kg->drop_seq)
		return;
	if (!kg->in_payload) {
		if (b == ';') {
			kg->in_payload = true;
			if (kg->control_len >= sizeof(kg->control))
				kg->control_len = sizeof(kg->control) - 1;
			kg->control[kg->control_len] = '\0';
			return;
		}
		if (kg->control_len + 1 < sizeof(kg->control)) {
			kg->control[kg->control_len++] = (char)b;
			kg->control[kg->control_len] = '\0';
		} else {
			kg->drop_seq = true;
		}
		return;
	}
	if (kg->payload_len + 1 > KG_MAX_BASE64) {
		kg->drop_seq = true;
		return;
	}
	if (kg->payload_len + 1 > kg->payload_cap &&
	    !grow_buf(&kg->payload, &kg->payload_cap, kg->payload_len + 1)) {
		kg->drop_seq = true;
		return;
	}
	kg->payload[kg->payload_len++] = (char)b;
}

static int b64_value(unsigned char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

static uint8_t *base64_decode_alloc(const char *src, size_t len, size_t *out_len)
{
	uint8_t *out;
	size_t out_cap = (len / 4u + 1u) * 3u;
	size_t n = 0;
	int val = 0;
	int valb = -8;

	*out_len = 0;
	out = malloc(out_cap ? out_cap : 1u);
	if (!out)
		return NULL;

	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)src[i];
		int d;

		if (isspace(c))
			continue;
		if (c == '=')
			break;
		d = b64_value(c);
		if (d < 0) {
			free(out);
			return NULL;
		}
		val = (val << 6) | d;
		valb += 6;
		if (valb >= 0) {
			if (n >= out_cap) {
				free(out);
				return NULL;
			}
			out[n++] = (uint8_t)((val >> valb) & 0xff);
			valb -= 8;
		}
	}
	*out_len = n;
	return out;
}

static uint32_t blend_rgb(uint32_t dst, uint32_t src, uint8_t alpha)
{
	uint32_t dr = (dst >> 16) & 0xffu;
	uint32_t dg = (dst >> 8) & 0xffu;
	uint32_t db = dst & 0xffu;
	uint32_t sr = (src >> 16) & 0xffu;
	uint32_t sg = (src >> 8) & 0xffu;
	uint32_t sb = src & 0xffu;
	uint32_t inv = 255u - alpha;

	return (((sr * alpha + dr * inv) / 255u) << 16) |
	       (((sg * alpha + dg * inv) / 255u) << 8) |
	       ((sb * alpha + db * inv) / 255u);
}

static int placement_slot(struct kitty_graphics *kg, int image_id, int placement_id)
{
	int slot;

	for (int i = 0; i < KG_MAX_PLACEMENTS; i++) {
		if (kg->placements[i].active &&
		    kg->placements[i].image_id == image_id &&
		    kg->placements[i].placement_id == placement_id)
			return i;
	}
	slot = kg->next_slot++ % KG_MAX_PLACEMENTS;
	placement_release(&kg->placements[slot]);
	return slot;
}

static void install_png(struct kitty_graphics *kg, const uint8_t *png,
			size_t png_len, const struct kg_params *params,
			const struct kitty_graphics_metrics *m)
{
	struct kg_placement *pl;
	uint8_t *rgba;
	uint32_t *scaled;
	int src_w;
	int src_h;
	int comp;
	int dst_w;
	int dst_h;
	int max_w;
	int max_h;
	int slot;

	if (!m || !png || !png_len)
		return;
	rgba = stbi_load_from_memory(png, (int)png_len, &src_w, &src_h, &comp, 4);
	if (!rgba || src_w <= 0 || src_h <= 0) {
		if (rgba)
			stbi_image_free(rgba);
		warnf("png decode failed");
		return;
	}

	dst_w = params->cols > 0 ? params->cols * m->cell_w : src_w;
	dst_h = params->rows > 0 ? params->rows * m->cell_h : src_h;
	if (dst_w < 1)
		dst_w = 1;
	if (dst_h < 1)
		dst_h = 1;
	max_w = (m->cols - m->cursor_col) * m->cell_w;
	max_h = (m->rows - m->cursor_row) * m->cell_h;
	if (max_w < 1)
		max_w = m->cell_w;
	if (max_h < 1)
		max_h = m->cell_h;
	if (dst_w > max_w)
		dst_w = max_w;
	if (dst_h > max_h)
		dst_h = max_h;

	scaled = calloc((size_t)dst_w * (size_t)dst_h, sizeof(*scaled));
	if (!scaled) {
		stbi_image_free(rgba);
		return;
	}
	for (int y = 0; y < dst_h; y++) {
		int sy = y * src_h / dst_h;
		for (int x = 0; x < dst_w; x++) {
			int sx = x * src_w / dst_w;
			const uint8_t *p = rgba + ((size_t)sy * src_w + sx) * 4u;
			scaled[(size_t)y * dst_w + x] =
				((uint32_t)p[3] << 24) |
				((uint32_t)p[0] << 16) |
				((uint32_t)p[1] << 8) |
				(uint32_t)p[2];
		}
	}
	stbi_image_free(rgba);

	slot = placement_slot(kg, params->image_id, params->placement_id);
	pl = &kg->placements[slot];
	pl->active = true;
	pl->image_id = params->image_id;
	pl->placement_id = params->placement_id;
	pl->x = m->pad_x + m->cursor_col * m->cell_w;
	pl->y = m->pad_y + m->cursor_row * m->cell_h;
	pl->w = dst_w;
	pl->h = dst_h;
	pl->argb = scaled;
}

static bool accum_append(struct kg_accum *a, const char *payload, size_t len)
{
	if (a->drop)
		return false;
	if (len > KG_MAX_BASE64 || a->len + len > KG_MAX_BASE64) {
		a->drop = true;
		return false;
	}
	if (a->len + len > a->cap &&
	    !grow_buf(&a->b64, &a->cap, a->len + len)) {
		a->drop = true;
		return false;
	}
	memcpy(a->b64 + a->len, payload, len);
	a->len += len;
	return true;
}

static void finish_sequence(struct kitty_graphics *kg,
			    const struct kitty_graphics_metrics *metrics)
{
	struct kg_params params;
	uint8_t *decoded;
	size_t decoded_len;

	if (kg->drop_seq || !kg->in_payload) {
		seq_reset(kg);
		return;
	}

	parse_control(kg->control, &params);
	if (!supported_params(&params)) {
		if (!params.more)
			accum_reset(&kg->accum);
		seq_reset(kg);
		return;
	}

	if (!kg->accum.active) {
		kg_params_init(&kg->accum.params);
		kg->accum.active = true;
		kg->accum.drop = false;
	}
	kg->accum.params = params;
	(void)accum_append(&kg->accum, kg->payload, kg->payload_len);

	if (params.more) {
		seq_reset(kg);
		return;
	}

	if (!kg->accum.drop && kg->accum.len > 0) {
		decoded = base64_decode_alloc(kg->accum.b64, kg->accum.len,
					      &decoded_len);
		if (decoded) {
			install_png(kg, decoded, decoded_len, &kg->accum.params,
				    metrics);
			free(decoded);
		} else {
			warnf("base64 decode failed");
		}
	}
	accum_reset(&kg->accum);
	seq_reset(kg);
}

struct kitty_graphics *kitty_graphics_create(void)
{
	struct kitty_graphics *kg = calloc(1, sizeof(*kg));

	if (kg)
		kg->state = KG_NORMAL;
	return kg;
}

void kitty_graphics_destroy(struct kitty_graphics *kg)
{
	if (!kg)
		return;
	kitty_graphics_clear(kg);
	accum_reset(&kg->accum);
	free(kg->payload);
	free(kg);
}

void kitty_graphics_clear(struct kitty_graphics *kg)
{
	if (!kg)
		return;
	for (int i = 0; i < KG_MAX_PLACEMENTS; i++)
		placement_release(&kg->placements[i]);
	kg->next_slot = 0;
	accum_reset(&kg->accum);
	seq_reset(kg);
	kg->state = KG_NORMAL;
}

void kitty_graphics_feed_byte(struct kitty_graphics *kg, uint8_t byte,
			      kitty_graphics_emit_fn emit, void *emit_ctx,
			      const struct kitty_graphics_metrics *metrics)
{
	if (!kg) {
		emit(emit_ctx, byte);
		return;
	}

	switch (kg->state) {
	case KG_NORMAL:
		if (byte == 0x1b) {
			kg->state = KG_ESC;
			return;
		}
		emit(emit_ctx, byte);
		return;
	case KG_ESC:
		if (byte == '_') {
			seq_reset(kg);
			kg->state = KG_APC_PREFIX;
			return;
		}
		emit(emit_ctx, 0x1b);
		if (byte == 0x1b)
			kg->state = KG_ESC;
		else {
			emit(emit_ctx, byte);
			kg->state = KG_NORMAL;
		}
		return;
	case KG_APC_PREFIX:
		if (byte == 'G') {
			kg->state = KG_BODY;
			return;
		}
		kg->state = KG_SKIP_APC;
		return;
	case KG_SKIP_APC:
		if (byte == 0x1b)
			kg->state = KG_SKIP_APC_ESC;
		return;
	case KG_SKIP_APC_ESC:
		kg->state = byte == '\\' ? KG_NORMAL : KG_SKIP_APC;
		return;
	case KG_BODY:
		if (byte == 0x1b) {
			kg->state = KG_BODY_ESC;
			return;
		}
		append_seq_byte(kg, byte);
		return;
	case KG_BODY_ESC:
		if (byte == '\\') {
			finish_sequence(kg, metrics);
			kg->state = KG_NORMAL;
			return;
		}
		append_seq_byte(kg, 0x1b);
		append_seq_byte(kg, byte);
		kg->state = KG_BODY;
		return;
	}
}

void kitty_graphics_render(const struct kitty_graphics *kg, int canvas_w,
			   int canvas_h, uint32_t *canvas_rgb)
{
	if (!kg || !canvas_rgb)
		return;
	for (int i = 0; i < KG_MAX_PLACEMENTS; i++) {
		const struct kg_placement *pl = &kg->placements[i];

		if (!pl->active || !pl->argb)
			continue;
		for (int y = 0; y < pl->h; y++) {
			int dy = pl->y + y;

			if (dy < 0 || dy >= canvas_h)
				continue;
			for (int x = 0; x < pl->w; x++) {
				int dx = pl->x + x;
				uint32_t src;
				uint8_t a;
				uint32_t *dst;

				if (dx < 0 || dx >= canvas_w)
					continue;
				src = pl->argb[(size_t)y * pl->w + x];
				a = (uint8_t)(src >> 24);
				if (!a)
					continue;
				dst = &canvas_rgb[(size_t)dy * canvas_w + dx];
				if (a == 255)
					*dst = src & 0x00ffffffu;
				else
					*dst = blend_rgb(*dst, src & 0x00ffffffu, a);
			}
		}
	}
}

int kitty_graphics_placement_count(const struct kitty_graphics *kg)
{
	int n = 0;

	if (!kg)
		return 0;
	for (int i = 0; i < KG_MAX_PLACEMENTS; i++)
		if (kg->placements[i].active)
			n++;
	return n;
}
