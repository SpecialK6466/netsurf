/*
 * Dreamcast SDL surface bridge (SDL 1.2 via kos-ports)
 *
 * A small wrapper so the Dreamcast frontend can initialise SDL, present the
 * RAM libnsfb surface, and cache cursor sprites. Implementations are
 * Dreamcast-only and isolated to this frontend.
 */

#ifndef NETSURF_DREAMCAST_SDL_DC_SURFACE_H
#define NETSURF_DREAMCAST_SDL_DC_SURFACE_H

#include <stdbool.h>
#include <stdint.h>

#include <libnsfb.h>

/* Avoid including framebuffer/fbtk.h here (it relies on bbox_t typedefs
 * that are provided indirectly by other framebuffer headers). The Dreamcast
 * entry point only needs an opaque cursor bitmap type.
 */
struct fbtk_bitmap;

/* Initialise SDL video and create a window/surface. */
bool dreamcast_sdl_init(int width, int height, int bpp);

/* Blit the given nsfb RAM surface to the SDL surface. */
void dreamcast_sdl_update(nsfb_t *fb);

/* Cache a cursor bitmap (fbtk) for manual compositing if needed. */
void dreamcast_sdl_set_cursor(const struct fbtk_bitmap *bm);

/* Performance: mark a region as dirty for partial updates */
void dreamcast_sdl_mark_dirty(int x, int y, int width, int height);

/* Performance: enable/disable dirty rectangle optimization */
void dreamcast_sdl_set_dirty_optimization(bool enable);

/* Tear down SDL. Safe to call multiple times. */
void dreamcast_sdl_quit(void);

#endif /* NETSURF_DREAMCAST_SDL_DC_SURFACE_H */
