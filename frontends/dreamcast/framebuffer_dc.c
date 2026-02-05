/*
 * Dreamcast wrapper for framebuffer frontend implementation.
 *
 * The upstream framebuffer frontend is reused by including its implementation
 * file, but we wrap cursor handling so the Dreamcast SDL blitter can composite
 * the cursor sprite.
 */

#include <stdbool.h>

struct fbtk_bitmap;

/* Rename the upstream symbol so we can provide a Dreamcast wrapper. */
#define framebuffer_set_cursor framebuffer_set_cursor_upstream
#include "../framebuffer/framebuffer.c"
#undef framebuffer_set_cursor

#include "dreamcast/sdl_dc_surface.h"

bool
framebuffer_set_cursor(struct fbtk_bitmap *bm)
{
	bool ret;

	ret = framebuffer_set_cursor_upstream(bm);
	/* Cache cursor sprite for Dreamcast SDL overlay rendering. */
	dreamcast_sdl_set_cursor(bm);

	return ret;
}
