#include "presets.h"

#include <string.h>

static bool station_ok(uint8_t band, uint32_t khz)
{
    return band <= 1 && khz != 0;
}

/* Length of the well-formed UTF-8 sequence at s (Unicode Table 3-7), 0 if there is none.
 * Stops at the first bad byte, so it never reads past a NUL. */
static size_t utf8_len(const unsigned char *s)
{
    const unsigned char c = s[0];
    unsigned char lo = 0x80, hi = 0xBF; /* allowed range of the second byte */
    size_t n;
    if (c < 0x80) {
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF) {
        n = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
        n = 3;
        lo = c == 0xE0 ? 0xA0 : lo; /* overlong */
        hi = c == 0xED ? 0x9F : hi; /* surrogates */
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 4;
        lo = c == 0xF0 ? 0x90 : lo; /* overlong */
        hi = c == 0xF4 ? 0x8F : hi; /* beyond U+10FFFF */
    } else {
        return 0;
    }
    if (s[1] < lo || s[1] > hi) {
        return 0;
    }
    for (size_t i = 2; i < n; i++) {
        if (s[i] < 0x80 || s[i] > 0xBF) {
            return 0;
        }
    }
    return n;
}

static bool is_control(const unsigned char *s, size_t n)
{
    return (n == 1 && (s[0] < 0x20 || s[0] == 0x7F)) || (n == 2 && s[0] == 0xC2 && s[1] < 0xA0);
}

void presets_copy_name(char dst[PRESET_NAME_SIZE], const char *src)
{
    char out[PRESET_NAME_SIZE] = {0}; /* zero-filled: the flash image stays deterministic */
    size_t len = 0;
    const unsigned char *s = (const unsigned char *)(src ? src : "");

    while (*s) {
        const size_t n = utf8_len(s);
        if (n == 0 || is_control(s, n) || (len == 0 && *s == ' ')) {
            s += n ? n : 1;
            continue;
        }
        if (len + n > PRESET_NAME_SIZE - 1) {
            break;
        }
        memcpy(out + len, s, n);
        len += n;
        s += n;
    }
    while (len > 0 && out[len - 1] == ' ') {
        out[--len] = '\0';
    }
    memcpy(dst, out, PRESET_NAME_SIZE);
}

void presets_clear_all(presets_t *p)
{
    memset(p, 0, sizeof(*p));
}

unsigned presets_used(const presets_t *p)
{
    unsigned n = 0;
    for (size_t i = 0; i < PRESETS_MAX; i++) {
        n += p->slot[i].khz != 0;
    }
    return n;
}

int presets_find(const presets_t *p, uint8_t band, uint32_t khz)
{
    if (khz == 0) {
        return -1;
    }
    for (int i = 0; i < PRESETS_MAX; i++) {
        if (p->slot[i].khz == khz && p->slot[i].band == band) {
            return i;
        }
    }
    return -1;
}

int presets_store(presets_t *p, int slot, uint8_t band, uint32_t khz, const char *name,
                  const char *fallback)
{
    if (!station_ok(band, khz) || slot >= PRESETS_MAX) {
        return -1;
    }
    if (slot < 0) {
        slot = presets_find(p, band, khz);
    }
    for (int i = 0; slot < 0 && i < PRESETS_MAX; i++) {
        if (p->slot[i].khz == 0) {
            slot = i;
        }
    }
    if (slot < 0) {
        return -1;
    }
    preset_t *e = &p->slot[slot];
    const bool same = e->khz == khz && e->band == band;
    if (name) {
        presets_copy_name(e->name, name);
    } else if (!same || e->name[0] == '\0') {
        presets_copy_name(e->name, fallback);
    }
    e->khz = khz;
    e->band = band;
    return slot;
}

bool presets_rename(presets_t *p, int slot, const char *name)
{
    if (slot < 0 || slot >= PRESETS_MAX || p->slot[slot].khz == 0) {
        return false;
    }
    presets_copy_name(p->slot[slot].name, name);
    return true;
}

bool presets_erase(presets_t *p, int slot)
{
    if (slot < 0 || slot >= PRESETS_MAX) {
        return false;
    }
    memset(&p->slot[slot], 0, sizeof(p->slot[slot]));
    return true;
}

int presets_next(const presets_t *p, int from, int dir)
{
    if (dir != 1 && dir != -1) {
        return -1;
    }
    int i = from;
    if (from < 0 || from >= PRESETS_MAX) {
        i = dir > 0 ? -1 : PRESETS_MAX;
    }
    for (int n = 0; n < PRESETS_MAX; n++) {
        i = (i + dir + PRESETS_MAX) % PRESETS_MAX;
        if (p->slot[i].khz != 0) {
            return i;
        }
    }
    return -1;
}

size_t presets_to_image(const presets_t *p, void *image)
{
    uint16_t n = PRESETS_MAX;
    while (n > 0 && p->slot[n - 1].khz == 0) {
        n--;
    }
    const presets_image_hdr_t h = {
        .version = PRESETS_VERSION,
        .slot_size = (uint8_t)sizeof(preset_t),
        .slots = n,
    };
    uint8_t *out = image;
    memcpy(out, &h, sizeof(h));
    memcpy(out + sizeof(h), p->slot, n * sizeof(preset_t));
    return sizeof(h) + n * sizeof(preset_t);
}

bool presets_from_image(presets_t *p, const void *image, size_t len)
{
    presets_image_hdr_t h;
    presets_clear_all(p);
    if (image == NULL || len < sizeof(h)) {
        return false;
    }
    memcpy(&h, image, sizeof(h));
    if (h.version != PRESETS_VERSION || h.slot_size != sizeof(preset_t) ||
        len != sizeof(h) + (size_t)h.slots * sizeof(preset_t)) {
        return false;
    }
    const uint8_t *in = (const uint8_t *)image + sizeof(h);
    for (size_t i = 0; i < h.slots && i < PRESETS_MAX; i++) {
        preset_t e;
        memcpy(&e, in + i * sizeof(e), sizeof(e));
        e.name[PRESET_NAME_SIZE - 1] = '\0';
        if (station_ok(e.band, e.khz)) {
            p->slot[i].khz = e.khz;
            p->slot[i].band = e.band;
            presets_copy_name(p->slot[i].name, e.name);
        }
    }
    return true;
}
