/*
 * Copyright 2024 NetSurf Browser Project
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

/**
 * \file
 * Dreamcast settings menu overlay implementation.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#include <libnsfb.h>
#include <libnsfb_plot.h>

#include "utils/log.h"
#include "utils/nsoption.h"

#include "netsurf/plotters.h"

#include "framebuffer/gui.h"
#include "framebuffer/fbtk.h"
#include "framebuffer/font.h"
#include "framebuffer/framebuffer.h"

#include "dreamcast/settings.h"

#ifdef __DREAMCAST__
#include <kos.h>
#include <malloc.h>
#include <dc/maple/controller.h>
#include <dc/fs_vmu.h>
#include <dc/vmu_pkg.h>
#endif

/* Settings menu dimensions */
#define SETTINGS_WIDTH    400
#define SETTINGS_HEIGHT   300
#define SETTINGS_PADDING  16
#define SETTINGS_LINE_HEIGHT 28
#define SETTINGS_TITLE_HEIGHT 40

/* Colors (ARGB format for libnsfb) */
#define COLOR_BG         0xE0202020  /* Dark gray, slightly transparent */
#define COLOR_TITLE_BG   0xFF404080  /* Purple header */
#define COLOR_TEXT       0xFFFFFFFF  /* White */
#define COLOR_HIGHLIGHT  0xFF6060A0  /* Highlighted row */
#define COLOR_VALUE      0xFF80FF80  /* Green for values */

/* Setting types */
typedef enum {
	SETTING_INT,
	SETTING_STRING
} setting_type_t;

/* Setting item definition */
typedef struct {
	const char *name;          /* Display name */
	setting_type_t type;       /* Type of setting */
	int option_id;             /* nsoption identifier (for get/set) */
	int min_val;               /* Minimum value (for INT) */
	int max_val;               /* Maximum value (for INT) */
	int step;                  /* Increment step (for INT) */
} setting_item_t;

/* List of settings to display */
static const setting_item_t settings_list[] = {
	{ "Font Size",     SETTING_INT, NSOPTION_font_size,     80, 320, 10 },
	{ "Min Font Size", SETTING_INT, NSOPTION_font_min_size, 50, 200, 10 },
	/* Homepage URL would be SETTING_STRING but requires OSK integration */
};

#define SETTINGS_COUNT (sizeof(settings_list) / sizeof(settings_list[0]))

/* Settings menu state */
static struct {
	fbtk_widget_t *parent;     /* Parent widget (root window) */
	fbtk_widget_t *window;     /* Settings overlay window */
	bool visible;              /* Is menu currently shown */
	int selected;              /* Currently selected item index */
	bool dirty;                /* Settings have been modified */
} settings_state = {
	.parent = NULL,
	.window = NULL,
	.visible = false,
	.selected = 0,
	.dirty = false
};

/* VMU file path for persistence */
static char vmu_path[64] = "";

/**
 * Find the first available VMU slot for read/write.
 */
static bool find_vmu_slot(char *path_out, size_t path_len)
{
#ifdef __DREAMCAST__
	/* Check VMU slots in order: a1, a2, b1, b2, c1, c2, d1, d2 */
	const char *slots[] = {
		"/vmu/a1", "/vmu/a2",
		"/vmu/b1", "/vmu/b2",
		"/vmu/c1", "/vmu/c2",
		"/vmu/d1", "/vmu/d2"
	};
	
	for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
		/* Try to open the VMU directory to check if it exists */
		DIR *d = opendir(slots[i]);
		if (d != NULL) {
			closedir(d);
			snprintf(path_out, path_len, "%s/NSCHOICE", slots[i]);
			return true;
		}
	}
#endif
	return false;
}

/**
 * Get integer value for a setting.
 */
static int get_setting_int(int option_id)
{
	switch (option_id) {
	case NSOPTION_font_size:
		return nsoption_int(font_size);
	case NSOPTION_font_min_size:
		return nsoption_int(font_min_size);
	default:
		return 0;
	}
}

/**
 * Set integer value for a setting.
 */
static void set_setting_int(int option_id, int value)
{
	switch (option_id) {
	case NSOPTION_font_size:
		nsoption_set_int(font_size, value);
		break;
	case NSOPTION_font_min_size:
		nsoption_set_int(font_min_size, value);
		break;
	}
}

/**
 * Draw callback for the settings window.
 */
