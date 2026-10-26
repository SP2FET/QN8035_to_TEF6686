#include "af_mem.h"

#include <string.h>

#include "af.h"

static int prog_find(const af_mem_t *m, uint16_t pi)
{
    for (unsigned i = 0; i < AF_MEM_PROGS; i++) {
        if (pi != 0 && m->p[i].pi == pi) {
            return (int)i;
        }
    }
    return -1;
}

static int code_find(const af_mem_prog_t *p, uint8_t code)
{
    for (unsigned i = 0; i < p->n; i++) {
        if (p->code[i] == code) {
            return (int)i;
        }
    }
    return -1;
}

/* Index of code in p, added if there is room; -1 if it is not there and the list is full. */
static int code_add(af_mem_prog_t *p, uint8_t code, bool *changed)
{
    const int i = code_find(p, code);
    if (i >= 0 || p->n >= AF_MEM_CODES) {
        return i;
    }
    p->code[p->n] = code;
    *changed = true;
    return p->n++;
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

bool af_mem_prog_valid(const af_mem_prog_t *p)
{
    if (p->pi == 0 || p->n > AF_MEM_CODES || (p->heard >> p->n) != 0) {
        return false;
    }
    for (unsigned k = 0; k < p->n; k++) {
        if (af_code_khz(p->code[k]) == 0) {
            return false;
        }
    }
    return true;
}

bool af_mem_load(af_mem_t *m, unsigned slot, const af_mem_prog_t *p)
{
    if (slot >= AF_MEM_PROGS || !af_mem_prog_valid(p)) {
        return false;
    }
    const int other = prog_find(m, p->pi);
    if (other >= 0 && (unsigned)other != slot) {
        return false;
    }
    m->p[slot] = *p;
    memset(&m->p[slot].code[p->n], 0, AF_MEM_CODES - p->n);
    if (newer(p->used, m->clock)) {
        m->clock = p->used;
    }
    return true;
}

bool af_mem_prog_same(const af_mem_prog_t *a, const af_mem_prog_t *b)
{
    return a->pi == b->pi && a->n == b->n && a->heard == b->heard &&
           memcmp(a->code, b->code, a->n) == 0;
}

int af_mem_learn(af_mem_t *m, uint16_t pi, uint32_t tuned_khz, const uint8_t *codes, uint8_t n)
{
    if (pi == 0) {
        return -1;
    }
    bool changed = false;
    int i = prog_find(m, pi);
    if (i < 0) {
        i = 0; /* an empty slot, else the least recently used */
        for (unsigned k = 0; k < AF_MEM_PROGS; k++) {
            if (m->p[k].pi == 0) {
                i = (int)k;
                break;
            }
            if (newer(m->p[i].used, m->p[k].used)) {
                i = (int)k;
            }
        }
        m->p[i] = (af_mem_prog_t){.pi = pi};
        changed = true;
    }
    af_mem_prog_t *p = &m->p[i];
    p->used = ++m->clock;
    for (unsigned k = 0; k < n; k++) {
        if (af_code_khz(codes[k]) != 0) {
            code_add(p, codes[k], &changed);
        }
    }
    const uint8_t tuned = af_khz_code(tuned_khz);
    if (tuned != 0) {
        const int k = code_add(p, tuned, &changed);
        if (k >= 0 && !(p->heard & (1u << k))) {
            p->heard |= 1u << k;
            changed = true;
        }
    }
    return changed ? i : -1;
}

bool af_mem_lookup(const af_mem_t *m, uint32_t khz, uint16_t *pi)
{
    const uint8_t code = af_khz_code(khz);
    if (code == 0) {
        return false;
    }
    int best = -1;
    bool best_heard = false;
    for (unsigned i = 0; i < AF_MEM_PROGS; i++) {
        const af_mem_prog_t *p = &m->p[i];
        const int k = p->pi != 0 ? code_find(p, code) : -1;
        if (k < 0) {
            continue;
        }
        const bool heard = (p->heard & (1u << k)) != 0;
        if (best < 0 || (heard && !best_heard) ||
            (heard == best_heard && newer(p->used, m->p[best].used))) {
            best = (int)i;
            best_heard = heard;
        }
    }
    if (best < 0) {
        return false;
    }
    *pi = m->p[best].pi;
    return true;
}

uint8_t af_mem_codes(const af_mem_t *m, uint16_t pi, uint8_t *codes)
{
    const int i = prog_find(m, pi);
    if (i < 0) {
        return 0;
    }
    memcpy(codes, m->p[i].code, m->p[i].n);
    return m->p[i].n;
}

uint8_t af_mem_heard(const af_mem_t *m, uint16_t pi, uint8_t *codes)
{
    const int i = prog_find(m, pi);
    if (i < 0) {
        return 0;
    }
    uint8_t n = 0;
    for (unsigned k = 0; k < m->p[i].n; k++) {
        if (m->p[i].heard & (1u << k)) {
            codes[n++] = m->p[i].code[k];
        }
    }
    return n;
}

unsigned af_mem_count(const af_mem_t *m)
{
    unsigned n = 0;
    for (unsigned i = 0; i < AF_MEM_PROGS; i++) {
        n += m->p[i].pi != 0;
    }
    return n;
}
