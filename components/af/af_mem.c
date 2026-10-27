#include "af_mem.h"

#include <string.h>

#include "af.h"

static int chan_find(const af_mem_t *m, uint8_t code)
{
    for (unsigned i = 0; code != 0 && i < AF_MEM_SLOTS; i++) {
        if (m->s[i].pi != 0 && m->s[i].code == code) {
            return (int)i;
        }
    }
    return -1;
}

/* Recency of a against b on the wrapping use clock. */
static bool newer(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

void af_mem_init(af_mem_t *m)
{
    memset(m, 0, sizeof(*m));
}

bool af_mem_slot_valid(const af_mem_slot_t *p)
{
    if (p->pi == 0 || af_code_khz(p->code) == 0 || p->n > AF_MEM_CODES) {
        return false;
    }
    for (unsigned k = 0; k < p->n; k++) {
        if (af_code_khz(p->codes[k]) == 0 || p->codes[k] == p->code) {
            return false;
        }
    }
    return true;
}

bool af_mem_load(af_mem_t *m, unsigned slot, const af_mem_slot_t *p)
{
    if (slot >= AF_MEM_SLOTS || !af_mem_slot_valid(p)) {
        return false;
    }
    const int other = chan_find(m, p->code);
    if (other >= 0 && (unsigned)other != slot) {
        return false;
    }
    m->s[slot] = *p;
    memset(&m->s[slot].codes[p->n], 0, AF_MEM_CODES - p->n);
    if (newer(p->used, m->clock)) {
        m->clock = p->used;
    }
    return true;
}

bool af_mem_slot_same(const af_mem_slot_t *a, const af_mem_slot_t *b)
{
    return a->pi == b->pi && a->code == b->code && a->n == b->n &&
           memcmp(a->codes, b->codes, a->n) == 0;
}

int af_mem_learn(af_mem_t *m, uint16_t pi, uint32_t tuned_khz, const uint8_t *codes, uint8_t n)
{
    const uint8_t chan = af_khz_code(tuned_khz);
    if (pi == 0 || chan == 0) {
        return -1;
    }
    bool changed = false;
    int i = chan_find(m, chan);
    if (i < 0) {
        i = 0; /* an empty slot, else the least recently used */
        for (unsigned k = 0; k < AF_MEM_SLOTS; k++) {
            if (m->s[k].pi == 0) {
                i = (int)k;
                break;
            }
            if (newer(m->s[i].used, m->s[k].used)) {
                i = (int)k;
            }
        }
    }
    if (m->s[i].pi != pi || m->s[i].code != chan) {
        m->s[i] = (af_mem_slot_t){.pi = pi, .code = chan};
        changed = true;
    }
    af_mem_slot_t *p = &m->s[i];
    p->used = ++m->clock;

    bool fits = true;
    for (unsigned k = 0; k < n && fits; k++) {
        const uint8_t c = codes[k];
        if (af_code_khz(c) == 0 || c == chan || memchr(p->codes, c, p->n) != NULL) {
            continue;
        }
        if (p->n < AF_MEM_CODES) {
            p->codes[p->n++] = c;
            changed = true;
        } else {
            fits = false;
        }
    }
    if (!fits) { /* the transmitter's current list replaces the stored one */
        af_mem_slot_t r = {.pi = pi, .code = chan, .used = p->used};
        for (unsigned k = 0; k < n && r.n < AF_MEM_CODES; k++) {
            const uint8_t c = codes[k];
            if (af_code_khz(c) != 0 && c != chan && memchr(r.codes, c, r.n) == NULL) {
                r.codes[r.n++] = c;
            }
        }
        changed |= !af_mem_slot_same(&r, p);
        *p = r;
    }
    return changed ? i : -1;
}

int af_mem_replace_list(af_mem_t *m, uint16_t pi, uint32_t khz, const uint8_t *codes, uint8_t n)
{
    const int changed = af_mem_learn(m, pi, khz, NULL, 0);
    const int i = chan_find(m, af_khz_code(khz));
    if (i < 0 || pi == 0) {
        return -1;
    }
    af_mem_slot_t fresh = {.pi = pi, .code = m->s[i].code, .used = m->s[i].used};
    for (unsigned k = 0; k < n && fresh.n < AF_MEM_CODES; k++) {
        const uint8_t c = codes[k];
        if (af_code_khz(c) && c != fresh.code && !memchr(fresh.codes, c, fresh.n)) {
            fresh.codes[fresh.n++] = c;
        }
    }
    const bool differs = !af_mem_slot_same(&fresh, &m->s[i]);
    m->s[i] = fresh;
    return changed >= 0 || differs ? i : -1;
}

int af_mem_forget(af_mem_t *m, uint16_t pi, uint32_t khz)
{
    const int i = chan_find(m, af_khz_code(khz));
    if (i < 0 || m->s[i].pi != pi) {
        return -1;
    }
    memset(&m->s[i], 0, sizeof(m->s[i]));
    return i;
}

bool af_mem_lookup(const af_mem_t *m, uint32_t khz, uint16_t *pi)
{
    const int i = chan_find(m, af_khz_code(khz));
    if (i < 0) {
        return false;
    }
    *pi = m->s[i].pi;
    return true;
}

uint8_t af_mem_list(const af_mem_t *m, uint32_t khz, uint16_t pi, uint8_t *codes)
{
    return af_mem_list_reg(m, khz, pi, codes, true);
}

uint8_t af_mem_list_reg(const af_mem_t *m, uint32_t khz, uint16_t pi, uint8_t *codes, bool reg)
{
    const int i = chan_find(m, af_khz_code(khz));
    if (i < 0 || !af_pi_match(m->s[i].pi, pi, reg)) {
        return 0;
    }
    memcpy(codes, m->s[i].codes, m->s[i].n);
    return m->s[i].n;
}

uint8_t af_mem_heard(const af_mem_t *m, uint16_t pi, uint8_t *codes)
{
    return af_mem_heard_reg(m, pi, codes, true);
}

uint8_t af_mem_heard_reg(const af_mem_t *m, uint16_t pi, uint8_t *codes, bool reg)
{
    uint8_t n = 0;
    for (unsigned i = 0; pi != 0 && i < AF_MEM_SLOTS; i++) {
        if (m->s[i].pi && af_pi_match(m->s[i].pi, pi, reg)) {
            codes[n++] = m->s[i].code;
        }
    }
    return n;
}

static void put(uint8_t *codes, uint8_t *n, uint8_t c, uint8_t tuned)
{
    if (c != 0 && c != tuned && *n < AF_MEM_CODES && memchr(codes, c, *n) == NULL) {
        codes[(*n)++] = c;
    }
}

uint8_t af_mem_programme(const af_mem_t *m, uint16_t pi, uint32_t tuned_khz, uint8_t *codes)
{
    return af_mem_programme_reg(m, pi, tuned_khz, codes, true);
}

uint8_t af_mem_programme_reg(const af_mem_t *m, uint16_t pi, uint32_t tuned_khz, uint8_t *codes,
                           bool reg)
{
    const uint8_t tuned = af_khz_code(tuned_khz);
    unsigned order[AF_MEM_SLOTS]; /* the slots that carry pi, most recently used first */
    unsigned k = 0;
    for (unsigned i = 0; pi != 0 && i < AF_MEM_SLOTS; i++) {
        if (!m->s[i].pi || !af_pi_match(m->s[i].pi, pi, reg)) {
            continue;
        }
        unsigned j = k++;
        for (; j > 0 && newer(m->s[i].used, m->s[order[j - 1]].used); j--) {
            order[j] = order[j - 1];
        }
        order[j] = i;
    }
    uint8_t n = 0;
    for (unsigned j = 0; j < k; j++) {
        put(codes, &n, m->s[order[j]].code, tuned);
    }
    const int t = chan_find(m, tuned);
    if (t >= 0 && af_pi_match(m->s[t].pi, pi, reg)) {
        for (unsigned c = 0; c < m->s[t].n; c++) {
            put(codes, &n, m->s[t].codes[c], tuned);
        }
    }
    for (unsigned j = 0; j < k; j++) {
        const af_mem_slot_t *p = &m->s[order[j]];
        for (unsigned c = 0; p->code != tuned && c < p->n; c++) {
            put(codes, &n, p->codes[c], tuned);
        }
    }
    return n;
}

unsigned af_mem_count(const af_mem_t *m)
{
    unsigned n = 0;
    for (unsigned i = 0; i < AF_MEM_SLOTS; i++) {
        n += m->s[i].pi != 0;
    }
    return n;
}
