/**
 * @file
 * TEF668x firmware patch images. Each image is two raw streams loaded in
 * boot state: the patch (opened with 1C 00 74) and the LUT (1C 00 75).
 * The byte arrays are machine-converted; see regen_patch.py.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    const uint8_t *patch;
    size_t patch_len;
    const uint8_t *lut;
    size_t lut_len;
} tef6686_patch_t;

extern const tef6686_patch_t tef6686_patch_v102_p224;
extern const tef6686_patch_t tef6686_patch_v205_p512;
