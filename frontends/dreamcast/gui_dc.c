/*
 * Copyright 2008, 2014 Vincent Sanders <vince@netsurf-browser.org>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#ifndef __DREAMCAST__
#include <dirent.h>
#endif
#include <sys/select.h>
#include <sys/time.h>
#ifndef __DREAMCAST__
#include <getopt.h>
#endif
#include <nsutils/time.h>

#include <libnsfb.h>
#include <libnsfb_plot.h>
#include <libnsfb_event.h>

#include "utils/utils.h"
#include "utils/nsoption.h"
#include "utils/filepath.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "netsurf/browser_window.h"
#include "netsurf/keypress.h"
#include "desktop/browser_history.h"
#include "netsurf/plotters.h"
#include "netsurf/window.h"
#include "netsurf/misc.h"
#include "netsurf/netsurf.h"
#include "netsurf/cookie_db.h"
#include "content/fetch.h"

#include "framebuffer/gui.h"
#include "framebuffer/fbtk.h"
#include "framebuffer/framebuffer.h"
#include "framebuffer/schedule.h"
#include "framebuffer/findfile.h"
#include "framebuffer/image_data.h"
#include "framebuffer/font.h"
#include "framebuffer/clipboard.h"
#include "framebuffer/fetch.h"
#include "framebuffer/bitmap.h"

#include "framebuffer/local_history.h"
#include "framebuffer/corewindow.h"
#include "desktop/browser_history.h"

#ifdef __DREAMCAST__
#include "dreamcast/sdl_dc_surface.h"
#include "dreamcast/settings.h"
#include <kos.h>
#include <dc/maple/keyboard.h>
#include <dc/maple/mouse.h>
#include <dc/video.h>
#endif

#define NSFB_TOOLBAR_DEFAULT_LAYOUT "blfsrutc"

fbtk_widget_t *fbtk;

#ifdef __DREAMCAST__
/* Dreamcast controller state for input mapping */
static struct {
	int joyx, joyy;
	uint32_t buttons;
} dc_input_prev = {0, 0, 0};

/* Dreamcast mouse state for input mapping */
static struct {
	uint32_t buttons;
} dc_mouse_prev = {0};

extern uint8 romdisk[];

/**
 * Ensure resource lookup prefers /rd/en.
 *
 * NetSurf's resource search path generation depends on LANG/LANGUAGE, which
 * are typically unset on Dreamcast. That causes lookups to hit /rd/Messages
 * (a stub in our romdisk) instead of /rd/en/Messages.
 */
static void
dreamcast_prepend_en_respath(void)
{
	char **old;
	int count = 0;
	char **newv;

	if (respaths == NULL)
		return;

	while (respaths[count] != NULL)
		count++;

	old = respaths;
	newv = calloc((size_t)count + 2, sizeof(char *));
	if (newv == NULL)
		return;

	newv[0] = strdup("/rd/en");
	if (newv[0] == NULL) {
		free(newv);
		return;
	}

	for (int i = 0; i < count; i++) {
		newv[i + 1] = old[i];
	}
	newv[count + 1] = NULL;

	/* Free old vector storage (strings are still owned by the new vector). */
	free(old);
	respaths = newv;
}

/* Forward declaration for input window (defined later in file) */
extern struct gui_window *input_window;

/* Poll Dreamcast controller and inject events into FBTK */
static void dreamcast_poll_input(void)
{
	maple_device_t *cont;
	cont_state_t *state;
	nsfb_event_t event;
	int dx = 0, dy = 0;

	/* Analog stick tuning parameters */
	const int deadzone = 24;      /* Ignore small movements (was 16) */
	const int max_speed = 12;     /* Max pixels per frame (was 6) */
	const int accel_threshold = 80; /* Start acceleration above this */

	cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
	if (!cont) return;

	state = (cont_state_t *)maple_dev_status(cont);
	if (!state) return;

	/* If settings menu is open, route input there first */
	if (dc_settings_is_open()) {
		if (dc_settings_input(state->buttons, dc_input_prev.buttons)) {
			dc_input_prev.buttons = state->buttons;
			dc_input_prev.joyx = state->joyx;
			dc_input_prev.joyy = state->joyy;
			return;
		}
	}

	/* Analog Stick -> Mouse Movement with deadzone and acceleration */
	if (abs(state->joyx) > deadzone) {
		int magnitude = abs(state->joyx) - deadzone;
		int sign = (state->joyx > 0) ? 1 : -1;
		/* Apply acceleration curve for faster movement at full tilt */
		if (abs(state->joyx) > accel_threshold) {
			/* Quadratic acceleration for high tilt */
			dx = sign * ((magnitude * magnitude * max_speed) / (128 * 128));
			if (dx == 0) dx = sign; /* Minimum 1 pixel */
		} else {
			/* Linear for small movements */
			dx = sign * ((magnitude * max_speed / 2) / 128);
			if (dx == 0 && magnitude > 0) dx = sign;
		}
	}
	if (abs(state->joyy) > deadzone) {
		int magnitude = abs(state->joyy) - deadzone;
		int sign = (state->joyy > 0) ? 1 : -1;
		if (abs(state->joyy) > accel_threshold) {
			dy = sign * ((magnitude * magnitude * max_speed) / (128 * 128));
			if (dy == 0) dy = sign;
		} else {
			dy = sign * ((magnitude * max_speed / 2) / 128);
			if (dy == 0 && magnitude > 0) dy = sign;
		}
	}

	if (dx || dy) {
		fbtk_warp_pointer(fbtk, dx, dy, true);
	}

	/* Buttons -> Mouse Clicks / Keys */
	uint32_t changed = state->buttons ^ dc_input_prev.buttons;
	uint32_t pressed = changed & state->buttons;

	/* Button A -> Left Click */
	if (changed & CONT_A) {
		event.type = (pressed & CONT_A) ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
		event.value.keycode = NSFB_KEY_MOUSE_1;
		fbtk_click(fbtk, &event);
	}

	/* Button B -> Right Click (Back/Menu context) */
	if (changed & CONT_B) {
		event.type = (pressed & CONT_B) ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
		event.value.keycode = NSFB_KEY_MOUSE_3;
		fbtk_click(fbtk, &event);
	}

	/* D-Pad -> Scrolling (Arrow keys) - disabled when OSK is open */
	if (!is_osk_visible()) {
		if (changed & CONT_DPAD_UP) {
			event.type = (pressed & CONT_DPAD_UP) ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
			event.value.keycode = NSFB_KEY_UP;
			fbtk_input(fbtk, &event);
		}
		if (changed & CONT_DPAD_DOWN) {
			event.type = (pressed & CONT_DPAD_DOWN) ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
			event.value.keycode = NSFB_KEY_DOWN;
			fbtk_input(fbtk, &event);
		}
		if (changed & CONT_DPAD_LEFT) {
			event.type = (pressed & CONT_DPAD_LEFT) ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
			event.value.keycode = NSFB_KEY_LEFT;
			fbtk_input(fbtk, &event);
		}
		if (changed & CONT_DPAD_RIGHT) {
			event.type = (pressed & CONT_DPAD_RIGHT) ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
			event.value.keycode = NSFB_KEY_RIGHT;
			fbtk_input(fbtk, &event);
		}
	}
    
	/* Start -> Toggle Settings Menu */
	if ((changed & CONT_START) && (pressed & CONT_START)) {
		dc_settings_toggle();
	}

	/* Y button -> Toggle On-Screen Keyboard */
	if ((changed & CONT_Y) && (pressed & CONT_Y)) {
		toggle_osk();
	}

	/* X button -> History Back */
	if ((changed & CONT_X) && (pressed & CONT_X)) {
		if (input_window && input_window->bw) {
			browser_window_history_back(input_window->bw, false);
		}
	}

	/* Analog triggers for navigation (threshold ~50% = 128) */
	{
		static bool ltrig_was_pressed = false;
		static bool rtrig_was_pressed = false;
		bool ltrig_pressed = (state->ltrig > 128);
		bool rtrig_pressed = (state->rtrig > 128);

		/* L+R triggers together -> Reload page */
		if (ltrig_pressed && rtrig_pressed) {
			if (!ltrig_was_pressed || !rtrig_was_pressed) {
				if (input_window && input_window->bw) {
					browser_window_reload(input_window->bw, false);
				}
			}
		}
		/* L trigger alone -> History Forward */
		else if (ltrig_pressed && !rtrig_pressed) {
			if (!ltrig_was_pressed) {
				if (input_window && input_window->bw) {
					browser_window_history_forward(input_window->bw, false);
				}
			}
		}
		/* R trigger alone -> History Forward (alternative to L trigger) */
		else if (rtrig_pressed && !ltrig_pressed) {
			if (!rtrig_was_pressed) {
				if (input_window && input_window->bw) {
					browser_window_history_forward(input_window->bw, false);
				}
			}
		}

		ltrig_was_pressed = ltrig_pressed;
		rtrig_was_pressed = rtrig_pressed;
	}

	dc_input_prev.buttons = state->buttons;
	dc_input_prev.joyx = state->joyx;
	dc_input_prev.joyy = state->joyy;
}

/**
 * Map KOS keyboard key codes to libnsfb key codes.
 *
 * KOS uses USB HID key codes. This provides a basic mapping for common keys.
 */
static enum nsfb_key_code_e
dreamcast_map_kbd_key(kbd_key_t key, kbd_mods_t mods)
{
	/* Alphanumeric keys (USB HID codes 0x04-0x1D = a-z) */
	if (key >= 0x04 && key <= 0x1D) {
		return (enum nsfb_key_code_e)(NSFB_KEY_a + (key - 0x04));
	}

	/* Number keys 1-9,0 (USB HID codes 0x1E-0x27) */
	if (key >= 0x1E && key <= 0x26) {
		return (enum nsfb_key_code_e)(NSFB_KEY_1 + (key - 0x1E));
	}
	if (key == 0x27) {
		return NSFB_KEY_0;
	}

	/* Function keys F1-F12 (USB HID codes 0x3A-0x45) */
	if (key >= 0x3A && key <= 0x45) {
		return (enum nsfb_key_code_e)(NSFB_KEY_F1 + (key - 0x3A));
	}

	/* Special keys */
	switch (key) {
	case 0x28: return NSFB_KEY_RETURN;      /* Enter */
	case 0x29: return NSFB_KEY_ESCAPE;      /* Escape */
	case 0x2A: return NSFB_KEY_BACKSPACE;   /* Backspace */
	case 0x2B: return NSFB_KEY_TAB;         /* Tab */
	case 0x2C: return NSFB_KEY_SPACE;       /* Space */
	case 0x2D: return NSFB_KEY_MINUS;       /* - */
	case 0x2E: return NSFB_KEY_EQUALS;      /* = */
	case 0x2F: return NSFB_KEY_LEFTBRACKET; /* [ */
	case 0x30: return NSFB_KEY_RIGHTBRACKET;/* ] */
	case 0x31: return NSFB_KEY_BACKSLASH;   /* \ */
	case 0x33: return NSFB_KEY_SEMICOLON;   /* ; */
	case 0x34: return NSFB_KEY_QUOTE;       /* ' */
	case 0x35: return NSFB_KEY_BACKQUOTE;   /* ` */
	case 0x36: return NSFB_KEY_COMMA;       /* , */
	case 0x37: return NSFB_KEY_PERIOD;      /* . */
	case 0x38: return NSFB_KEY_SLASH;       /* / */
	case 0x39: return NSFB_KEY_CAPSLOCK;    /* Caps Lock */
	case 0x49: return NSFB_KEY_INSERT;      /* Insert */
	case 0x4A: return NSFB_KEY_HOME;        /* Home */
	case 0x4B: return NSFB_KEY_PAGEUP;      /* Page Up */
	case 0x4C: return NSFB_KEY_DELETE;      /* Delete */
	case 0x4D: return NSFB_KEY_END;         /* End */
	case 0x4E: return NSFB_KEY_PAGEDOWN;    /* Page Down */
	case 0x4F: return NSFB_KEY_RIGHT;       /* Right Arrow */
	case 0x50: return NSFB_KEY_LEFT;        /* Left Arrow */
	case 0x51: return NSFB_KEY_DOWN;        /* Down Arrow */
	case 0x52: return NSFB_KEY_UP;          /* Up Arrow */
	case 0xE0: return NSFB_KEY_LCTRL;       /* Left Control */
	case 0xE1: return NSFB_KEY_LSHIFT;      /* Left Shift */
	case 0xE2: return NSFB_KEY_LALT;        /* Left Alt */
	case 0xE4: return NSFB_KEY_RCTRL;       /* Right Control */
	case 0xE5: return NSFB_KEY_RSHIFT;      /* Right Shift */
	case 0xE6: return NSFB_KEY_RALT;        /* Right Alt */
	default:
		return NSFB_KEY_UNKNOWN;
	}
}

