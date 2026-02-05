/*
 * Dreamcast romdisk stub.
 *
 * The Dreamcast/KOS init glue (see kos_init.c) expects a romdisk image symbol
 * named romdisk[]. During early bring-up we may not have generated the romdisk
 * binary yet, so provide an empty weak definition.
 */

#include <stdint.h>

__attribute__((weak)) const uint8_t romdisk[] = {0};
