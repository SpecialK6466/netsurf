/*
 * Dreamcast SDL surface bridge (SDL 1.2 via kos-ports)
 *
 * Presents the RAM libnsfb surface via SDL.
 *
 * The kos-ports libnsfb "ram" surface does not render a visible cursor.
 * The older working PoC composites the NetSurf cursor sprite over the SDL
 * surface, so we do the same here.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL/SDL.h>

#include <libnsfb.h>

/* Forward declaration: avoid including kos-ports libnsfb_cursor.h.
 * The kos-ports header may lack nsfb_colour_t, but we only need loc_get.
 */
bool nsfb_cursor_loc_get(nsfb_t *nsfb, nsfb_bbox_t *loc);

/* fbtk.h relies on bbox_t which is typedef'd in framebuffer/gui.h */
#include "framebuffer/gui.h"
#include "framebuffer/fbtk.h"
#include "dreamcast/sdl_dc_surface.h"

/* Cached SDL surface and cursor sprite (RGBA8888). */
static SDL_Surface *dc_screen = NULL;
static struct {
	const uint8_t *rgba;
	int width;
	int height;
	int hot_x;
	int hot_y;
	bool valid;
} dc_cursor;

/* Performance optimization: dirty rectangle tracking */
static struct {
	bool active;
	nsfb_bbox_t dirty;
	bool dirty_set;
} dc_dirty_rect;

static inline uint16_t
dc_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void
dc_blit_cursor_rgba(SDL_Surface *dst, int cx, int cy)
{
	if (!dc_cursor.valid) {
		return;
	}

	int bpp = dst->format->BitsPerPixel;
	int start_x = cx - dc_cursor.hot_x;
	int start_y = cy - dc_cursor.hot_y;

	for (int sy = 0; sy < dc_cursor.height; sy++) {
		int dy = start_y + sy;
		if (dy < 0 || dy >= dst->h)
			continue;

		const uint8_t *src_row = dc_cursor.rgba + (sy * dc_cursor.width * 4);
		uint8_t *dst_row = (uint8_t *)dst->pixels + dy * dst->pitch;

		for (int sx = 0; sx < dc_cursor.width; sx++) {
			int dx = start_x + sx;
			if (dx < 0 || dx >= dst->w)
				continue;

			const uint8_t *sp = src_row + (sx * 4);
			uint8_t sr = sp[0];
			uint8_t sg = sp[1];
			uint8_t sb = sp[2];
			uint8_t sa = sp[3];

			if (sa == 0)
				continue;

			if (bpp == 16) {
				uint8_t *dp = dst_row + (dx * 2);
				uint16_t d;
				if (sa == 255) {
					uint16_t out = dc_rgb565(sr, sg, sb);
					memcpy(dp, &out, sizeof(out));
				} else {
					memcpy(&d, dp, sizeof(d));
					uint8_t dr = (uint8_t)(((d >> 11) & 0x1F) << 3);
					uint8_t dg = (uint8_t)(((d >> 5) & 0x3F) << 2);
					uint8_t db = (uint8_t)((d & 0x1F) << 3);
					uint8_t inv = (uint8_t)(255 - sa);
					uint8_t rr = (uint8_t)((sr * sa + dr * inv) / 255);
					uint8_t rg = (uint8_t)((sg * sa + dg * inv) / 255);
					uint8_t rb = (uint8_t)((sb * sa + db * inv) / 255);
					uint16_t out = dc_rgb565(rr, rg, rb);
					memcpy(dp, &out, sizeof(out));
				}
			} else if (bpp == 32) {
				uint8_t *dp = dst_row + (dx * 4);
				uint32_t d;
				memcpy(&d, dp, sizeof(d));
				uint8_t dr = (uint8_t)((d >> 16) & 0xFF);
				uint8_t dg = (uint8_t)((d >> 8) & 0xFF);
				uint8_t db = (uint8_t)(d & 0xFF);
				uint8_t inv = (uint8_t)(255 - sa);
				uint8_t rr = (uint8_t)((sr * sa + dr * inv) / 255);
				uint8_t rg = (uint8_t)((sg * sa + dg * inv) / 255);
				uint8_t rb = (uint8_t)((sb * sa + db * inv) / 255);
				uint32_t out = (0xFFu << 24) | ((uint32_t)rr << 16) | ((uint32_t)rg << 8) | (uint32_t)rb;
				memcpy(dp, &out, sizeof(out));
			}
		}
	}
}