/**
 * Poll Dreamcast keyboard and inject events into FBTK.
 *
 * Uses the KOS keyboard queue to get key press events with proper translation.
 */
static void dreamcast_poll_keyboard(void)
{
	maple_device_t *kbd;
	int key;
	nsfb_event_t event;

	kbd = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
	if (!kbd) return;

	/* Pop keys from the keyboard queue until empty */
	while ((key = kbd_queue_pop(kbd, 1)) != KBD_QUEUE_END) {
		/*
		 * When xlat=1, kbd_queue_pop returns:
		 * - ASCII value for printable characters
		 * - Raw keycode << 8 for non-printable keys
		 */
		if (key < 256) {
			/* Printable ASCII character - inject as keypress */
			event.type = NSFB_EVENT_KEY_DOWN;

			/* Map ASCII to NSFB key codes */
			if (key >= 'a' && key <= 'z') {
				event.value.keycode = (enum nsfb_key_code_e)(NSFB_KEY_a + (key - 'a'));
			} else if (key >= 'A' && key <= 'Z') {
				/* Uppercase: same key code, but fbtk_input handles shift */
				event.value.keycode = (enum nsfb_key_code_e)(NSFB_KEY_a + (key - 'A'));
			} else if (key >= '0' && key <= '9') {
				event.value.keycode = (enum nsfb_key_code_e)(NSFB_KEY_0 + (key - '0'));
			} else {
				/* Map common punctuation */
				switch (key) {
				case ' ':  event.value.keycode = NSFB_KEY_SPACE; break;
				case '\n': event.value.keycode = NSFB_KEY_RETURN; break;
				case '\r': event.value.keycode = NSFB_KEY_RETURN; break;
				case '\t': event.value.keycode = NSFB_KEY_TAB; break;
				case '\b': event.value.keycode = NSFB_KEY_BACKSPACE; break;
				case '-':  event.value.keycode = NSFB_KEY_MINUS; break;
				case '=':  event.value.keycode = NSFB_KEY_EQUALS; break;
				case '[':  event.value.keycode = NSFB_KEY_LEFTBRACKET; break;
				case ']':  event.value.keycode = NSFB_KEY_RIGHTBRACKET; break;
				case '\\': event.value.keycode = NSFB_KEY_BACKSLASH; break;
				case ';':  event.value.keycode = NSFB_KEY_SEMICOLON; break;
				case '\'': event.value.keycode = NSFB_KEY_QUOTE; break;
				case '`':  event.value.keycode = NSFB_KEY_BACKQUOTE; break;
				case ',':  event.value.keycode = NSFB_KEY_COMMA; break;
				case '.':  event.value.keycode = NSFB_KEY_PERIOD; break;
				case '/':  event.value.keycode = NSFB_KEY_SLASH; break;
				default:
					/* For other characters, try to pass through */
					event.value.keycode = NSFB_KEY_UNKNOWN;
					break;
				}
			}

			if (event.value.keycode != NSFB_KEY_UNKNOWN) {
				fbtk_input(fbtk, &event);
				/* Also send key up immediately for queue-based input */
				event.type = NSFB_EVENT_KEY_UP;
				fbtk_input(fbtk, &event);
			}
		} else {
			/* Non-printable key: raw keycode in upper bits */
			int rawkey = key >> 8;
			kbd_mods_t mods = {0};
			enum nsfb_key_code_e nsfb_key = dreamcast_map_kbd_key((kbd_key_t)rawkey, mods);

			if (nsfb_key != NSFB_KEY_UNKNOWN) {
				event.type = NSFB_EVENT_KEY_DOWN;
				event.value.keycode = nsfb_key;
				fbtk_input(fbtk, &event);
				event.type = NSFB_EVENT_KEY_UP;
				fbtk_input(fbtk, &event);
			}
		}
	}
}

/**
 * Poll Dreamcast mouse and inject events into FBTK.
 */
static void dreamcast_poll_mouse(void)
{
	maple_device_t *mouse;
	mouse_state_t *state;
	nsfb_event_t event;

	mouse = maple_enum_type(0, MAPLE_FUNC_MOUSE);
	if (!mouse) return;

	state = (mouse_state_t *)maple_dev_status(mouse);
	if (!state) return;

	/* Mouse movement -> pointer warp */
	if (state->dx != 0 || state->dy != 0) {
		fbtk_warp_pointer(fbtk, state->dx, state->dy, true);
	}

	/* Mouse buttons */
	uint32_t changed = state->buttons ^ dc_mouse_prev.buttons;
	uint32_t pressed = changed & state->buttons;

	/* Left button */
	if (changed & MOUSE_LEFTBUTTON) {
		event.type = (pressed & MOUSE_LEFTBUTTON) ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
		event.value.keycode = NSFB_KEY_MOUSE_1;
		fbtk_click(fbtk, &event);
	}

	/* Right button */
	if (changed & MOUSE_RIGHTBUTTON) {
		event.type = (pressed & MOUSE_RIGHTBUTTON) ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
		event.value.keycode = NSFB_KEY_MOUSE_3;
		fbtk_click(fbtk, &event);
	}

	/* Side button -> middle click */
	if (changed & MOUSE_SIDEBUTTON) {
		event.type = (pressed & MOUSE_SIDEBUTTON) ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
		event.value.keycode = NSFB_KEY_MOUSE_2;
		fbtk_click(fbtk, &event);
	}

	/* Scroll wheel (dz) -> scroll up/down */
	if (state->dz != 0) {
		event.type = NSFB_EVENT_KEY_DOWN;
		event.value.keycode = (state->dz > 0) ? NSFB_KEY_MOUSE_4 : NSFB_KEY_MOUSE_5;
		fbtk_click(fbtk, &event);
		event.type = NSFB_EVENT_KEY_UP;
		fbtk_click(fbtk, &event);
	}

	dc_mouse_prev.buttons = state->buttons;
}

/**
 * Apply Dreamcast-specific memory and cache tuning.
 *
 * The Dreamcast has only 16MB of RAM, so we must use conservative cache sizes
 * to prevent out-of-memory conditions during page loads.
 */
static void dreamcast_apply_memory_tuning(void)
{
	/* Disable disc cache entirely - Dreamcast has no writable disk */
	nsoption_set_uint(disc_cache_size, 0);

	/* Reduce memory cache from default 12MB to 2MB */
	nsoption_set_int(memory_cache_size, 2 * 1024 * 1024);

	/* Disable background image loading to save memory */
	nsoption_set_bool(background_images, false);

	fprintf(stderr, "[dc] Memory tuning applied: disc_cache=0, memory_cache=2MB\n");
}

/* Video cable type detected at startup */
static int8_t dc_cable_type = -1;

/**
 * Detect video cable type and apply display settings.
 *
 * Adjusts font sizes and other display parameters based on the connected
 * video cable. VGA users get standard sizes, while composite/RF users get
 * larger fonts for better readability on low-resolution displays.
 */
static void dreamcast_detect_video_cable(void)
{
	dc_cable_type = vid_check_cable();

	const char *cable_name;
	switch (dc_cable_type) {
	case CT_VGA:
		cable_name = "VGA";
		/* VGA: high quality, use standard font sizes */
		nsoption_set_int(font_size, 128);      /* 12.8pt */
		nsoption_set_int(font_min_size, 85);   /* 8.5pt */
		break;
	case CT_RGB:
		cable_name = "RGB/SCART";
		/* RGB: good quality, slightly larger fonts */
		nsoption_set_int(font_size, 140);      /* 14.0pt */
		nsoption_set_int(font_min_size, 100);  /* 10.0pt */
		break;
	case CT_COMPOSITE:
		cable_name = "Composite/S-Video";
		/* Composite: low quality, larger fonts for readability */
		nsoption_set_int(font_size, 160);      /* 16.0pt */
		nsoption_set_int(font_min_size, 120);  /* 12.0pt */
		break;
	case CT_NONE:
	default:
		cable_name = "Unknown/None";
		/* Default to composite-like settings for safety */
		nsoption_set_int(font_size, 160);      /* 16.0pt */
		nsoption_set_int(font_min_size, 120);  /* 12.0pt */
		break;
	}

	fprintf(stderr, "[dc] Video cable detected: %s (type=%d)\n",
		cable_name, dc_cable_type);
	fprintf(stderr, "[dc] Font settings: size=%d, min_size=%d\n",
		nsoption_int(font_size), nsoption_int(font_min_size));
}
#endif

static bool fb_complete = false;

struct gui_window *input_window = NULL;
struct gui_window *search_current_window;
struct gui_window *window_list = NULL;

/* private data for browser user widget */
struct browser_widget_s {
	struct browser_window *bw; /**< The browser window connected to this gui window */
	int scrollx, scrolly; /**< scroll offsets. */

	/* Pending window redraw state. */
	bool redraw_required; /**< flag indicating the foreground loop
			       * needs to redraw the browser widget.
			       */
	bbox_t redraw_box; /**< Area requiring redraw. */
	bool pan_required; /**< flag indicating the foreground loop
			    * needs to pan the window.
			    */
	int panx, pany; /**< Panning required. */
};

static struct gui_drag {
	enum state {
		GUI_DRAG_NONE,
		GUI_DRAG_PRESSED,
		GUI_DRAG_DRAG
	} state;
	int button;
	int x;
	int y;
	bool grabbed_pointer;
} gui_drag;


/**
 * Cause an abnormal program termination.
 *
 * \note This never returns and is intended to terminate without any cleanup.
 *
 * \param error The message to display to the user.
 */
static void die(const char *error)
{
	fprintf(stderr, "%s\n", error);
	exit(1);
}


/**
 * Warn the user of an event.
 *
 * \param[in] warning A warning looked up in the message translation table
 * \param[in] detail Additional text to be displayed or NULL.
 * \return NSERROR_OK on success or error code if there was a
 *           faliure displaying the message to the user.
 */
static nserror fb_warn_user(const char *warning, const char *detail)
{
	NSLOG(netsurf, INFO, "%s %s", warning, detail);
	return NSERROR_OK;
}

/* queue a redraw operation, co-ordinates are relative to the window */
static void
fb_queue_redraw(struct fbtk_widget_s *widget, int x0, int y0, int x1, int y1)
{
	struct browser_widget_s *bwidget = fbtk_get_userpw(widget);

	bwidget->redraw_box.x0 = min(bwidget->redraw_box.x0, x0);
	bwidget->redraw_box.y0 = min(bwidget->redraw_box.y0, y0);
	bwidget->redraw_box.x1 = max(bwidget->redraw_box.x1, x1);
	bwidget->redraw_box.y1 = max(bwidget->redraw_box.y1, y1);

	if (fbtk_clip_to_widget(widget, &bwidget->redraw_box)) {
		bwidget->redraw_required = true;
		fbtk_request_redraw(widget);
	} else {
		bwidget->redraw_box.y0 = bwidget->redraw_box.x0 = INT_MAX;
		bwidget->redraw_box.y1 = bwidget->redraw_box.x1 = -(INT_MAX);
		bwidget->redraw_required = false;
	}
}

