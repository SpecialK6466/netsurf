# NetSurf Dreamcast frontend makefile fragment
# Reuse the framebuffer frontend sources with Dreamcast defaults and KOS toolchain.
#
# This stays isolated to frontends/dreamcast as per DREAMCAST-PORT.md.

# Tool overrides (kos-cc/kos-c++) and pkg-config paths for kos-ports
include frontends/dreamcast/Makefile.tools
# Frontend defaults (resource paths, font paths, toggles)
include frontends/dreamcast/Makefile.defaults

# Ensure framebuffer options are compiled in (required for NSOPTION_fb_* symbols)
CFLAGS += -Dnsframebuffer

# Reuse framebuffer assets so image/font generation can find sources
FRONTEND_RESOURCES_DIR := frontends/framebuffer/res
# Ensure tools (convert_image/font) include framebuffer headers (fbtk.h lives there)
INCLUDE_DIRS += frontends/framebuffer

# ---------------------------------------------------------------------------
# Reuse the framebuffer frontend sources and rules
# ---------------------------------------------------------------------------
include frontends/framebuffer/Makefile

# Generate split Messages into the build directory.
# The framebuffer resources tree contains a Messages symlink; after a clean build
# the symlink target may not exist, which breaks romdisk staging with cp -L.
MESSAGES_TARGET := $(OBJROOT)/messages

# ---------------------------------------------------------------------------
# Source mapping
# ---------------------------------------------------------------------------
# The core build system will prefix S_FRONTEND entries with frontends/$(TARGET)/.
# Since we reuse framebuffer's sources, we point those entries at ../framebuffer
# so the resulting path is frontends/dreamcast/../framebuffer/<file>.
S_FRONTEND := gui_dc.c framebuffer_dc.c schedule_dc.c \
             ../framebuffer/bitmap.c ../framebuffer/fetch.c ../framebuffer/findfile.c \
             ../framebuffer/corewindow.c ../framebuffer/local_history.c \
             ../framebuffer/clipboard.c \
             ../framebuffer/font_$(NETSURF_FB_FONTLIB).c \
             ../framebuffer/fbtk/fbtk.c ../framebuffer/fbtk/event.c \
             ../framebuffer/fbtk/fill.c ../framebuffer/fbtk/bitmap.c \
             ../framebuffer/fbtk/user.c ../framebuffer/fbtk/window.c \
             ../framebuffer/fbtk/text.c ../framebuffer/fbtk/scroll.c \
             ../framebuffer/fbtk/osk.c

 # Recompute SOURCES now that S_FRONTEND has been remapped.
 SOURCES = $(S_COMMON) $(S_IMAGE) $(S_BROWSER) $(S_FRONTEND) $(S_IMAGES) $(S_FONTS)
 
 # Font generation dependency fix:
 # The framebuffer Makefile attaches font generation prerequisites to
 # 'frontends/framebuffer/font_internal.c'. In the Dreamcast build we compile
 # the same file via 'frontends/dreamcast/../framebuffer/font_internal.c', which
 # does not match that prerequisite rule. Ensure the generated font sources exist
 # (which also generates the matching headers) before compiling.
 frontends/dreamcast/../framebuffer/font_internal.c: $(S_FONTS)

# ---------------------------------------------------------------------------
# Dreamcast-specific adjustments
# ---------------------------------------------------------------------------

# Name the resulting binary distinctly
EXETARGET := netsurf-dreamcast.elf

 # KOS / POSIX feature flags (config.h already handles HAVE_* guards)
 CFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L

 # Dreamcast debug: compile in DEBUG logs so cURL verbose output is visible
 # (fetch_curl_debug logs at NSLOG(fetch, DEBUG, ...)).
 # The main Makefile sets CFLAGS before including us, so we must override via
 # another -D (compiler uses last definition).
 CFLAGS += -DNETSURF_LOG_LEVEL=DEBUG

 # Dreamcast-only feature toggles
 CFLAGS += -DNETSURF_DC_ENABLE_NET=$(NETSURF_DC_ENABLE_NET)

# Rename framebuffer main() so we can provide a Dreamcast entry point
CFLAGS += -Dmain=framebuffer_main