static int settings_redraw_cb(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{
	nsfb_t *nsfb = fbtk_get_nsfb(widget);
	int x = fbtk_get_absx(widget);
	int y = fbtk_get_absy(widget);
	int width = fbtk_get_width(widget);
	nsfb_bbox_t rect;
	nsfb_bbox_t bbox;
	char value_str[32];
	char diag_str[96];
	
	/* Font style for text rendering */
	plot_font_style_t title_style = {
		.family = PLOT_FONT_FAMILY_SANS_SERIF,
		.size = 16 * PLOT_STYLE_SCALE,
		.weight = 700,
		.flags = FONTF_NONE,
		.background = COLOR_TITLE_BG,
		.foreground = COLOR_TEXT
	};
	
	plot_font_style_t item_style = {
		.family = PLOT_FONT_FAMILY_SANS_SERIF,
		.size = 14 * PLOT_STYLE_SCALE,
		.weight = 400,
		.flags = FONTF_NONE,
		.background = COLOR_BG,
		.foreground = COLOR_TEXT
	};
	
	plot_font_style_t help_style = {
		.family = PLOT_FONT_FAMILY_SANS_SERIF,
		.size = 12 * PLOT_STYLE_SCALE,
		.weight = 400,
		.flags = FONTF_NONE,
		.background = 0xFF303030,
		.foreground = 0xFFAAAAAA
	};

	plot_font_style_t diag_style = {
		.family = PLOT_FONT_FAMILY_SANS_SERIF,
		.size = 12 * PLOT_STYLE_SCALE,
		.weight = 400,
		.flags = FONTF_NONE,
		.background = COLOR_BG,
		.foreground = 0xFFCCCCCC
	};
	
	struct redraw_context ctx = {
		.interactive = true,
		.background_images = true,
		.plot = &fb_plotters
	};
	
	/* Get bounding box for claim/update */
	fbtk_get_bbox(widget, &bbox);
	nsfb_claim(nsfb, &bbox);
	
	/* Draw background */
	rect.x0 = x;
	rect.y0 = y;
	rect.x1 = x + width;
	rect.y1 = y + SETTINGS_HEIGHT;
	nsfb_plot_rectangle_fill(nsfb, &rect, COLOR_BG);
	
	/* Draw title bar */
	rect.y1 = y + SETTINGS_TITLE_HEIGHT;
	nsfb_plot_rectangle_fill(nsfb, &rect, COLOR_TITLE_BG);
	
	/* Draw title text */
	ctx.plot->text(&ctx, &title_style,
		x + SETTINGS_PADDING, y + 28,
		"Settings", 8);
	
	/* Draw setting items */
	int item_y = y + SETTINGS_TITLE_HEIGHT + SETTINGS_PADDING;
	for (size_t i = 0; i < SETTINGS_COUNT; i++) {
		const setting_item_t *item = &settings_list[i];
		
		/* Highlight selected item */
		if ((int)i == settings_state.selected) {
			rect.x0 = x + 4;
			rect.y0 = item_y - 2;
			rect.x1 = x + width - 4;
			rect.y1 = item_y + SETTINGS_LINE_HEIGHT - 4;
			nsfb_plot_rectangle_fill(nsfb, &rect, COLOR_HIGHLIGHT);
			item_style.background = COLOR_HIGHLIGHT;
		} else {
			item_style.background = COLOR_BG;
		}
		
		/* Draw item name */
		ctx.plot->text(&ctx, &item_style,
			x + SETTINGS_PADDING + 16, item_y + 18,
			item->name, strlen(item->name));
		
		/* Draw value for integer settings */
		if (item->type == SETTING_INT) {
			int val = get_setting_int(item->option_id);
			int range = item->max_val - item->min_val;
			int bar_width = 100;
			int filled = (val - item->min_val) * bar_width / range;
			
			/* Value text */
			if (item->option_id == NSOPTION_font_size ||
			    item->option_id == NSOPTION_font_min_size) {
				snprintf(value_str, sizeof(value_str), "%.1fpt", val / 10.0);
			} else {
				snprintf(value_str, sizeof(value_str), "%d", val);
			}
			ctx.plot->text(&ctx, &item_style,
				x + width - SETTINGS_PADDING - 50, item_y + 18,
				value_str, strlen(value_str));
			
			/* Bar background */
			rect.x0 = x + width - SETTINGS_PADDING - bar_width - 60;
			rect.y0 = item_y + 6;
			rect.x1 = rect.x0 + bar_width;
			rect.y1 = item_y + SETTINGS_LINE_HEIGHT - 10;
			nsfb_plot_rectangle_fill(nsfb, &rect, 0xFF404040);
			
			/* Bar fill */
			rect.x1 = rect.x0 + filled;
			nsfb_plot_rectangle_fill(nsfb, &rect, COLOR_VALUE);
		}
		
		item_y += SETTINGS_LINE_HEIGHT;
	}

	/* Diagnostics section (optional) */
	{
		int diag_y = y + SETTINGS_HEIGHT - 30 - SETTINGS_PADDING - (SETTINGS_LINE_HEIGHT * 2);
		ctx.plot->text(&ctx, &diag_style,
			x + SETTINGS_PADDING, diag_y + 18,
			"Diagnostics", 11);

		snprintf(diag_str, sizeof(diag_str), "Build: %s %s", __DATE__, __TIME__);
		ctx.plot->text(&ctx, &diag_style,
			x + SETTINGS_PADDING, diag_y + 18 + SETTINGS_LINE_HEIGHT,
			diag_str, strlen(diag_str));

		/* mallinfo() is provided by newlib on Dreamcast toolchains */
		{
			struct mallinfo mi = mallinfo();
			snprintf(diag_str, sizeof(diag_str),
				"Heap: used %d KB, free %d KB",
				mi.uordblks / 1024, mi.fordblks / 1024);
			ctx.plot->text(&ctx, &diag_style,
				x + SETTINGS_PADDING, diag_y + 18 + (SETTINGS_LINE_HEIGHT * 2),
				diag_str, strlen(diag_str));
		}
	}
	
	/* Draw help text at bottom */
	rect.x0 = x;
	rect.y0 = y + SETTINGS_HEIGHT - 30;
	rect.x1 = x + width;
	rect.y1 = y + SETTINGS_HEIGHT;
	nsfb_plot_rectangle_fill(nsfb, &rect, 0xFF303030);
	
	ctx.plot->text(&ctx, &help_style,
		x + SETTINGS_PADDING, y + SETTINGS_HEIGHT - 10,
		"D-Pad: Navigate/Adjust  A: Save  B: Cancel",
		strlen("D-Pad: Navigate/Adjust  A: Save  B: Cancel"));
	
	nsfb_update(nsfb, &bbox);
	
	return 0;
}

bool dc_settings_init(fbtk_widget_t *parent)
{
	int parent_width, parent_height;
	int win_x, win_y;
	
	if (parent == NULL) {
		return false;
	}
	
	settings_state.parent = parent;
	
	/* Get parent dimensions for centering */
	parent_width = fbtk_get_width(parent);
	parent_height = fbtk_get_height(parent);
	
	win_x = (parent_width - SETTINGS_WIDTH) / 2;
	win_y = (parent_height - SETTINGS_HEIGHT) / 2;
	
	/* Create overlay window */
	settings_state.window = fbtk_create_window(
		parent,
		win_x, win_y,
		SETTINGS_WIDTH, SETTINGS_HEIGHT,
		0  /* No fill color, we draw our own background */
	);
	
	if (settings_state.window == NULL) {
		NSLOG(netsurf, ERROR, "Failed to create settings window");
		return false;
	}
	
	/* Set up redraw callback */
	fbtk_set_handler(settings_state.window, FBTK_CBT_REDRAW, 
			 settings_redraw_cb, NULL);
	
	/* Start hidden */
	fbtk_set_mapping(settings_state.window, false);
	settings_state.visible = false;
	
	/* Find VMU path for persistence */
	find_vmu_slot(vmu_path, sizeof(vmu_path));
	
	NSLOG(netsurf, INFO, "Settings menu initialized");
	return true;
}

void dc_settings_toggle(void)
{
	if (settings_state.window == NULL) {
		return;
	}
	
	if (settings_state.visible) {
		/* Closing - save if dirty */
		if (settings_state.dirty) {
			dc_settings_save();
			settings_state.dirty = false;
		}
		fbtk_set_mapping(settings_state.window, false);
		settings_state.visible = false;
		NSLOG(netsurf, INFO, "Settings menu closed");
	} else {
		/* Opening */
		fbtk_set_zorder(settings_state.window, INT_MIN);
		fbtk_set_mapping(settings_state.window, true);
		fbtk_request_redraw(settings_state.window);
		settings_state.visible = true;
		NSLOG(netsurf, INFO, "Settings menu opened");
	}
}

bool dc_settings_is_open(void)
{
	return settings_state.visible;
}

bool dc_settings_input(uint32_t buttons, uint32_t prev_buttons)
{
#ifdef __DREAMCAST__
	uint32_t pressed = (buttons ^ prev_buttons) & buttons;
	
	if (!settings_state.visible) {
		return false;
	}
	
	/* D-pad Up - previous item */
	if (pressed & CONT_DPAD_UP) {
		if (settings_state.selected > 0) {
			settings_state.selected--;
			fbtk_request_redraw(settings_state.window);
		}
		return true;
	}
	
	/* D-pad Down - next item */
	if (pressed & CONT_DPAD_DOWN) {
		if (settings_state.selected < (int)SETTINGS_COUNT - 1) {
			settings_state.selected++;
			fbtk_request_redraw(settings_state.window);
		}
		return true;
	}
	
	/* D-pad Left/Right - adjust value */
	if ((pressed & CONT_DPAD_LEFT) || (pressed & CONT_DPAD_RIGHT)) {
		const setting_item_t *item = &settings_list[settings_state.selected];
		
		if (item->type == SETTING_INT) {
			int val = get_setting_int(item->option_id);
			int delta = (pressed & CONT_DPAD_LEFT) ? -item->step : item->step;
			val += delta;
			
			/* Clamp to range */
			if (val < item->min_val) val = item->min_val;
			if (val > item->max_val) val = item->max_val;
			
			set_setting_int(item->option_id, val);
			settings_state.dirty = true;
			fbtk_request_redraw(settings_state.window);
		}
		return true;
	}
	
	/* B button - close without explicit save (auto-saves anyway) */
	if (pressed & CONT_B) {
		dc_settings_toggle();
		return true;
	}
	
	/* Start button also closes */
	if (pressed & CONT_START) {
		dc_settings_toggle();
		return true;
	}
	
	/* Consume all input while settings menu is open */
	return true;
#else
	(void)buttons;
	(void)prev_buttons;
	return false;
#endif
}

void dc_settings_fini(void)
{
	if (settings_state.window != NULL) {
		/* Save on exit if dirty */
		if (settings_state.dirty) {
			dc_settings_save();
		}
		fbtk_destroy_widget(settings_state.window);
		settings_state.window = NULL;
	}
	settings_state.visible = false;
}

void dc_settings_load(void)
{
#ifdef __DREAMCAST__
	if (vmu_path[0] == '\0') {
		if (!find_vmu_slot(vmu_path, sizeof(vmu_path))) {
			NSLOG(netsurf, WARNING, "No VMU found for settings");
			return;
		}
	}
	
	/* Try to read VMU package file */
	FILE *fp = fopen(vmu_path, "rb");
	if (!fp) {
		NSLOG(netsurf, INFO, "No saved settings found at %s", vmu_path);
		return;
	}
	
	/* Get file size */
	fseek(fp, 0, SEEK_END);
	long file_size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	
	if (file_size <= sizeof(vmu_hdr_t)) {
		NSLOG(netsurf, WARNING, "VMU file too small at %s", vmu_path);
		fclose(fp);
		return;
	}
	
	/* Read VMU header */
	vmu_hdr_t hdr;
	if (fread(&hdr, sizeof(vmu_hdr_t), 1, fp) != 1) {
		NSLOG(netsurf, ERROR, "Failed to read VMU header from %s", vmu_path);
		fclose(fp);
		return;
	}
	
	/* Verify this is a NetSurf settings file */
	if (strncmp(hdr.app_id, "NSURF", 5) != 0) {
		NSLOG(netsurf, INFO, "VMU file is not a NetSurf settings file: %s", vmu_path);
		fclose(fp);
		return;
	}
	
	/* Read the actual settings data */
	long data_size = file_size - sizeof(vmu_hdr_t);
	char *settings_data = malloc(data_size + 1);
	if (!settings_data) {
		NSLOG(netsurf, ERROR, "Failed to allocate memory for settings data");
		fclose(fp);
		return;
	}
	
	if (fread(settings_data, 1, data_size, fp) != data_size) {
		NSLOG(netsurf, ERROR, "Failed to read settings data from %s", vmu_path);
		free(settings_data);
		fclose(fp);
		return;
	}
	
	settings_data[data_size] = '\0'; /* Null-terminate for string operations */
	fclose(fp);
	
	/* Create a temporary file with the settings data for nsoption_read */
	char temp_path[64];
	snprintf(temp_path, sizeof(temp_path), "/tmp/ns_settings_%ld", (long)time(NULL));
	
	FILE *temp_fp = fopen(temp_path, "w");
	if (temp_fp) {
		fwrite(settings_data, 1, data_size, temp_fp);
		fclose(temp_fp);
		
		/* Use NetSurf's option parser to read the data */
		nserror err = nsoption_read(temp_path, NULL);
		if (err == NSERROR_OK) {
			NSLOG(netsurf, INFO, "Settings loaded from %s", vmu_path);
		} else {
			NSLOG(netsurf, WARNING, "Failed to parse settings from %s", vmu_path);
		}
		
		/* Clean up temporary file */
		unlink(temp_path);
	} else {
		NSLOG(netsurf, ERROR, "Failed to create temporary file for settings");
	}
	
	free(settings_data);
#endif
}

bool dc_settings_save(void)
{
#ifdef __DREAMCAST__
	if (vmu_path[0] == '\0') {
		if (!find_vmu_slot(vmu_path, sizeof(vmu_path))) {
			NSLOG(netsurf, WARNING, "No VMU available for saving settings");
			return false;
		}
	}
	
	/* Create temporary file to store raw settings data */
	char temp_path[64];
	snprintf(temp_path, sizeof(temp_path), "/tmp/ns_settings_%ld", (long)time(NULL));
	
	/* Use NetSurf's option writer to create the settings data */
	nserror err = nsoption_write(temp_path, NULL, NULL);
	if (err != NSERROR_OK) {
		NSLOG(netsurf, ERROR, "Failed to write settings data to temp file");
		unlink(temp_path);
		return false;
	}
	
	/* Read the settings data */
	FILE *temp_fp = fopen(temp_path, "rb");
	if (!temp_fp) {
		NSLOG(netsurf, ERROR, "Failed to open temp settings file");
		unlink(temp_path);
		return false;
	}
	
	fseek(temp_fp, 0, SEEK_END);
	long data_size = ftell(temp_fp);
	fseek(temp_fp, 0, SEEK_SET);
	
	char *settings_data = malloc(data_size);
	if (!settings_data) {
		NSLOG(netsurf, ERROR, "Failed to allocate memory for settings data");
		fclose(temp_fp);
		unlink(temp_path);
		return false;
	}
	
	if (fread(settings_data, 1, data_size, temp_fp) != data_size) {
		NSLOG(netsurf, ERROR, "Failed to read settings data from temp file");
		free(settings_data);
		fclose(temp_fp);
		unlink(temp_path);
		return false;
	}
	
	fclose(temp_fp);
	unlink(temp_path);
	
	/* Create VMU package */
	vmu_pkg_t pkg;
	memset(&pkg, 0, sizeof(pkg));
	
	/* Set package metadata */
	strcpy(pkg.desc_short, "NetSurf Settings");
	strcpy(pkg.desc_long, "NetSurf browser settings");
	strcpy(pkg.app_id, "NSURF");
	pkg.icon_cnt = 0;  /* No icons for settings file */
	pkg.icon_anim_speed = 0;
	pkg.eyecatch_type = VMUPKG_EC_NONE;
	pkg.data_len = data_size;
	pkg.data = (const uint8_t *)settings_data;
	pkg.icon_data = NULL;
	pkg.eyecatch_data = NULL;
	
	/* Build VMU package with proper header */
	uint8_t *vmu_data = NULL;
	int vmu_size = 0;
	int build_result = vmu_pkg_build(&pkg, &vmu_data, &vmu_size);
	
	if (build_result != 0 || vmu_data == NULL) {
		NSLOG(netsurf, ERROR, "Failed to build VMU package");
		free(settings_data);
		if (vmu_data) free(vmu_data);
		return false;
	}
	
	/* Write VMU package to file */
	FILE *fp = fopen(vmu_path, "wb");
	if (!fp) {
		NSLOG(netsurf, ERROR, "Failed to open VMU file for writing: %s", vmu_path);
		free(settings_data);
		free(vmu_data);
		return false;
	}
	
	bool success = (fwrite(vmu_data, 1, vmu_size, fp) == vmu_size);
	fclose(fp);
	
	free(settings_data);
	free(vmu_data);
	
	if (success) {
		NSLOG(netsurf, INFO, "Settings saved to %s (%d bytes)", vmu_path, vmu_size);
	} else {
		NSLOG(netsurf, ERROR, "Failed to write VMU package to %s", vmu_path);
	}
	
	return success;
#else
	return true;
#endif
}