/* queue a window scroll */
static void
widget_scroll_y(struct gui_window *gw, int y, bool abs)
{
	struct browser_widget_s *bwidget = fbtk_get_userpw(gw->browser);
	int content_width, content_height;
	int height;

	NSLOG(netsurf, DEEPDEBUG, "window scroll");
	if (abs) {
		bwidget->pany = y - bwidget->scrolly;
	} else {
		bwidget->pany += y;
	}

	browser_window_get_extents(gw->bw, true,
			&content_width, &content_height);

	height = fbtk_get_height(gw->browser);

	/* dont pan off the top */
	if ((bwidget->scrolly + bwidget->pany) < 0)
		bwidget->pany = -bwidget->scrolly;

	/* do not pan off the bottom of the content */
	if ((bwidget->scrolly + bwidget->pany) > (content_height - height))
		bwidget->pany = (content_height - height) - bwidget->scrolly;

	if (bwidget->pany == 0)
		return;

	bwidget->pan_required = true;

	fbtk_request_redraw(gw->browser);

	fbtk_set_scroll_position(gw->vscroll, bwidget->scrolly + bwidget->pany);
}

/* queue a window scroll */
static void
widget_scroll_x(struct gui_window *gw, int x, bool abs)
{
	struct browser_widget_s *bwidget = fbtk_get_userpw(gw->browser);
	int content_width, content_height;
	int width;

	if (abs) {
		bwidget->panx = x - bwidget->scrollx;
	} else {
		bwidget->panx += x;
	}

	browser_window_get_extents(gw->bw, true,
			&content_width, &content_height);

	width = fbtk_get_width(gw->browser);

	/* dont pan off the left */
	if ((bwidget->scrollx + bwidget->panx) < 0)
		bwidget->panx = - bwidget->scrollx;

	/* do not pan off the right of the content */
	if ((bwidget->scrollx + bwidget->panx) > (content_width - width))
		bwidget->panx = (content_width - width) - bwidget->scrollx;

	if (bwidget->panx == 0)
		return;

	bwidget->pan_required = true;

	fbtk_request_redraw(gw->browser);

	fbtk_set_scroll_position(gw->hscroll, bwidget->scrollx + bwidget->panx);
}

static void
fb_pan(fbtk_widget_t *widget,
       struct browser_widget_s *bwidget,
       struct browser_window *bw)
{
	int x;
	int y;
	int width;
	int height;
	nsfb_bbox_t srcbox;
	nsfb_bbox_t dstbox;

	nsfb_t *nsfb = fbtk_get_nsfb(widget);

	height = fbtk_get_height(widget);
	width = fbtk_get_width(widget);

	NSLOG(netsurf, DEEPDEBUG, "panning %d, %d",
			bwidget->panx, bwidget->pany);

	x = fbtk_get_absx(widget);
	y = fbtk_get_absy(widget);

	/* if the pan exceeds the viewport size just redraw the whole area */
	if (bwidget->pany >= height || bwidget->pany <= -height ||
	    bwidget->panx >= width || bwidget->panx <= -width) {

		bwidget->scrolly += bwidget->pany;
		bwidget->scrollx += bwidget->panx;
		fb_queue_redraw(widget, 0, 0, width, height);

		/* ensure we don't try to scroll again */
		bwidget->panx = 0;
		bwidget->pany = 0;
		bwidget->pan_required = false;
		return;
	}

	if (bwidget->pany < 0) {
		/* pan up by less then viewport height */
		srcbox.x0 = x;
		srcbox.y0 = y;
		srcbox.x1 = srcbox.x0 + width;
		srcbox.y1 = srcbox.y0 + height + bwidget->pany;

		dstbox.x0 = x;
		dstbox.y0 = y - bwidget->pany;
		dstbox.x1 = dstbox.x0 + width;
		dstbox.y1 = dstbox.y0 + height + bwidget->pany;

		/* move part that remains visible up */
		nsfb_plot_copy(nsfb, &srcbox, nsfb, &dstbox);

		/* redraw newly exposed area */
		bwidget->scrolly += bwidget->pany;
		fb_queue_redraw(widget, 0, 0, width, - bwidget->pany);

	} else if (bwidget->pany > 0) {
		/* pan down by less then viewport height */
		srcbox.x0 = x;
		srcbox.y0 = y + bwidget->pany;
		srcbox.x1 = srcbox.x0 + width;
		srcbox.y1 = srcbox.y0 + height - bwidget->pany;

		dstbox.x0 = x;
		dstbox.y0 = y;
		dstbox.x1 = dstbox.x0 + width;
		dstbox.y1 = dstbox.y0 + height - bwidget->pany;

		/* move part that remains visible down */
		nsfb_plot_copy(nsfb, &srcbox, nsfb, &dstbox);

		/* redraw newly exposed area */
		bwidget->scrolly += bwidget->pany;
		fb_queue_redraw(widget, 0, height - bwidget->pany,
				width, height);
	}

	if (bwidget->panx < 0) {
		/* pan left by less then viewport width */
		srcbox.x0 = x;
		srcbox.y0 = y;
		srcbox.x1 = srcbox.x0 + width + bwidget->panx;
		srcbox.y1 = srcbox.y0 + height;

		dstbox.x0 = x - bwidget->panx;
		dstbox.y0 = y;
		dstbox.x1 = dstbox.x0 + width + bwidget->panx;
		dstbox.y1 = dstbox.y0 + height;

		/* move part that remains visible left */
		nsfb_plot_copy(nsfb, &srcbox, nsfb, &dstbox);

		/* redraw newly exposed area */
		bwidget->scrollx += bwidget->panx;
		fb_queue_redraw(widget, 0, 0, -bwidget->panx, height);

	} else if (bwidget->panx > 0) {
		/* pan right by less then viewport width */
		srcbox.x0 = x + bwidget->panx;
		srcbox.y0 = y;
		srcbox.x1 = srcbox.x0 + width - bwidget->panx;
		srcbox.y1 = srcbox.y0 + height;

		dstbox.x0 = x;
		dstbox.y0 = y;
		dstbox.x1 = dstbox.x0 + width - bwidget->panx;
		dstbox.y1 = dstbox.y0 + height;

		/* move part that remains visible right */
		nsfb_plot_copy(nsfb, &srcbox, nsfb, &dstbox);

		/* redraw newly exposed area */
		bwidget->scrollx += bwidget->panx;
		fb_queue_redraw(widget, width - bwidget->panx, 0,
				width, height);
	}

	bwidget->pan_required = false;
	bwidget->panx = 0;
	bwidget->pany = 0;
}

static void
fb_redraw(fbtk_widget_t *widget,
	  struct browser_widget_s *bwidget,
	  struct browser_window *bw)
{
	int x;
	int y;
	int caret_x, caret_y, caret_h;
	struct rect clip;
	struct redraw_context ctx = {
		.interactive = true,
		.background_images = true,
		.plot = &fb_plotters
	};
	nsfb_t *nsfb = fbtk_get_nsfb(widget);

	x = fbtk_get_absx(widget);
	y = fbtk_get_absy(widget);

	/* adjust clipping co-ordinates according to window location */
	bwidget->redraw_box.y0 += y;
	bwidget->redraw_box.y1 += y;
	bwidget->redraw_box.x0 += x;
	bwidget->redraw_box.x1 += x;

	nsfb_claim(nsfb, &bwidget->redraw_box);

	/* redraw bounding box is relative to window */
	clip.x0 = bwidget->redraw_box.x0;
	clip.y0 = bwidget->redraw_box.y0;
	clip.x1 = bwidget->redraw_box.x1;
	clip.y1 = bwidget->redraw_box.y1;

	browser_window_redraw(bw,
			x - bwidget->scrollx,
			y - bwidget->scrolly,
			&clip, &ctx);

	if (fbtk_get_caret(widget, &caret_x, &caret_y, &caret_h)) {
		/* This widget has caret, so render it */
		nsfb_bbox_t line;
		nsfb_plot_pen_t pen;

		line.x0 = x - bwidget->scrollx + caret_x;
		line.y0 = y - bwidget->scrolly + caret_y;
		line.x1 = x - bwidget->scrollx + caret_x;
		line.y1 = y - bwidget->scrolly + caret_y + caret_h;

		pen.stroke_type = NFSB_PLOT_OPTYPE_SOLID;
		pen.stroke_width = 1;
		pen.stroke_colour = 0xFF0000FF;

		nsfb_plot_line(nsfb, &line, &pen);
	}

	nsfb_update(fbtk_get_nsfb(widget), &bwidget->redraw_box);

	bwidget->redraw_box.y0 = bwidget->redraw_box.x0 = INT_MAX;
	bwidget->redraw_box.y1 = bwidget->redraw_box.x1 = INT_MIN;
	bwidget->redraw_required = false;
}

static int
fb_browser_window_redraw(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct gui_window *gw = cbi->context;
	struct browser_widget_s *bwidget;

	bwidget = fbtk_get_userpw(widget);
	if (bwidget == NULL) {
		NSLOG(netsurf, INFO,
		      "browser widget from widget %p was null", widget);
		return -1;
	}

	if (bwidget->pan_required) {
		fb_pan(widget, bwidget, gw->bw);
	}

	if (bwidget->redraw_required) {
		/* Performance: mark dirty rectangle for partial update */
		int widget_x = fbtk_get_absx(widget);
		int widget_y = fbtk_get_absy(widget);
		dreamcast_sdl_mark_dirty(widget_x + bwidget->redraw_box.x0,
					 widget_y + bwidget->redraw_box.y0,
					 bwidget->redraw_box.x1 - bwidget->redraw_box.x0,
					 bwidget->redraw_box.y1 - bwidget->redraw_box.y0);
		fb_redraw(widget, bwidget, gw->bw);
	} else {
		/* Full redraw - mark entire widget as dirty */
		int widget_x = fbtk_get_absx(widget);
		int widget_y = fbtk_get_absy(widget);
		int widget_width = fbtk_get_width(widget);
		int widget_height = fbtk_get_height(widget);
		
		dreamcast_sdl_mark_dirty(widget_x, widget_y, widget_width, widget_height);
		
		bwidget->redraw_box.x0 = 0;
		bwidget->redraw_box.y0 = 0;
		bwidget->redraw_box.x1 = widget_width;
		bwidget->redraw_box.y1 = widget_height;
		fb_redraw(widget, bwidget, gw->bw);
	}
	return 0;
}

static int fb_browser_window_destroy(fbtk_widget_t *widget,
		fbtk_callback_info *cbi)
{
	struct browser_widget_s *browser_widget;

	if (widget == NULL) {
		return 0;
	}

	/* Free private data */
	browser_widget = fbtk_get_userpw(widget);
	free(browser_widget);

	return 0;
}

static void
framebuffer_surface_iterator(void *ctx, const char *name, enum nsfb_type_e type)
{
	const char *arg0 = ctx;

	fprintf(stderr, "%s: %s\n", arg0, name);
}

static enum nsfb_type_e fetype = NSFB_SURFACE_COUNT;
static const char *fename;
static int febpp;
static int fewidth;
static int feheight;
static const char *feurl;

static void
framebuffer_pick_default_fename(void *ctx, const char *name, enum nsfb_type_e type)
{
	if (type < fetype) {
		fename = name;
	}
}