# SDL 1.2 (kos-ports) for presenting the RAM surface
SDL_CFLAGS  ?= $(shell $(PKG_CONFIG) --cflags sdl 2>/dev/null || sdl-config --cflags 2>/dev/null)
SDL_LIBS    ?= $(shell $(PKG_CONFIG) --libs sdl 2>/dev/null   || sdl-config --libs 2>/dev/null)
CFLAGS  += $(SDL_CFLAGS)
LDFLAGS += $(SDL_LIBS)

# Add Dreamcast sources (init + SDL blitter + entry)
# Note: romdisk_stub.c removed because we link the real romdisk.o below
SOURCES += frontends/dreamcast/kos_init.c \
           frontends/dreamcast/sdl_dc_surface.c \
           frontends/dreamcast/posix_shim.c \
           frontends/dreamcast/irq_compat.c \
           frontends/dreamcast/entropy_dc.c \
           frontends/dreamcast/settings.c \
           frontends/dreamcast/main.c

 # Dreamcast-only DNS workaround:
 # When enabled, wrap libc resolver calls so libcurl can resolve hostnames
 # via the DNS server configured by KOS (net_default_dev->dns).
 ifneq ($(NETSURF_DC_ENABLE_NET),0)
 SOURCES += frontends/dreamcast/dns_shim.c
 LDFLAGS += -Wl,--wrap=getaddrinfo -Wl,--wrap=freeaddrinfo -Wl,--wrap=gai_strerror -Wl,--wrap=gethostbyname
 endif
 
 # ---------------------------------------------------------------------------
 # Romdisk generation
 # ---------------------------------------------------------------------------
 # When using the freetype font backend, we must ship TTF files on the romdisk.
 # These are sourced from the host system by default.
 DC_HOST_FONT_DIR ?= /usr/share/fonts/truetype/dejavu
 DC_TTF_FONTS ?= DejaVuSans.ttf DejaVuSans-Bold.ttf \
		DejaVuSerif.ttf DejaVuSerif-Bold.ttf \
		DejaVuSansMono.ttf DejaVuSansMono-Bold.ttf
 
 ROMDISK_STAGE := $(OBJROOT)/romdisk_stage
 ROMDISK_IMG   := $(OBJROOT)/romdisk.img
 ROMDISK_OBJ   := $(OBJROOT)/romdisk.o
 ROMDISK_STAMP := $(ROMDISK_STAGE)/.stamp

# The message targets are generated later by the top-level Makefile.
# Use secondary expansion so $(MESSAGES) is evaluated when this rule is executed.
.SECONDEXPANSION:

 # ---------------------------------------------------------------------------
 # Offline site scraping (host-side helper)
 # ---------------------------------------------------------------------------
 DC_SCRAPE_SCRIPT ?= frontends/dreamcast/tools/scrape_site.sh
 DC_SCRAPE_LIST   ?= frontends/dreamcast/tools/sites.txt
 DC_SCRAPE_LEVEL  ?= 2
 DC_SCRAPE_QUOTA  ?= 2m
 DC_SCRAPE_MAX_FILESIZE ?= 256k
 DC_SITES_STAMP   ?= frontends/dreamcast/res/sites/.stamp

 # Warn if romdisk becomes too large for practical Dreamcast execution.
 # The romdisk image is linked into the ELF and consumes RAM.
 DC_ROMDISK_WARN_BYTES ?= 6291456

.PHONY: dc-scrape-sites dc-scrape-site

 # Scrape all sites from the list file into frontends/dreamcast/res/sites/
 # This performs network access on the build host.
 dc-scrape-sites:
 	$(VQ)echo "  SCRAPE: $(DC_SCRAPE_LIST)"
	$(Q)bash $(DC_SCRAPE_SCRIPT) --clean --list $(DC_SCRAPE_LIST) --level $(DC_SCRAPE_LEVEL) --quota $(DC_SCRAPE_QUOTA) --max-filesize $(DC_SCRAPE_MAX_FILESIZE)

# Scrape a single site:
#   make TARGET=dreamcast dc-scrape-site DC_SITE_NAME=example DC_SITE_URL=http://example.com/
 dc-scrape-site:
 	$(VQ)echo "  SCRAPE: $(DC_SITE_NAME) $(DC_SITE_URL)"
	$(Q)bash $(DC_SCRAPE_SCRIPT) $(DC_SITE_NAME) $(DC_SITE_URL) --level $(DC_SCRAPE_LEVEL) --quota $(DC_SCRAPE_QUOTA) --max-filesize $(DC_SCRAPE_MAX_FILESIZE)

