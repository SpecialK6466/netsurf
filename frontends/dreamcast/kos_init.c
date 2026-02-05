/*
 * Dreamcast / KOS init glue
 *
 * Mounts the embedded romdisk and ensures KOS is initialised before NetSurf
 * starts. This keeps Dreamcast-specific init isolated to the frontend.
 */

#include <kos/init.h>

/* romdisk image is provided by build (genromfs + bin2o) */
extern const uint8_t romdisk[]; /* NOLINT(modernize-avoid-c-arrays) */

/* Initialise KOS with default flags and mount romdisk at /rd */
/*
 * Keep INIT_NET optional.
 *
 * The older working PoC used INIT_DEFAULT only. INIT_NET reduces available
 * heap, which can lead to early out-of-memory failures during page load.
 *
 * Enable by building with: NETSURF_DC_ENABLE_NET=1
 */
#if defined(NETSURF_DC_ENABLE_NET) && (NETSURF_DC_ENABLE_NET != 0)
KOS_INIT_FLAGS(INIT_DEFAULT | INIT_NET);
#else
KOS_INIT_FLAGS(INIT_DEFAULT);
#endif
KOS_INIT_ROMDISK(romdisk);
