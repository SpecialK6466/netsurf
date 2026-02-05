#include <stdbool.h>

/*
 * Compatibility shim:
 * kos-ports SDL 1.2 references an external `irq_inside_int()` symbol.
 * In newer KOS, `irq_inside_int()` is a static inline in <kos/irq.h>, so no
 * global symbol is emitted, which breaks linking.
 *
 * Keep this in the Dreamcast frontend to avoid patching KOS or kos-ports.
 */

extern int inside_int;

int irq_inside_int(void) {
	return inside_int;
}
