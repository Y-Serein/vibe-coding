// SPDX-License-Identifier: GPL-2.0+
#ifndef AIKB_KITTY_GRAPHICS_H
#define AIKB_KITTY_GRAPHICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct kitty_graphics;

struct kitty_graphics_metrics {
	int pad_x;
	int pad_y;
	int cell_w;
	int cell_h;
	int cols;
	int rows;
	int cursor_col;
	int cursor_row;
};

typedef void (*kitty_graphics_emit_fn)(void *ctx, uint8_t byte);

struct kitty_graphics *kitty_graphics_create(void);
void kitty_graphics_destroy(struct kitty_graphics *kg);
void kitty_graphics_clear(struct kitty_graphics *kg);

void kitty_graphics_feed_byte(struct kitty_graphics *kg, uint8_t byte,
			      kitty_graphics_emit_fn emit, void *emit_ctx,
			      const struct kitty_graphics_metrics *metrics);

void kitty_graphics_render(const struct kitty_graphics *kg, int canvas_w,
			   int canvas_h, uint32_t *canvas_rgb);

int kitty_graphics_placement_count(const struct kitty_graphics *kg);

#endif