bool
dreamcast_sdl_init(int width, int height, int bpp)
{
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "[dcdbg] SDL_Init failed: %s\n", SDL_GetError());
		return false;
	}

	/* Hide the SDL cursor; libnsfb tracks cursor */
	SDL_ShowCursor(SDL_DISABLE);

	/*
	 * Prefer double-buffered HW surface.
	 *
	 * On some KOS SDL builds, the screen may not update reliably without
	 * double buffering.
	 */
	uint32_t flags = SDL_HWSURFACE | SDL_DOUBLEBUF;
	dc_screen = SDL_SetVideoMode(width, height, bpp, flags);
	if (dc_screen == NULL) {
		/* Fallback: software surface (PoC used this). */
		flags = SDL_SWSURFACE;
		dc_screen = SDL_SetVideoMode(width, height, bpp, flags);
	}
	if (dc_screen == NULL) {
		fprintf(stderr, "[dcdbg] SDL_SetVideoMode failed: %s\n", SDL_GetError());
		return false;
	}

	fprintf(stderr, "[dcdbg] SDL initialized: %dx%d @ %d bpp (pitch=%d flags=0x%lx)\n",
		width, height, bpp, dc_screen->pitch, (unsigned long)dc_screen->flags);

	/*
	 * Sanity check: paint a visible colour once at startup.
	 * If the user still sees a black screen, presentation is broken.
	 */
	SDL_FillRect(dc_screen, NULL, SDL_MapRGB(dc_screen->format, 0, 0, 200));
	SDL_Flip(dc_screen);
	fprintf(stderr, "[dcdbg] SDL startup fill presented\n");

	return true;
}

void
dreamcast_sdl_update(nsfb_t *fb)
{
	static int update_count;
	static int sample_logged;
	static uint32_t last_update_time = 0;
	static int frame_skip_counter = 0;

	if (dc_screen == NULL || fb == NULL) {
		return;
	}

	/* Performance: frame rate limiting during page loads */
	uint32_t current_time = SDL_GetTicks();
	if (current_time - last_update_time < 16) { /* ~60 FPS cap */
		frame_skip_counter++;
		if (frame_skip_counter < 3) { /* Skip every 3rd frame during heavy load */
			return;
		}
	}
	frame_skip_counter = 0;
	last_update_time = current_time;

	uint8_t *fb_base = NULL;
	int fb_stride = 0;
	enum nsfb_format_e fb_fmt;
	int w = 0, h = 0;

	nsfb_get_geometry(fb, &w, &h, &fb_fmt);
	/* libnsfb provides both pointer and line length via nsfb_get_buffer */
	if (nsfb_get_buffer(fb, &fb_base, &fb_stride) != 0) {
		return;
	}
	if (fb_base == NULL) {
		return;
	}

	if (SDL_MUSTLOCK(dc_screen)) {
		if (SDL_LockSurface(dc_screen) != 0) {
			return;
		}
	}

	if (update_count < 3) {
		fprintf(stderr,
			"[dcdbg] SDL update %d: nsfb=%dx%d stride=%d fmt=%d, sdl=%dx%d pitch=%d bpp=%d\n",
			update_count, w, h, fb_stride, (int)fb_fmt,
			dc_screen->w, dc_screen->h, dc_screen->pitch,
			dc_screen->format->BitsPerPixel);
		update_count++;
	}

	if (sample_logged == 0) {
		/* Sample a few bytes so we can tell if the nsfb buffer is all-zero. */
		uint32_t sum = 0;
		int sample_len = 64;
		if (fb_stride > 0 && w > 0 && h > 0) {
			int max = fb_stride;
			if (max > sample_len)
				max = sample_len;
			for (int i = 0; i < max; i++) {
				sum += fb_base[i];
			}
			fprintf(stderr, "[dcdbg] nsfb sample sum=%lu first16=%02x %02x %02x %02x ...\n",
				(unsigned long)sum,
				fb_base[0], fb_base[1], fb_base[2], fb_base[3]);
			sample_logged = 1;
		}
	}

	/*
	 * Performance: dirty rectangle optimization
	 * Only copy the regions that actually changed instead of full screen
	 */
	{
		uint8_t *dst = (uint8_t *)dc_screen->pixels;
		int dst_pitch = dc_screen->pitch;
		int bytes_per_pixel = dc_screen->format->BitsPerPixel / 8;
		
		/* Check if we have a dirty rectangle set */
		if (dc_dirty_rect.dirty_set && dc_dirty_rect.active) {
			/* Copy only the dirty region */
			int copy_x = dc_dirty_rect.dirty.x0;
			int copy_y = dc_dirty_rect.dirty.y0;
			int copy_width = dc_dirty_rect.dirty.x1 - copy_x;
			int copy_height = dc_dirty_rect.dirty.y1 - copy_y;
			
			/* Bounds checking */
			if (copy_x < 0) copy_x = 0;
			if (copy_y < 0) copy_y = 0;
			if (copy_x + copy_width > w) copy_width = w - copy_x;
			if (copy_y + copy_height > h) copy_height = h - copy_y;
			if (copy_x + copy_width > dc_screen->w) copy_width = dc_screen->w - copy_x;
			if (copy_y + copy_height > h) copy_height = h - copy_y;
			
			if (copy_width > 0 && copy_height > 0) {
				int copy_bytes = copy_width * bytes_per_pixel;
				for (int y = 0; y < copy_height; y++) {
					int src_y = copy_y + y;
					int dst_y = copy_y + y;
					memcpy(dst + dst_y * dst_pitch + copy_x * bytes_per_pixel,
					       fb_base + src_y * fb_stride + copy_x * bytes_per_pixel,
					       (size_t)copy_bytes);
				}
				
				/* Update only the dirty region */
				SDL_UpdateRect(dc_screen, copy_x, copy_y, copy_width, copy_height);
			}
			dc_dirty_rect.dirty_set = false;
		} else {
			/* Fallback: full screen copy (original behavior) */
			int copy_width = (w < dc_screen->w) ? w : dc_screen->w;
			int copy_height = (h < dc_screen->h) ? h : dc_screen->h;
			int copy_bytes = copy_width * bytes_per_pixel;

			if (copy_bytes > fb_stride)
				copy_bytes = fb_stride;
			if (copy_bytes > dst_pitch)
				copy_bytes = dst_pitch;

			for (int y = 0; y < copy_height; y++) {
				memcpy(dst + y * dst_pitch, fb_base + y * fb_stride, (size_t)copy_bytes);
			}
		}
	}

	/* Composite cursor sprite (PoC behavior). */
	{
		nsfb_bbox_t loc;
		if (nsfb_cursor_loc_get(fb, &loc)) {
			dc_blit_cursor_rgba(dc_screen, loc.x0, loc.y0);
		}
	}

	if (SDL_MUSTLOCK(dc_screen)) {
		SDL_UnlockSurface(dc_screen);
	}

	/*
	 * Present frame.
	 * Performance: only use SDL_Flip when doing full screen updates,
	 * otherwise we already called SDL_UpdateRect for dirty regions.
	 */
	if (!dc_dirty_rect.active || !dc_dirty_rect.dirty_set) {
		/* Full screen update - use SDL_Flip */
		if (SDL_Flip(dc_screen) != 0) {
			fprintf(stderr, "[dcdbg] SDL_Flip failed: %s\n", SDL_GetError());
			SDL_UpdateRect(dc_screen, 0, 0, 0, 0);
		}
	}
	/* For dirty regions, SDL_UpdateRect was already called above */
}