# Populate the staging directory with resources
# 1. Copy framebuffer defaults (icons, etc.)
# 2. Copy generated messages
# Note: Use -L to dereference symlinks (KOS romdisk can't resolve them at runtime)
$(ROMDISK_STAMP): $$(RESOURCES) $$(MESSAGES) $(DC_SITES_STAMP) frontends/dreamcast/Makefile frontends/dreamcast/Makefile.defaults frontends/dreamcast/Makefile.tools
	$(VQ)echo "   STAGE: $(ROMDISK_STAGE)"
	$(Q)mkdir -p $(ROMDISK_STAGE)
	$(Q)for f in frontends/framebuffer/res/*; do \
		b=$$(basename "$$f"); \
		if [ "$$b" != "Messages" ]; then \
			cp -rL "$$f" "$(ROMDISK_STAGE)/"; \
		fi; \
	done
	$(Q)if [ -d frontends/dreamcast/res ]; then \
		cp -rL frontends/dreamcast/res/* "$(ROMDISK_STAGE)/"; \
	fi
	$(Q)mkdir -p $(ROMDISK_STAGE)/
	$(Q)cp -rL $(MESSAGES_TARGET)/* $(ROMDISK_STAGE)/
	$(Q)cp -L resources/ca-bundle $(ROMDISK_STAGE)/ca-bundle
	$(Q)printf '%s\n' \
		'ca_bundle:/rd/ca-bundle' \
		'suppress_curl_debug:0' \
		'http_proxy:0' \
		'curl_fetch_cachesize:8' \
		'curl_fetch_timeout:20' \
		'max_fetchers:2' \
		'max_fetchers_per_host:1' \
		'max_cached_fetch_handles:0' \
		'enable_javascript:1' \
		'script_timeout:10' \
		'fb_osk:1' \
		'log_filter:((lvl:WARN || (lvl:DEBUG && cat:fetch)) || (lvl:INFO && cat:netsurf))' \
		'verbose_filter:((lvl:WARN || (lvl:DEBUG && cat:fetch)) || (lvl:INFO && cat:netsurf))' \
		> $(ROMDISK_STAGE)/Choices
	$(Q)if [ "$(NETSURF_FB_FONTLIB)" = "freetype" ]; then \
		mkdir -p "$(ROMDISK_STAGE)/fonts"; \
		for f in $(DC_TTF_FONTS); do \
			if [ ! -f "$(DC_HOST_FONT_DIR)/$$f" ]; then \
				echo "Missing host font: $(DC_HOST_FONT_DIR)/$$f" >&2; \
				exit 1; \
			fi; \
			cp -L "$(DC_HOST_FONT_DIR)/$$f" "$(ROMDISK_STAGE)/fonts/"; \
		done; \
		rm -f "$(ROMDISK_STAGE)/fonts/glyph_data"; \
	fi
	$(Q)touch $@

$(ROMDISK_STAGE): $(ROMDISK_STAMP)

 # Generate romdisk image
 $(ROMDISK_IMG): $(ROMDISK_STAMP)
	$(VQ)echo " GENROMFS: $@"
	$(Q)	$(GENROMFS) -f $@ -d $(ROMDISK_STAGE) -v > /dev/null
	$(Q)	sz=$$(wc -c < $@); \
		if [ $$sz -gt $(DC_ROMDISK_WARN_BYTES) ]; then \
			echo "WARNING: romdisk.img is $$sz bytes (over $(DC_ROMDISK_WARN_BYTES)). This may prevent Dreamcast from launching or leave no heap." >&2; \
		fi

# Convert romdisk image to object file (symbol name: romdisk)
$(ROMDISK_OBJ): $(ROMDISK_IMG)
	$(VQ)echo "   BIN2O: $@"
	$(Q)$(BIN2O) $< romdisk $@

# Ensure romdisk object is linked
LDFLAGS += $(ROMDISK_OBJ)
# Add dependency to EXETARGET so it builds before link
$(EXETARGET): $(ROMDISK_OBJ)