static bool
process_cmdline(int argc, char** argv)
{
	int opt;
	int option_index;
	#ifndef __DREAMCAST__
	static struct option long_options[] = {
		{0, 0, 0,  0 }
	}; /* no long options */
	#endif

	NSLOG(netsurf, INFO, "argc %d, argv %p", argc, argv);

	nsfb_enumerate_surface_types(framebuffer_pick_default_fename, NULL);

	febpp = 32;

#ifdef __DREAMCAST__
	/* Dreamcast port: force RAM surface + 16bpp and a conservative default size. */
	fename = "ram";
	febpp = 16;
	fewidth = 640;
	feheight = 480;
#endif

	fewidth = nsoption_int(window_width);
	if (fewidth <= 0) {
		fewidth = 800;
	}
	feheight = nsoption_int(window_height);
	if (feheight <= 0) {
		feheight = 600;
	}

#ifdef __DREAMCAST__
	/* Dreamcast port: enforce RAM surface + 16bpp + fixed 640x480 geometry. */
	fename = "ram";
	febpp = 16;
	fewidth = 640;
	feheight = 480;
#endif

	if ((nsoption_charp(homepage_url) != NULL) && 
	    (nsoption_charp(homepage_url)[0] != '\0')) {
		feurl = nsoption_charp(homepage_url);
	} else {
		feurl = NETSURF_HOMEPAGE;
	}

	#ifdef __DREAMCAST__
	/* KOS/newlib does not generally provide getopt_long(). */
	while ((opt = getopt(argc, argv, "f:b:w:h:")) != -1) {
	#else
	while((opt = getopt_long(argc, argv, "f:b:w:h:",
				 long_options, &option_index)) != -1) {
	#endif
		switch (opt) {
		case 'f':
			fename = optarg;
			break;

		case 'b':
			febpp = atoi(optarg);
			break;

		case 'w':
			fewidth = atoi(optarg);
			break;

		case 'h':
			feheight = atoi(optarg);
			break;

		default:
			fprintf(stderr,
				"Usage: %s [-f frontend] [-b bpp] [-w width] [-h height] <url>\n",
				argv[0]);
			return false;
		}
	}

	if (optind < argc) {
		feurl = argv[optind];
	}

#ifdef __DREAMCAST__
	/* Enforce Dreamcast-required surface/format regardless of command line. */
	fename = "ram";
	febpp = 16;
	fewidth = 640;
	feheight = 480;
#endif

	if (nsfb_type_from_name(fename) == NSFB_SURFACE_NONE) {
		if (strcmp(fename, "?") != 0) {
			fprintf(stderr,
				"%s: Unknown surface `%s`\n", argv[0], fename);
		}
		fprintf(stderr, "%s: Valid surface names are:\n", argv[0]);
		nsfb_enumerate_surface_types(framebuffer_surface_iterator, argv[0]);
		return false;
	}

	return true;
}

/**
 * Set option defaults for framebuffer frontend
 *
 * @param defaults The option table to update.
 * @return error status.
 */
static nserror set_defaults(struct nsoption_s *defaults)
{
	int idx;
	static const struct {
		enum nsoption_e nsc;
		colour c;
	} sys_colour_defaults[]= {
		{ NSOPTION_sys_colour_AccentColor, 0x00666666},
		{ NSOPTION_sys_colour_AccentColorText, 0x00ffffff},
		{ NSOPTION_sys_colour_ActiveText, 0x000000ee},
		{ NSOPTION_sys_colour_ButtonBorder, 0x00aaaaaa},
		{ NSOPTION_sys_colour_ButtonFace, 0x00dddddd},
		{ NSOPTION_sys_colour_ButtonText, 0x00000000},
		{ NSOPTION_sys_colour_Canvas, 0x00aaaaaa},
		{ NSOPTION_sys_colour_CanvasText, 0x00000000},
		{ NSOPTION_sys_colour_Field, 0x00f1f1f1},
		{ NSOPTION_sys_colour_FieldText, 0x00000000},
		{ NSOPTION_sys_colour_GrayText, 0x00777777},
		{ NSOPTION_sys_colour_Highlight, 0x00ee0000},
		{ NSOPTION_sys_colour_HighlightText, 0x00000000},
		{ NSOPTION_sys_colour_LinkText, 0x00ee0000},
		{ NSOPTION_sys_colour_Mark, 0x0000ffff},
		{ NSOPTION_sys_colour_MarkText, 0x00000000},
		{ NSOPTION_sys_colour_SelectedItem, 0x00e48435},
		{ NSOPTION_sys_colour_SelectedItemText, 0x00ffffff},
		{ NSOPTION_sys_colour_VisitedText, 0x008b1a55},
		{ NSOPTION_LISTEND, 0},
	};

	/* Set defaults for absent option strings */
	nsoption_setnull_charp(cookie_file, strdup("~/.netsurf/Cookies"));
	nsoption_setnull_charp(cookie_jar, strdup("~/.netsurf/Cookies"));

	if (nsoption_charp(cookie_file) == NULL ||
	    nsoption_charp(cookie_jar) == NULL) {
		NSLOG(netsurf, INFO, "Failed initialising cookie options");
		return NSERROR_BAD_PARAMETER;
	}

	/* set system colours for framebuffer ui */
	for (idx=0; sys_colour_defaults[idx].nsc != NSOPTION_LISTEND; idx++) {
		defaults[sys_colour_defaults[idx].nsc].value.c = sys_colour_defaults[idx].c;
	}
	return NSERROR_OK;
}


/**
 * Ensures output logging stream is correctly configured
 */
static bool nslog_stream_configure(FILE *fptr)
{
        /* set log stream to be non-buffering */
	setbuf(fptr, NULL);

	return true;
}

static void framebuffer_run(void)
{
	nsfb_event_t event;
	int timeout; /* timeout in miliseconds */

	while (fb_complete != true) {
		/* run the scheduler and discover how long to wait for
		 * the next event.
		 */
		timeout = schedule_run();

#ifdef __DREAMCAST__
		/* Poll all Dreamcast input devices */
		dreamcast_poll_input();
		dreamcast_poll_keyboard();
		dreamcast_poll_mouse();
		/* Cap timeout to ensure responsiveness */
		if (timeout < 0 || timeout > 20)
			timeout = 20;
#endif

		/* if redraws are pending do not wait for event,
		 * return immediately
		 */
		if (fbtk_get_redraw_pending(fbtk))
			timeout = 0;

#ifdef __DREAMCAST__
		/*
		 * Drive NetSurf fetchers using fetch_fdset/select.
		 *
		 * Without this, cURL progress depends on periodic scheduled polling. If the
		 * frontend loop blocks in event waits or rendering, socket activity may not
		 * be observed promptly which can manifest as inconsistent timeouts.
		 *
		 * fetch_fdset() makes progress (calls fetcher poll callbacks) and provides
		 * the sockets to wait on. If any are present, select() wakes us as soon as
		 * network activity occurs, and we immediately call fetch_fdset() again to
		 * let libcurl process the readable/writable sockets.
		 */
		{
			fd_set read_fd_set;
			fd_set write_fd_set;
			fd_set except_fd_set;
			int maxfd = -1;

			if (fetch_fdset(&read_fd_set, &write_fd_set, &except_fd_set, &maxfd) == NSERROR_OK) {
				if ((maxfd >= 0) && (timeout > 0)) {
					struct timeval tv;

					tv.tv_sec = timeout / 1000;
					tv.tv_usec = (timeout % 1000) * 1000;

					(void)select(maxfd + 1, &read_fd_set, &write_fd_set, &except_fd_set, &tv);

					/* Progress fetchers immediately after waking on socket activity. */
					(void)fetch_fdset(&read_fd_set, &write_fd_set, &except_fd_set, &maxfd);

					/* We already waited; don't block again in the event pump. */
					timeout = 0;
				}
			}
		}
#endif

		if (fbtk_event(fbtk, &event, timeout)) {
			if ((event.type == NSFB_EVENT_CONTROL) &&
			    (event.value.controlcode ==  NSFB_CONTROL_QUIT))
				fb_complete = true;
		}

		fbtk_redraw(fbtk);

#ifdef __DREAMCAST__
		dreamcast_sdl_update(fbtk_get_nsfb(fbtk));
#endif
	}
}

static void gui_quit(void)
{
	NSLOG(netsurf, INFO, "gui_quit");

#ifndef __DREAMCAST__
	/* Save cookies to disk (not on Dreamcast - no writable storage) */
	urldb_save_cookies(nsoption_charp(cookie_jar));
#endif

	framebuffer_finalise();
}

/* called back when click in browser window */
static int
fb_browser_window_click(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct gui_window *gw = cbi->context;
	struct browser_widget_s *bwidget = fbtk_get_userpw(widget);
	browser_mouse_state mouse;
	int x = cbi->x + bwidget->scrollx;
	int y = cbi->y + bwidget->scrolly;
	uint64_t time_now;
	static struct {
		enum { CLICK_SINGLE, CLICK_DOUBLE, CLICK_TRIPLE } type;
		uint64_t time;
	} last_click;

	if (cbi->event->type != NSFB_EVENT_KEY_DOWN &&
	    cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	NSLOG(netsurf, DEEPDEBUG, "browser window clicked at %d,%d",
			cbi->x, cbi->y);

	switch (cbi->event->type) {
	case NSFB_EVENT_KEY_DOWN:
		switch (cbi->event->value.keycode) {
		case NSFB_KEY_MOUSE_1:
			browser_window_mouse_click(gw->bw,
					BROWSER_MOUSE_PRESS_1, x, y);
			gui_drag.state = GUI_DRAG_PRESSED;
			gui_drag.button = 1;
			gui_drag.x = x;
			gui_drag.y = y;
			break;

		case NSFB_KEY_MOUSE_3:
			browser_window_mouse_click(gw->bw,
					BROWSER_MOUSE_PRESS_2, x, y);
			gui_drag.state = GUI_DRAG_PRESSED;
			gui_drag.button = 2;
			gui_drag.x = x;
			gui_drag.y = y;
			break;

		case NSFB_KEY_MOUSE_4:
			/* scroll up */
			if (browser_window_scroll_at_point(gw->bw,
							   x, y,
							   0, -100) == false)
				widget_scroll_y(gw, -100, false);
			break;

		case NSFB_KEY_MOUSE_5:
			/* scroll down */
			if (browser_window_scroll_at_point(gw->bw,
							   x, y,
							   0, 100) == false)
				widget_scroll_y(gw, 100, false);
			break;

		default:
			break;

		}

		break;
	case NSFB_EVENT_KEY_UP:

		mouse = 0;
		nsu_getmonotonic_ms(&time_now);

		switch (cbi->event->value.keycode) {
		case NSFB_KEY_MOUSE_1:
			if (gui_drag.state == GUI_DRAG_DRAG) {
				/* End of a drag, rather than click */

				if (gui_drag.grabbed_pointer) {
					/* need to ungrab pointer */
					fbtk_tgrab_pointer(widget);
					gui_drag.grabbed_pointer = false;
				}

				gui_drag.state = GUI_DRAG_NONE;

				/* Tell core */
				browser_window_mouse_track(gw->bw, 0, x, y);
				break;
			}
			/* This is a click;
			 * clear PRESSED state and pass to core */
			gui_drag.state = GUI_DRAG_NONE;
			mouse = BROWSER_MOUSE_CLICK_1;
			break;

		case NSFB_KEY_MOUSE_3:
			if (gui_drag.state == GUI_DRAG_DRAG) {
				/* End of a drag, rather than click */
				gui_drag.state = GUI_DRAG_NONE;

				if (gui_drag.grabbed_pointer) {
					/* need to ungrab pointer */
					fbtk_tgrab_pointer(widget);
					gui_drag.grabbed_pointer = false;
				}

				/* Tell core */
				browser_window_mouse_track(gw->bw, 0, x, y);
				break;
			}
			/* This is a click;
			 * clear PRESSED state and pass to core */
			gui_drag.state = GUI_DRAG_NONE;
			mouse = BROWSER_MOUSE_CLICK_2;
			break;

		default:
			break;

		}

		/* Determine if it's a double or triple click, allowing
		 * 0.5 seconds (500ms) between clicks
		 */
		if ((time_now < (last_click.time + 500)) &&
		    (cbi->event->value.keycode != NSFB_KEY_MOUSE_4) &&
		    (cbi->event->value.keycode != NSFB_KEY_MOUSE_5)) {
			if (last_click.type == CLICK_SINGLE) {
				/* Set double click */
				mouse |= BROWSER_MOUSE_DOUBLE_CLICK;
				last_click.type = CLICK_DOUBLE;

			} else if (last_click.type == CLICK_DOUBLE) {
				/* Set triple click */
				mouse |= BROWSER_MOUSE_TRIPLE_CLICK;
				last_click.type = CLICK_TRIPLE;
			} else {
				/* Set normal click */
				last_click.type = CLICK_SINGLE;
			}
		} else {
			last_click.type = CLICK_SINGLE;
		}

		if (mouse) {
			browser_window_mouse_click(gw->bw, mouse, x, y);
		}

		last_click.time = time_now;

		break;
	default:
		break;

	}
	return 1;
}

/* called back when movement in browser window */
static int
fb_browser_window_move(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	browser_mouse_state mouse = 0;
	struct gui_window *gw = cbi->context;
	struct browser_widget_s *bwidget = fbtk_get_userpw(widget);
	int x = cbi->x + bwidget->scrollx;
	int y = cbi->y + bwidget->scrolly;

	if (gui_drag.state == GUI_DRAG_PRESSED &&
			(abs(x - gui_drag.x) > 5 ||
			 abs(y - gui_drag.y) > 5)) {
		/* Drag started */
		if (gui_drag.button == 1) {
			browser_window_mouse_click(gw->bw,
					BROWSER_MOUSE_DRAG_1,
					gui_drag.x, gui_drag.y);
		} else {
			browser_window_mouse_click(gw->bw,
					BROWSER_MOUSE_DRAG_2,
					gui_drag.x, gui_drag.y);
		}
		gui_drag.grabbed_pointer = fbtk_tgrab_pointer(widget);
		gui_drag.state = GUI_DRAG_DRAG;
	}

	if (gui_drag.state == GUI_DRAG_DRAG) {
		/* set up mouse state */
		mouse |= BROWSER_MOUSE_DRAG_ON;

		if (gui_drag.button == 1)
			mouse |= BROWSER_MOUSE_HOLDING_1;
		else
			mouse |= BROWSER_MOUSE_HOLDING_2;
	}

	browser_window_mouse_track(gw->bw, mouse, x, y);

	return 0;
}


static int
fb_browser_window_input(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct gui_window *gw = cbi->context;
	static fbtk_modifier_type modifier = FBTK_MOD_CLEAR;
	int ucs4 = -1;

	NSLOG(netsurf, INFO, "got value %d", cbi->event->value.keycode);

	switch (cbi->event->type) {
	case NSFB_EVENT_KEY_DOWN:
		switch (cbi->event->value.keycode) {

		case NSFB_KEY_DELETE:
			browser_window_key_press(gw->bw, NS_KEY_DELETE_RIGHT);
			break;

		case NSFB_KEY_PAGEUP:
			if (browser_window_key_press(gw->bw,
					NS_KEY_PAGE_UP) == false)
				widget_scroll_y(gw, -fbtk_get_height(
						gw->browser), false);
			break;

		case NSFB_KEY_PAGEDOWN:
			if (browser_window_key_press(gw->bw,
					NS_KEY_PAGE_DOWN) == false)
				widget_scroll_y(gw, fbtk_get_height(
						gw->browser), false);
			break;

		case NSFB_KEY_RIGHT:
			if (modifier & FBTK_MOD_RCTRL ||
					modifier & FBTK_MOD_LCTRL) {
				/* CTRL held */
				if (browser_window_key_press(gw->bw,
						NS_KEY_LINE_END) == false)
					widget_scroll_x(gw, INT_MAX, true);

			} else if (modifier & FBTK_MOD_RSHIFT ||
					modifier & FBTK_MOD_LSHIFT) {
				/* SHIFT held */
				if (browser_window_key_press(gw->bw,
						NS_KEY_WORD_RIGHT) == false)
					widget_scroll_x(gw, fbtk_get_width(
						gw->browser), false);

			} else {
				/* no modifier */
				if (browser_window_key_press(gw->bw,
						NS_KEY_RIGHT) == false)
					widget_scroll_x(gw, 100, false);
			}
			break;

		case NSFB_KEY_LEFT:
			if (modifier & FBTK_MOD_RCTRL ||
					modifier & FBTK_MOD_LCTRL) {
				/* CTRL held */
				if (browser_window_key_press(gw->bw,
						NS_KEY_LINE_START) == false)
					widget_scroll_x(gw, 0, true);

			} else if (modifier & FBTK_MOD_RSHIFT ||
					modifier & FBTK_MOD_LSHIFT) {
				/* SHIFT held */
				if (browser_window_key_press(gw->bw,
						NS_KEY_WORD_LEFT) == false)
					widget_scroll_x(gw, -fbtk_get_width(
						gw->browser), false);

			} else {
				/* no modifier */
				if (browser_window_key_press(gw->bw,
						NS_KEY_LEFT) == false)
					widget_scroll_x(gw, -100, false);
			}
			break;

		case NSFB_KEY_UP:
			if (browser_window_key_press(gw->bw,
					NS_KEY_UP) == false)
				widget_scroll_y(gw, -100, false);
			break;

		case NSFB_KEY_DOWN:
			if (browser_window_key_press(gw->bw,
					NS_KEY_DOWN) == false)
				widget_scroll_y(gw, 100, false);
			break;

		case NSFB_KEY_MINUS:
			if (modifier & FBTK_MOD_RCTRL ||
					modifier & FBTK_MOD_LCTRL) {
				browser_window_set_scale(gw->bw, -0.1, false);
			}
			break;

		case NSFB_KEY_EQUALS: /* PLUS */
			if (modifier & FBTK_MOD_RCTRL ||
					modifier & FBTK_MOD_LCTRL) {
				browser_window_set_scale(gw->bw, 0.1, false);
			}
			break;

		case NSFB_KEY_0:
			if (modifier & FBTK_MOD_RCTRL ||
					modifier & FBTK_MOD_LCTRL) {
				browser_window_set_scale(gw->bw, 1.0, true);
			}
			break;

		case NSFB_KEY_RSHIFT:
			modifier |= FBTK_MOD_RSHIFT;
			break;

		case NSFB_KEY_LSHIFT:
			modifier |= FBTK_MOD_LSHIFT;
			break;

		case NSFB_KEY_RCTRL:
			modifier |= FBTK_MOD_RCTRL;
			break;

		case NSFB_KEY_LCTRL:
			modifier |= FBTK_MOD_LCTRL;
			break;

		case NSFB_KEY_y:
		case NSFB_KEY_z:
			if (cbi->event->value.keycode == NSFB_KEY_z &&
					(modifier & FBTK_MOD_RCTRL ||
					 modifier & FBTK_MOD_LCTRL) &&
					(modifier & FBTK_MOD_RSHIFT ||
					 modifier & FBTK_MOD_LSHIFT)) {
				/* Z pressed with CTRL and SHIFT held */
				browser_window_key_press(gw->bw, NS_KEY_REDO);
				break;

			} else if (cbi->event->value.keycode == NSFB_KEY_z &&
					(modifier & FBTK_MOD_RCTRL ||
					 modifier & FBTK_MOD_LCTRL)) {
				/* Z pressed with CTRL held */
				browser_window_key_press(gw->bw, NS_KEY_UNDO);
				break;

			} else if (cbi->event->value.keycode == NSFB_KEY_y &&
					(modifier & FBTK_MOD_RCTRL ||
					 modifier & FBTK_MOD_LCTRL)) {
				/* Y pressed with CTRL held */
				browser_window_key_press(gw->bw, NS_KEY_REDO);
				break;
			}
			/* Z or Y pressed but not undo or redo; */
			fallthrough;

		default:
			ucs4 = fbtk_keycode_to_ucs4(cbi->event->value.keycode,
						    modifier);
			if (ucs4 != -1)
				browser_window_key_press(gw->bw, ucs4);
			break;
		}
		break;

	case NSFB_EVENT_KEY_UP:
		switch (cbi->event->value.keycode) {
		case NSFB_KEY_RSHIFT:
			modifier &= ~FBTK_MOD_RSHIFT;
			break;

		case NSFB_KEY_LSHIFT:
			modifier &= ~FBTK_MOD_LSHIFT;
			break;

		case NSFB_KEY_RCTRL:
			modifier &= ~FBTK_MOD_RCTRL;
			break;

		case NSFB_KEY_LCTRL:
			modifier &= ~FBTK_MOD_LCTRL;
			break;

		default:
			break;
		}
		break;

	default:
		break;
	}

	return 0;
}

static void
fb_update_back_forward(struct gui_window *gw)
{
	struct browser_window *bw = gw->bw;

	fbtk_set_bitmap(gw->back,
			(browser_window_back_available(bw)) ?
			&left_arrow : &left_arrow_g);
	fbtk_set_bitmap(gw->forward,
			(browser_window_forward_available(bw)) ?
			&right_arrow : &right_arrow_g);
}

/* left icon click routine */
static int
fb_leftarrow_click(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct gui_window *gw = cbi->context;
	struct browser_window *bw = gw->bw;

	if (cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	if (browser_window_back_available(bw))
		browser_window_history_back(bw, false);

	fb_update_back_forward(gw);

	return 1;
}

/* right arrow icon click routine */
static int
fb_rightarrow_click(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct gui_window *gw = cbi->context;
	struct browser_window *bw = gw->bw;

	if (cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	if (browser_window_forward_available(bw))
		browser_window_history_forward(bw, false);

	fb_update_back_forward(gw);
	return 1;

}

/* reload icon click routine */
static int
fb_reload_click(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct browser_window *bw = cbi->context;

	if (cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	browser_window_reload(bw, true);
	return 1;
}

/* stop icon click routine */
static int
fb_stop_click(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct browser_window *bw = cbi->context;

	if (cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	browser_window_stop(bw);
	return 0;
}

static int
fb_osk_click(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{

	if (cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	map_osk();

	return 0;
}

/* close browser window icon click routine */
static int
fb_close_click(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	if (cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	fb_complete = true;

	return 0;
}

static int
fb_scroll_callback(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct gui_window *gw = cbi->context;

	switch (cbi->type) {
	case FBTK_CBT_SCROLLY:
		widget_scroll_y(gw, cbi->y, true);
		break;

	case FBTK_CBT_SCROLLX:
		widget_scroll_x(gw, cbi->x, true);
		break;

	default:
		break;
	}
	return 0;
}

static int
fb_url_enter(void *pw, char *text)
{
	struct browser_window *bw = pw;
	nsurl *url;
	nserror error;

	error = nsurl_create(text, &url);
	if (error != NSERROR_OK) {
		fb_warn_user("Errorcode:", messages_get_errorcode(error));
	} else {
		browser_window_navigate(bw, url, NULL, BW_NAVIGATE_HISTORY,
				NULL, NULL, NULL);
		nsurl_unref(url);
	}

	return 0;
}

static int
fb_url_move(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	framebuffer_set_cursor(&caret_image);
	return 0;
}

static int
set_ptr_default_move(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	framebuffer_set_cursor(&pointer_image);
	return 0;
}

static int
fb_localhistory_btn_clik(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	struct gui_window *gw = cbi->context;

	if (cbi->event->type != NSFB_EVENT_KEY_UP)
		return 0;

	fb_local_history_present(fbtk, gw->bw);

	return 0;
}


/** Create a toolbar window and populate it with buttons. 
 *
 * The toolbar layout uses a character to define buttons type and position:
 * b - back
 * l - local history
 * f - forward
 * s - stop 
 * r - refresh
 * u - url bar expands to fit remaining space
 * t - throbber/activity indicator
 * c - close the current window
 *
 * The default layout is "blfsrut" there should be no more than a
 * single url bar entry or behaviour will be undefined.
 *
 * @param gw Parent window 
 * @param toolbar_height The height in pixels of the toolbar
 * @param padding The padding in pixels round each element of the toolbar
 * @param frame_col Frame colour.
 * @param toolbar_layout A string defining which buttons and controls
 *                       should be added to the toolbar. May be empty
 *                       string to disable the bar..
 * 
 */
static fbtk_widget_t *
create_toolbar(struct gui_window *gw, 
	       int toolbar_height, 
	       int padding, 
	       colour frame_col,
	       const char *toolbar_layout)
{
	fbtk_widget_t *toolbar;
	fbtk_widget_t *widget;

	int xpos; /* The position of the next widget. */
	int xlhs = 0; /* extent of the left hand side widgets */
	int xdir = 1; /* the direction of movement + or - 1 */
	const char *itmtype; /* type of the next item */

	if (toolbar_layout == NULL) {
		toolbar_layout = NSFB_TOOLBAR_DEFAULT_LAYOUT;
	}

	NSLOG(netsurf, INFO, "Using toolbar layout %s", toolbar_layout);

	itmtype = toolbar_layout;

	/* check for the toolbar being disabled */
	if ((*itmtype == 0) || (*itmtype == 'q')) {
		return NULL;
	}

	toolbar = fbtk_create_window(gw->window, 0, 0, 0, 
				     toolbar_height, 
				     frame_col);

	if (toolbar == NULL) {
		return NULL;
	}

	fbtk_set_handler(toolbar, 
			 FBTK_CBT_POINTERENTER, 
			 set_ptr_default_move, 
			 NULL);


	xpos = padding;

	/* loop proceeds creating widget on the left hand side until
	 * it runs out of layout or encounters a url bar declaration
	 * wherupon it works backwards from the end of the layout
	 * untill the space left is for the url bar
	 */
	while ((itmtype >= toolbar_layout) && 
	       (*itmtype != 0) && 
	       (xdir !=0)) {

		NSLOG(netsurf, INFO, "toolbar adding %c", *itmtype);


		switch (*itmtype) {

		case 'b': /* back */
			widget = fbtk_create_button(toolbar, 
						    (xdir == 1) ? xpos : 
						     xpos - left_arrow.width, 
						    padding, 
						    left_arrow.width, 
						    -padding, 
						    frame_col, 
						    &left_arrow, 
						    fb_leftarrow_click, 
						    gw);
			gw->back = widget; /* keep reference */
			break;

		case 'l': /* local history */
			widget = fbtk_create_button(toolbar,
						    (xdir == 1) ? xpos : 
						     xpos - history_image.width,
						    padding,
						    history_image.width,
						    -padding,
						    frame_col,
						    &history_image,
						    fb_localhistory_btn_clik,
						    gw);
			gw->history = widget;
			break;

		case 'f': /* forward */
			widget = fbtk_create_button(toolbar,
						    (xdir == 1)?xpos : 
						     xpos - right_arrow.width,
						    padding,
						    right_arrow.width,
						    -padding,
						    frame_col,
						    &right_arrow,
						    fb_rightarrow_click,
						    gw);
			gw->forward = widget;
			break;

		case 'c': /* close the current window */
			widget = fbtk_create_button(toolbar,
						    (xdir == 1)?xpos : 
						     xpos - stop_image_g.width,
						    padding,
						    stop_image_g.width,
						    -padding,
						    frame_col,
						    &stop_image_g,
						    fb_close_click,
						    gw->bw);
			gw->close = widget;
			break;

		case 's': /* stop  */
			widget = fbtk_create_button(toolbar,
						    (xdir == 1)?xpos : 
						     xpos - stop_image.width,
						    padding,
						    stop_image.width,
						    -padding,
						    frame_col,
						    &stop_image,
						    fb_stop_click,
						    gw->bw);
			gw->stop = widget;
			break;

		case 'r': /* reload */
			widget = fbtk_create_button(toolbar,
						    (xdir == 1)?xpos : 
						     xpos - reload.width,
						    padding,
						    reload.width,
						    -padding,
						    frame_col,
						    &reload,
						    fb_reload_click,
						    gw->bw);
			gw->reload = widget;
			break;

		case 't': /* throbber/activity indicator */
			widget = fbtk_create_bitmap(toolbar,
						    (xdir == 1)?xpos : 
						     xpos - throbber0.width,
						    padding,
						    throbber0.width,
						    -padding,
						    frame_col, 
						    &throbber0);
			gw->throbber = widget;
			break;


		case 'u': /* url bar*/
			if (xdir == -1) {
				/* met the u going backwards add url
				 * now we know available extent 
				 */ 

				widget = fbtk_create_writable_text(toolbar,
						   xlhs,
						   padding,
						   xpos - xlhs,
						   -padding,
						   FB_COLOUR_WHITE,
						   FB_COLOUR_BLACK,
						   true,
						   fb_url_enter,
						   gw->bw);

				fbtk_set_handler(widget, 
						 FBTK_CBT_POINTERENTER, 
						 fb_url_move, gw->bw);

				gw->url = widget; /* keep reference */

				/* toolbar is complete */
				xdir = 0;
				break;
			}
			/* met url going forwards, note position and
			 * reverse direction 
			 */
			itmtype = toolbar_layout + strlen(toolbar_layout);
			xdir = -1;
			xlhs = xpos;
			xpos = (2 * fbtk_get_width(toolbar));
			widget = toolbar;
			break;

		default:
			widget = NULL;
			xdir = 0;
			NSLOG(netsurf, INFO,
			      "Unknown element %c in toolbar layout",
			      *itmtype);
		        break;

		}

		if (widget != NULL) {
			xpos += (xdir * (fbtk_get_width(widget) + padding));
		}

		NSLOG(netsurf, INFO, "xpos is %d", xpos);

		itmtype += xdir;
	}

	fbtk_set_mapping(toolbar, true);

	return toolbar;
}


/** Resize a toolbar.
 *
 * @param gw Parent window
 * @param toolbar_height The height in pixels of the toolbar
 * @param padding The padding in pixels round each element of the toolbar
 * @param toolbar_layout A string defining which buttons and controls
 *                       should be added to the toolbar. May be empty
 *                       string to disable the bar.
 */
static void
resize_toolbar(struct gui_window *gw,
	       int toolbar_height,
	       int padding,
	       const char *toolbar_layout)
{
	fbtk_widget_t *widget;

	int xpos; /* The position of the next widget. */
	int xlhs = 0; /* extent of the left hand side widgets */
	int xdir = 1; /* the direction of movement + or - 1 */
	const char *itmtype; /* type of the next item */
	int x = 0, y = 0, w = 0, h = 0;

	if (gw->toolbar == NULL) {
		return;
	}

	if (toolbar_layout == NULL) {
		toolbar_layout = NSFB_TOOLBAR_DEFAULT_LAYOUT;
	}

	itmtype = toolbar_layout;

	if (*itmtype == 0) {
		return;
	}

	fbtk_set_pos_and_size(gw->toolbar, 0, 0, 0, toolbar_height);

	xpos = padding;

	/* loop proceeds creating widget on the left hand side until
	 * it runs out of layout or encounters a url bar declaration
	 * wherupon it works backwards from the end of the layout
	 * untill the space left is for the url bar
	 */
	while (itmtype >= toolbar_layout && xdir != 0) {

		switch (*itmtype) {
		case 'b': /* back */
			widget = gw->back;
			x = (xdir == 1) ? xpos : xpos - left_arrow.width;
			y = padding;
			w = left_arrow.width;
			h = -padding;
			break;

		case 'l': /* local history */
			widget = gw->history;
			x = (xdir == 1) ? xpos : xpos - history_image.width;
			y = padding;
			w = history_image.width;
			h = -padding;
			break;

		case 'f': /* forward */
			widget = gw->forward;
			x = (xdir == 1) ? xpos : xpos - right_arrow.width;
			y = padding;
			w = right_arrow.width;
			h = -padding;
			break;

		case 'c': /* close the current window */
			widget = gw->close;
			x = (xdir == 1) ? xpos : xpos - stop_image_g.width;
			y = padding;
			w = stop_image_g.width;
			h = -padding;
			break;

		case 's': /* stop  */
			widget = gw->stop;
			x = (xdir == 1) ? xpos : xpos - stop_image.width;
			y = padding;
			w = stop_image.width;
			h = -padding;
			break;

		case 'r': /* reload */
			widget = gw->reload;
			x = (xdir == 1) ? xpos : xpos - reload.width;
			y = padding;
			w = reload.width;
			h = -padding;
			break;

		case 't': /* throbber/activity indicator */
			widget = gw->throbber;
			x = (xdir == 1) ? xpos : xpos - throbber0.width;
			y = padding;
			w = throbber0.width;
			h = -padding;
			break;


		case 'u': /* url bar*/
			if (xdir == -1) {
				/* met the u going backwards add url
				 * now we know available extent
				 */
				widget = gw->url;
				x = xlhs;
				y = padding;
				w = xpos - xlhs;
				h = -padding;

				/* toolbar is complete */
				xdir = 0;
				break;
			}
			/* met url going forwards, note position and
			 * reverse direction
			 */
			itmtype = toolbar_layout + strlen(toolbar_layout);
			xdir = -1;
			xlhs = xpos;
			w = fbtk_get_width(gw->toolbar);
			xpos = 2 * w;
			widget = gw->toolbar;
			break;

		default:
			widget = NULL;
		        break;

		}

		if (widget != NULL) {
			if (widget != gw->toolbar)
				fbtk_set_pos_and_size(widget, x, y, w, h);
			xpos += xdir * (w + padding);
		}

		itmtype += xdir;
	}
}

/** Routine called when "stripped of focus" event occours for browser widget.
 *
 * @param widget The widget reciving "stripped of focus" event.
 * @param cbi The callback parameters.
 * @return The callback result.
 */
static int
fb_browser_window_strip_focus(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	fbtk_set_caret(widget, false, 0, 0, 0, NULL);

	return 0;
}

static void
create_browser_widget(struct gui_window *gw, int toolbar_height, int furniture_width)
{
	struct browser_widget_s *browser_widget;
	browser_widget = calloc(1, sizeof(struct browser_widget_s));

	gw->browser = fbtk_create_user(gw->window,
				       0,
				       toolbar_height,
				       -furniture_width,
				       -furniture_width,
				       browser_widget);

	fbtk_set_handler(gw->browser, FBTK_CBT_REDRAW, fb_browser_window_redraw, gw);
	fbtk_set_handler(gw->browser, FBTK_CBT_DESTROY, fb_browser_window_destroy, gw);
	fbtk_set_handler(gw->browser, FBTK_CBT_INPUT, fb_browser_window_input, gw);
	fbtk_set_handler(gw->browser, FBTK_CBT_CLICK, fb_browser_window_click, gw);
	fbtk_set_handler(gw->browser, FBTK_CBT_STRIP_FOCUS, fb_browser_window_strip_focus, gw);
	fbtk_set_handler(gw->browser, FBTK_CBT_POINTERMOVE, fb_browser_window_move, gw);
}

static void
resize_browser_widget(struct gui_window *gw, int x, int y,
		int width, int height)
{
	fbtk_set_pos_and_size(gw->browser, x, y, width, height);
	browser_window_schedule_reformat(gw->bw);
}

static void
create_normal_browser_window(struct gui_window *gw, int furniture_width)
{
	fbtk_widget_t *widget;
	fbtk_widget_t *toolbar;
	int statusbar_width = 0;
	int toolbar_height = nsoption_int(fb_toolbar_size);

	NSLOG(netsurf, INFO, "Normal window");

	gw->window = fbtk_create_window(fbtk, 0, 0, 0, 0, 0);

	statusbar_width = nsoption_int(toolbar_status_size) *
		fbtk_get_width(gw->window) / 10000;

	/* toolbar */
	toolbar = create_toolbar(gw, 
				 toolbar_height, 
				 2, 
				 FB_FRAME_COLOUR, 
				 nsoption_charp(fb_toolbar_layout));
	gw->toolbar = toolbar;

	/* set the actually created toolbar height */
	if (toolbar != NULL) {
		toolbar_height = fbtk_get_height(toolbar);
	} else {
		toolbar_height = 0;
	}

	/* status bar */
	gw->status = fbtk_create_text(gw->window,
				      0,
				      fbtk_get_height(gw->window) - furniture_width,
				      statusbar_width, furniture_width,
				      FB_FRAME_COLOUR, FB_COLOUR_BLACK,
				      false);
	fbtk_set_handler(gw->status, FBTK_CBT_POINTERENTER, set_ptr_default_move, NULL);

	NSLOG(netsurf, INFO, "status bar %p at %d,%d", gw->status,
	      fbtk_get_absx(gw->status), fbtk_get_absy(gw->status));

	/* create horizontal scrollbar */
	gw->hscroll = fbtk_create_hscroll(gw->window,
					  statusbar_width,
					  fbtk_get_height(gw->window) - furniture_width,
					  fbtk_get_width(gw->window) - statusbar_width - furniture_width,
					  furniture_width,
					  FB_SCROLL_COLOUR,
					  FB_FRAME_COLOUR,
					  fb_scroll_callback,
					  gw);

	/* fill bottom right area */

	if (nsoption_bool(fb_osk) == true) {
		widget = fbtk_create_text_button(gw->window,
						 fbtk_get_width(gw->window) - furniture_width,
						 fbtk_get_height(gw->window) - furniture_width,
						 furniture_width,
						 furniture_width,
						 FB_FRAME_COLOUR, FB_COLOUR_BLACK,
						 fb_osk_click,
						 NULL);
		widget = fbtk_create_button(gw->window,
				fbtk_get_width(gw->window) - furniture_width,
				fbtk_get_height(gw->window) - furniture_width,
				furniture_width,
				furniture_width,
				FB_FRAME_COLOUR,
				&osk_image,
				fb_osk_click,
				NULL);
	} else {
		widget = fbtk_create_fill(gw->window,
					  fbtk_get_width(gw->window) - furniture_width,
					  fbtk_get_height(gw->window) - furniture_width,
					  furniture_width,
					  furniture_width,
					  FB_FRAME_COLOUR);

		fbtk_set_handler(widget, FBTK_CBT_POINTERENTER, set_ptr_default_move, NULL);
	}

	gw->bottom_right = widget;

	/* create vertical scrollbar */
	gw->vscroll = fbtk_create_vscroll(gw->window,
					  fbtk_get_width(gw->window) - furniture_width,
					  toolbar_height,
					  furniture_width,
					  fbtk_get_height(gw->window) - toolbar_height - furniture_width,
					  FB_SCROLL_COLOUR,
					  FB_FRAME_COLOUR,
					  fb_scroll_callback,
					  gw);

	/* browser widget */
	create_browser_widget(gw, toolbar_height, nsoption_int(fb_furniture_size));

	/* Give browser_window's user widget input focus */
	fbtk_set_focus(gw->browser);
}

static void
resize_normal_browser_window(struct gui_window *gw, int furniture_width)
{
	bool resized;
	int width, height;
	int statusbar_width;
	int toolbar_height = fbtk_get_height(gw->toolbar);

	/* Resize the main window widget */
	resized = fbtk_set_pos_and_size(gw->window, 0, 0, 0, 0);
	if (!resized)
		return;

	width = fbtk_get_width(gw->window);
	height = fbtk_get_height(gw->window);
	statusbar_width = nsoption_int(toolbar_status_size) * width / 10000;

	resize_toolbar(gw, toolbar_height, 2,
			nsoption_charp(fb_toolbar_layout));
	fbtk_set_pos_and_size(gw->status,
			0, height - furniture_width,
			statusbar_width, furniture_width);
	fbtk_reposition_hscroll(gw->hscroll,
			statusbar_width, height - furniture_width,
			width - statusbar_width - furniture_width,
			furniture_width);
	fbtk_set_pos_and_size(gw->bottom_right,
			width - furniture_width, height - furniture_width,
			furniture_width, furniture_width);
	fbtk_reposition_vscroll(gw->vscroll,
			width - furniture_width,
			toolbar_height, furniture_width,
			height - toolbar_height - furniture_width);
	resize_browser_widget(gw,
			0, toolbar_height,
			width - furniture_width,
			height - furniture_width - toolbar_height);
}

static void gui_window_add_to_window_list(struct gui_window *gw)
{
	gw->next = NULL;
	gw->prev = NULL;

	if (window_list == NULL) {
		window_list = gw;
	} else {
		window_list->prev = gw;
		gw->next = window_list;
		window_list = gw;
	}
}

static void gui_window_remove_from_window_list(struct gui_window *gw)
{
	struct gui_window *list;

	for (list = window_list; list != NULL; list = list->next) {
		if (list != gw)
			continue;

		if (list == window_list) {
			window_list = list->next;
			if (window_list != NULL)
				window_list->prev = NULL;
		} else {
			list->prev->next = list->next;
			if (list->next != NULL) {
				list->next->prev = list->prev;
			}
		}
		break;
	}
}


static struct gui_window *
gui_window_create(struct browser_window *bw,
		struct gui_window *existing,
		gui_window_create_flags flags)
{
	struct gui_window *gw;

	gw = calloc(1, sizeof(struct gui_window));

	if (gw == NULL)
		return NULL;

	/* associate the gui window with the underlying browser window
	 */
	gw->bw = bw;

	create_normal_browser_window(gw, nsoption_int(fb_furniture_size));

	/* map and request redraw of gui window */
	fbtk_set_mapping(gw->window, true);

	/* Add it to the window list */
	gui_window_add_to_window_list(gw);

	return gw;
}

static void
gui_window_destroy(struct gui_window *gw)
{
	gui_window_remove_from_window_list(gw);

	fbtk_destroy_widget(gw->window);

	free(gw);
}


/**
 * Invalidates an area of a framebuffer browser window
 *
 * \param g The netsurf window being invalidated.
 * \param rect area to redraw or NULL for the entire window area
 * \return NSERROR_OK on success or appropriate error code
 */
static nserror
fb_window_invalidate_area(struct gui_window *g, const struct rect *rect)
{
	struct browser_widget_s *bwidget = fbtk_get_userpw(g->browser);

	if (rect != NULL) {
		fb_queue_redraw(g->browser,
				rect->x0 - bwidget->scrollx,
				rect->y0 - bwidget->scrolly,
				rect->x1 - bwidget->scrollx,
				rect->y1 - bwidget->scrolly);
	} else {
		fb_queue_redraw(g->browser,
				0,
				0,
				fbtk_get_width(g->browser),
				fbtk_get_height(g->browser));
	}
	return NSERROR_OK;
}

static bool
gui_window_get_scroll(struct gui_window *g, int *sx, int *sy)
{
	struct browser_widget_s *bwidget = fbtk_get_userpw(g->browser);

	*sx = bwidget->scrollx;
	*sy = bwidget->scrolly;

	return true;
}

/**
 * Set the scroll position of a framebuffer browser window.
 *
 * Scrolls the viewport to ensure the specified rectangle of the
 *   content is shown. The framebuffer implementation scrolls the contents so
 *   the specified point in the content is at the top of the viewport.
 *
 * \param gw gui_window to scroll
 * \param rect The rectangle to ensure is shown.
 * \return NSERROR_OK on success or apropriate error code.
 */
static nserror
gui_window_set_scroll(struct gui_window *gw, const struct rect *rect)
{
	struct browser_widget_s *bwidget = fbtk_get_userpw(gw->browser);

	assert(bwidget);

	widget_scroll_x(gw, rect->x0, true);
	widget_scroll_y(gw, rect->y0, true);

	return NSERROR_OK;
}


/**
 * Find the current dimensions of a framebuffer browser window content area.
 *
 * \param gw The gui window to measure content area of.
 * \param width receives width of window
 * \param height receives height of window
 * \return NSERROR_OK on sucess and width and height updated.
 */
static nserror
gui_window_get_dimensions(struct gui_window *gw, int *width, int *height)
{
	*width = fbtk_get_width(gw->browser);
	*height = fbtk_get_height(gw->browser);

	return NSERROR_OK;
}

static void
gui_window_update_extent(struct gui_window *gw)
{
	int w, h;
	browser_window_get_extents(gw->bw, true, &w, &h);

	fbtk_set_scroll_parameters(gw->hscroll, 0, w,
			fbtk_get_width(gw->browser), 100);

	fbtk_set_scroll_parameters(gw->vscroll, 0, h,
			fbtk_get_height(gw->browser), 100);
}

static void
gui_window_set_status(struct gui_window *g, const char *text)
{
	fbtk_set_text(g->status, text);
}

static void
gui_window_set_pointer(struct gui_window *g, gui_pointer_shape shape)
{
	switch (shape) {
	case GUI_POINTER_POINT:
		framebuffer_set_cursor(&hand_image);
		break;

	case GUI_POINTER_CARET:
		framebuffer_set_cursor(&caret_image);
		break;

	case GUI_POINTER_MENU:
		framebuffer_set_cursor(&menu_image);
		break;

	case GUI_POINTER_PROGRESS:
		framebuffer_set_cursor(&progress_image);
		break;

	case GUI_POINTER_MOVE:
		framebuffer_set_cursor(&move_image);
		break;

	default:
		framebuffer_set_cursor(&pointer_image);
		break;
	}
}

static nserror
gui_window_set_url(struct gui_window *g, nsurl *url)
{
	fbtk_set_text(g->url, nsurl_access(url));
	return NSERROR_OK;
}

static void
throbber_advance(void *pw)
{
	struct gui_window *g = pw;
	struct fbtk_bitmap *image;

	switch (g->throbber_index) {
	case 0:
		image = &throbber1;
		g->throbber_index = 1;
		break;

	case 1:
		image = &throbber2;
		g->throbber_index = 2;
		break;

	case 2:
		image = &throbber3;
		g->throbber_index = 3;
		break;

	case 3:
		image = &throbber4;
		g->throbber_index = 4;
		break;

	case 4:
		image = &throbber5;
		g->throbber_index = 5;
		break;

	case 5:
		image = &throbber6;
		g->throbber_index = 6;
		break;

	case 6:
		image = &throbber7;
		g->throbber_index = 7;
		break;

	case 7:
		image = &throbber8;
		g->throbber_index = 0;
		break;

	default:
		return;
	}

	if (g->throbber_index >= 0) {
		fbtk_set_bitmap(g->throbber, image);
		framebuffer_schedule(100, throbber_advance, g);
	}
}

static void
gui_window_start_throbber(struct gui_window *g)
{
	g->throbber_index = 0;
	framebuffer_schedule(100, throbber_advance, g);
}

static void
gui_window_stop_throbber(struct gui_window *gw)
{
	gw->throbber_index = -1;
	fbtk_set_bitmap(gw->throbber, &throbber0);

	fb_update_back_forward(gw);

}

static void
gui_window_remove_caret_cb(fbtk_widget_t *widget)
{
	struct browser_widget_s *bwidget = fbtk_get_userpw(widget);
	int c_x, c_y, c_h;

	if (fbtk_get_caret(widget, &c_x, &c_y, &c_h)) {
		/* browser window already had caret:
		 * redraw its area to remove it first */
		fb_queue_redraw(widget,
				c_x - bwidget->scrollx,
				c_y - bwidget->scrolly,
				c_x + 1 - bwidget->scrollx,
				c_y + c_h - bwidget->scrolly);
	}
}

static void
gui_window_place_caret(struct gui_window *g, int x, int y, int height,
		const struct rect *clip)
{
	struct browser_widget_s *bwidget = fbtk_get_userpw(g->browser);

	/* set new pos */
	fbtk_set_caret(g->browser, true, x, y, height,
			gui_window_remove_caret_cb);

	/* redraw new caret pos */
	fb_queue_redraw(g->browser,
			x - bwidget->scrollx,
			y - bwidget->scrolly,
			x + 1 - bwidget->scrollx,
			y + height - bwidget->scrolly);
}

static void
gui_window_remove_caret(struct gui_window *g)
{
	int c_x, c_y, c_h;

	if (fbtk_get_caret(g->browser, &c_x, &c_y, &c_h)) {
		/* browser window owns the caret, so can remove it */
		fbtk_set_caret(g->browser, false, 0, 0, 0, NULL);
	}
}

/**
 * process miscellaneous window events
 *
 * \param gw The window receiving the event.
 * \param event The event code.
 * \return NSERROR_OK when processed ok
 */
static nserror
gui_window_event(struct gui_window *gw, enum gui_window_event event)
{
	switch (event) {
	case GW_EVENT_UPDATE_EXTENT:
		gui_window_update_extent(gw);
		break;

	case GW_EVENT_REMOVE_CARET:
		gui_window_remove_caret(gw);
		break;

	case GW_EVENT_START_THROBBER:
		gui_window_start_throbber(gw);
		break;

	case GW_EVENT_STOP_THROBBER:
		gui_window_stop_throbber(gw);
		break;

	default:
		break;
	}
	return NSERROR_OK;
}

static struct gui_window_table framebuffer_window_table = {
	.create = gui_window_create,
	.destroy = gui_window_destroy,
	.invalidate = fb_window_invalidate_area,
	.get_scroll = gui_window_get_scroll,
	.set_scroll = gui_window_set_scroll,
	.get_dimensions = gui_window_get_dimensions,
	.event = gui_window_event,

	.set_url = gui_window_set_url,
	.set_status = gui_window_set_status,
	.set_pointer = gui_window_set_pointer,
	.place_caret = gui_window_place_caret,
};


static struct gui_misc_table framebuffer_misc_table = {
	.schedule = framebuffer_schedule,

	.quit = gui_quit,
};

/**
 * Entry point from OS.
 *
 * /param argc The number of arguments in the string vector.
 * /param argv The argument string vector.
 * /return The return code to the OS
 */
int
main(int argc, char** argv)
{
	struct browser_window *bw;
	char *options;
	char *messages;
	nsurl *url;
	nserror ret;
	nsfb_t *nsfb;
	struct netsurf_table framebuffer_table = {
		.misc = &framebuffer_misc_table,
		.window = &framebuffer_window_table,
		.corewindow = framebuffer_core_window_table,
		.clipboard = framebuffer_clipboard_table,
		.fetch = framebuffer_fetch_table,
		.utf8 = framebuffer_utf8_table,
		.bitmap = framebuffer_bitmap_table,
		.layout = framebuffer_layout_table,
	};

#ifdef __DREAMCAST__
	/* Mount the romdisk filesystem at /rd */
	fs_romdisk_mount("/rd", romdisk, 1);
#endif

	ret = netsurf_register(&framebuffer_table);
        if (ret != NSERROR_OK) {
		die("NetSurf operation table failed registration");
        }

	respaths = fb_init_resource_path(NETSURF_FB_RESPATH":"NETSURF_FB_FONTPATH);

#ifdef __DREAMCAST__
	dreamcast_prepend_en_respath();
#endif

	/* initialise logging. Not fatal if it fails but not much we
	 * can do about it either.
	 */
	#ifdef __DREAMCAST__
	/* Ensure stderr is unbuffered so logs appear promptly in emulators. */
	setbuf(stderr, NULL);
	/*
	 * Ensure debug output is emitted even if we are not using libnslog.
	 * When libnslog is in use we rely on log_filter/verbose_filter to control
	 * output, so do not force verbose logging.
	 */
	#ifdef WITH_NSLOG
	verbose_log = false;
	#else
	verbose_log = true;
	#endif
	#endif
	nslog_init(nslog_stream_configure, &argc, argv);

	/* user options setup */
	ret = nsoption_init(set_defaults, &nsoptions, &nsoptions_default);
	if (ret != NSERROR_OK) {
		die("Options failed to initialise");
	}
#ifdef __DREAMCAST__
	ret = nsoption_read("/rd/Choices", nsoptions);
	if (ret != NSERROR_OK) {
		fprintf(stderr, "[dc] Failed to read /rd/Choices (rc=%d)\n", (int)ret);
	}
	/* Show the active filter strings (useful when diagnosing missing logs). */
	fprintf(stderr, "[dc] log_filter='%s'\n",
		(nsoption_charp(log_filter) != NULL) ? nsoption_charp(log_filter) : "(null)");
	fprintf(stderr, "[dc] verbose_filter='%s'\n",
		(nsoption_charp(verbose_filter) != NULL) ? nsoption_charp(verbose_filter) : "(null)");
	/* Ensure logging filter is applied after loading /rd/Choices. */
	{
		nserror logret = nslog_set_filter_by_options();
		fprintf(stderr, "[dc] nslog_set_filter_by_options (after Choices) rc=%d\n",
			(int)logret);
		if (logret != NSERROR_OK) {
			fprintf(stderr, "[dc] Failed to apply log filter from /rd/Choices (rc=%d)\n",
				(int)logret);
		}
	}
	/* Ensure curl has a CA bundle even if Choices could not be read. */
	if (nsoption_charp(ca_bundle) == NULL) {
		nsoption_setnull_charp(ca_bundle, strdup("/rd/ca-bundle"));
		fprintf(stderr, "[dc] Forcing ca_bundle to /rd/ca-bundle\n");
	}
	/* ca_path is intentionally left unset on Dreamcast - curl.c will
	 * explicitly clear CURLOPT_CAPATH to override libcurl's default.
	 * PolarSSL/mbedTLS will use only the ca_bundle file. */
	{
		FILE *cafp = fopen("/rd/ca-bundle", "r");
		if (cafp == NULL) {
			fprintf(stderr, "[dc] Unable to open /rd/ca-bundle for reading\n");
		} else {
			fclose(cafp);
			fprintf(stderr, "[dc] /rd/ca-bundle is readable\n");
		}
	}
#else
	options = filepath_find(respaths, "Choices");
	nsoption_read(options, nsoptions);
	free(options);
#endif
	nsoption_commandline(&argc, argv, nsoptions);
	/* Show the final filter strings after commandline overrides. */
	fprintf(stderr, "[dc] log_filter (after cmdline)='%s'\n",
		(nsoption_charp(log_filter) != NULL) ? nsoption_charp(log_filter) : "(null)");
	fprintf(stderr, "[dc] verbose_filter (after cmdline)='%s'\n",
		(nsoption_charp(verbose_filter) != NULL) ? nsoption_charp(verbose_filter) : "(null)");
	/* Re-apply logging filter after commandline overrides. */
	{
		nserror logret = nslog_set_filter_by_options();
		fprintf(stderr, "[dc] nslog_set_filter_by_options (after cmdline) rc=%d\n",
			(int)logret);
		if (logret != NSERROR_OK) {
			fprintf(stderr, "[dc] Failed to apply log filter after commandline (rc=%d)\n",
				(int)logret);
		}
	}

	#ifdef __DREAMCAST__
	fprintf(stderr, "[dc] ca_bundle='%s'\n",
		(nsoption_charp(ca_bundle) != NULL) ? nsoption_charp(ca_bundle) : "(null)");
	fprintf(stderr, "[dc] ca_path='%s' (will be cleared by curl.c)\n",
		(nsoption_charp(ca_path) != NULL) ? nsoption_charp(ca_path) : "(null)");
	fprintf(stderr, "[dc] suppress_curl_debug=%d\n",
		nsoption_bool(suppress_curl_debug) ? 1 : 0);
	fprintf(stderr, "[dc] verbose_log=%d\n", verbose_log ? 1 : 0);
	#endif

	/* message init */
	messages = filepath_find(respaths, "Messages");
        ret = messages_add_from_file(messages);
	free(messages);
	if (ret != NSERROR_OK) {
		fprintf(stderr, "Message translations failed to load\n");
	}

#ifdef __DREAMCAST__
    dreamcast_apply_memory_tuning();
    dreamcast_detect_video_cable();
#endif

	/* common initialisation */
	ret = netsurf_init(NULL);
	if (ret != NSERROR_OK) {
		die("NetSurf failed to initialise");
	}

	/* Override, since we have no support for non-core SELECT menu */
	nsoption_set_bool(core_select_menu, true);

	if (process_cmdline(argc,argv) != true)
		die("unable to process command line.\n");

	nsfb = framebuffer_initialise(fename, fewidth, feheight, febpp);
	if (nsfb == NULL)
		die("Unable to initialise framebuffer");

#ifdef __DREAMCAST__
	if (dreamcast_sdl_init(fewidth, feheight, febpp) == false) {
		die("Unable to initialise Dreamcast SDL video");
	}
	
	/* Performance: enable dirty rectangle optimization for better page load performance */
	dreamcast_sdl_set_dirty_optimization(true);
#endif

	framebuffer_set_cursor(&pointer_image);

	if (fb_font_init() == false)
		die("Unable to initialise the font system");

	fbtk = fbtk_init(nsfb);

	fbtk_enable_oskb(fbtk);

#ifdef __DREAMCAST__
    if (!dc_settings_init(fbtk)) {
        NSLOG(netsurf, WARNING, "Failed to initialize settings menu");
    }
    dc_settings_load();
#else
	/* Load persistent cookies from disk (not on Dreamcast - no writable storage) */
	urldb_load_cookies(nsoption_charp(cookie_file));
#endif

	/* create an initial browser window */

	NSLOG(netsurf, INFO, "calling browser_window_create");

	ret = nsurl_create(feurl, &url);
	if (ret == NSERROR_OK) {
		ret = browser_window_create(BW_CREATE_HISTORY,
					      url,
					      NULL,
					      NULL,
					      &bw);
		nsurl_unref(url);
	}
	if (ret != NSERROR_OK) {
		fb_warn_user("Errorcode:", messages_get_errorcode(ret));
	} else {
		framebuffer_run();

		browser_window_destroy(bw);
	}

#ifdef __DREAMCAST__
	/* Cleanup settings menu (saves if dirty) */
	dc_settings_fini();

	dreamcast_sdl_quit();
#endif

	netsurf_exit();

	if (fb_font_finalise() == false)
		NSLOG(netsurf, INFO, "Font finalisation failed.");

	/* finalise options */
	nsoption_finalise(nsoptions, nsoptions_default);

	/* finalise logging */
	nslog_finalise();

	return 0;
}

void gui_resize(fbtk_widget_t *root, int width, int height)
{
	struct gui_window *gw;
	nsfb_t *nsfb = fbtk_get_nsfb(root);

	/* Enforce a minimum */
	if (width < 300)
		width = 300;
	if (height < 200)
		height = 200;

	if (framebuffer_resize(nsfb, width, height, febpp) == false) {
		return;
	}

	fbtk_set_pos_and_size(root, 0, 0, width, height);

	fewidth = width;
	feheight = height;

	for (gw = window_list; gw != NULL; gw = gw->next) {
		resize_normal_browser_window(gw,
				nsoption_int(fb_furniture_size));
	}

	fbtk_request_redraw(root);
}


/*
 * Local Variables:
 * c-basic-offset:8
 * End:
 */