void
dreamcast_sdl_set_cursor(const struct fbtk_bitmap *bm)
{
	if (bm == NULL || bm->pixdata == NULL || bm->width <= 0 || bm->height <= 0) {
		dc_cursor.valid = false;
		return;
	}

	/* fbtk_bitmap pixdata is RGBA8888 as generated by convert_image. */
	dc_cursor.rgba = bm->pixdata;
	dc_cursor.width = bm->width;
	dc_cursor.height = bm->height;
	dc_cursor.hot_x = bm->hot_x;
	dc_cursor.hot_y = bm->hot_y;
	dc_cursor.valid = true;
}

/* Performance: mark a region as dirty for partial updates */
void
dreamcast_sdl_mark_dirty(int x, int y, int width, int height)
{
	if (!dc_dirty_rect.active) {
		return;
	}

	if (!dc_dirty_rect.dirty_set) {
		/* First dirty rectangle */
		dc_dirty_rect.dirty.x0 = x;
		dc_dirty_rect.dirty.y0 = y;
		dc_dirty_rect.dirty.x1 = x + width;
		dc_dirty_rect.dirty.y1 = y + height;
		dc_dirty_rect.dirty_set = true;
	} else {
		/* Expand existing dirty rectangle to include new region */
		if (x < dc_dirty_rect.dirty.x0)
			dc_dirty_rect.dirty.x0 = x;
		if (y < dc_dirty_rect.dirty.y0)
			dc_dirty_rect.dirty.y0 = y;
		if (x + width > dc_dirty_rect.dirty.x1)
			dc_dirty_rect.dirty.x1 = x + width;
		if (y + height > dc_dirty_rect.dirty.y1)
			dc_dirty_rect.dirty.y1 = y + height;
	}
}

/* Performance: enable/disable dirty rectangle optimization */
void
dreamcast_sdl_set_dirty_optimization(bool enable)
{
	dc_dirty_rect.active = enable;
	dc_dirty_rect.dirty_set = false;
	if (enable) {
		fprintf(stderr, "[dcdbg] Dirty rectangle optimization enabled\n");
	} else {
		fprintf(stderr, "[dcdbg] Dirty rectangle optimization disabled\n");
	}
}

void
dreamcast_sdl_quit(void)
{
	if (dc_screen != NULL) {
		SDL_Quit();
		dc_screen = NULL;
	}
}
