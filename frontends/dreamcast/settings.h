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
 * Dreamcast settings menu overlay interface.
 */

#ifndef NETSURF_DREAMCAST_SETTINGS_H
#define NETSURF_DREAMCAST_SETTINGS_H

#include <stdbool.h>
#include "framebuffer/fbtk.h"

/**
 * Initialize the settings menu system.
 *
 * \param parent  The parent FBTK widget (typically the root window).
 * \return true on success, false on failure.
 */
bool dc_settings_init(fbtk_widget_t *parent);

/**
 * Toggle the settings menu visibility.
 *
 * If the menu is hidden, it will be shown and input captured.
 * If the menu is visible, it will be hidden and settings saved.
 */
void dc_settings_toggle(void);

/**
 * Check if the settings menu is currently visible.
 *
 * \return true if the settings menu is open.
 */
bool dc_settings_is_open(void);

/**
 * Process controller input for the settings menu.
 *
 * This should be called from the main input polling loop when the
 * settings menu is open. It handles D-pad navigation and value adjustment.
 *
 * \param buttons      Current button state (CONT_* bitmask).
 * \param prev_buttons Previous button state for edge detection.
 * \return true if input was consumed by the settings menu.
 */
bool dc_settings_input(uint32_t buttons, uint32_t prev_buttons);

/**
 * Shutdown the settings menu and free resources.
 */
void dc_settings_fini(void);

/**
 * Load settings from VMU on startup.
 *
 * Attempts to find and load a Choices file from any available VMU.
 * If no VMU or file is found, default settings are used.
 */
void dc_settings_load(void);

/**
 * Save current settings to VMU.
 *
 * Attempts to save the Choices file to the first writable VMU.
 * Displays a warning if no VMU is available.
 *
 * \return true on success, false if save failed.
 */
bool dc_settings_save(void);

#endif /* NETSURF_DREAMCAST_SETTINGS_H */
