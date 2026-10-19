/*
 * Memory channels: numbered slots, each empty or holding one station (band, frequency, name).
 *
 * Pure C, no ESP-IDF APIs, host-tested (pio test -e native). The flash image comes back from
 * NVS as is (radio_mem), so presets_from_image() treats it as untrusted. Slot numbers here are
 * 0-based; the web UI shows slot + 1.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRESETS_MAX      40
#define PRESET_NAME_SIZE 24 /* bytes of UTF-8 including the NUL */
/* Bump when preset_t changes meaning or layout: older images are then discarded, not misread. */
#define PRESETS_VERSION  1

typedef struct {
    uint32_t khz;                /* 0 = empty slot */
    uint8_t band;                /* radio_band_t: 0 FM, 1 AM */
    char name[PRESET_NAME_SIZE]; /* "" = none; valid UTF-8 without control characters */
} preset_t;

typedef struct {
    preset_t slot[PRESETS_MAX];
} presets_t;

/* Flash image: this header, then `slots` preset_t records (trailing empty slots left out). */
typedef struct {
    uint8_t version;   /* PRESETS_VERSION */
    uint8_t slot_size; /* sizeof(preset_t) */
    uint16_t slots;
} presets_image_hdr_t;

#define PRESETS_IMAGE_MAX (sizeof(presets_image_hdr_t) + sizeof(presets_t))

void presets_clear_all(presets_t *p);
unsigned presets_used(const presets_t *p);

/* First slot holding band/khz, or -1. */
int presets_find(const presets_t *p, uint8_t band, uint32_t khz);

/*
 * Stores a station. slot < 0 picks the first slot already holding it, else the first free one.
 * The name is `name` if given; with name NULL, a slot that already holds this station keeps
 * the name it has, otherwise it gets `fallback` (NULL = none).
 * Returns the slot, or -1 (bad slot, band or frequency, or no free slot).
 */
int presets_store(presets_t *p, int slot, uint8_t band, uint32_t khz, const char *name,
                  const char *fallback);

/* false: slot out of range or empty. */
bool presets_rename(presets_t *p, int slot, const char *name);
/* false: slot out of range. Erasing an empty slot is not an error. */
bool presets_erase(presets_t *p, int slot);

/* Next used slot after `from` in direction dir (+1/-1), wrapping; from < 0 starts at the edge
 * (first slot going up, last going down). -1 if all are empty. */
int presets_next(const presets_t *p, int from, int dir);

/* Copies the printable part of src: valid UTF-8 only, no control characters, leading and
 * trailing spaces dropped, cut at a character boundary to fit. src NULL gives "". */
void presets_copy_name(char dst[PRESET_NAME_SIZE], const char *src);

/* Writes the image to `image` (PRESETS_IMAGE_MAX bytes) and returns its length. */
size_t presets_to_image(const presets_t *p, void *image);
/* Replaces *p with a stored image. Slots that do not validate come back empty, slots beyond
 * PRESETS_MAX are dropped. false (and *p all empty): not an image of this version. */
bool presets_from_image(presets_t *p, const void *image, size_t len);

#ifdef __cplusplus
}
#endif
