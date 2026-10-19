#include "af.h"

#include <stdlib.h>
#include <string.h>

/* 0.1 kHz. PE5PVB TEF668X.cpp rejects AF samples beyond 12.5 kHz; programme audio alone moves
 * a single offset reading by up to ~9 kHz on a clean station. */
#define AF_OFFSET_LIMIT 125

static bool due(uint32_t now, uint32_t t)
{
    return (int32_t)(now - t) >= 0;
}

static int find(const af_state_t *s, uint32_t khz)
{
    for (unsigned i = 0; i < s->n; i++) {
        if (s->e[i].khz == khz) {
            return (int)i;
        }
    }
    return -1;
}

static void add(af_state_t *s, uint32_t khz)
{
    if (khz == 0 || s->n >= AF_MAX || find(s, khz) >= 0) {
        return;
    }
    s->e[s->n++] = (af_entry_t){.khz = khz, .score = AF_SCORE_NONE};
}

static void drop(af_state_t *s, unsigned i)
{
    memmove(&s->e[i], &s->e[i + 1], (s->n - i - 1) * sizeof(s->e[0]));
    s->n--;
    if (s->next > i) {
        s->next--;
    }
    if (s->next >= s->n) {
        s->next = 0;
    }
}

uint32_t af_code_khz(uint8_t code)
{
    return (code >= 1 && code <= 204) ? 87500u + 100u * code : 0;
}

int16_t af_score(int16_t level, uint16_t usn, uint16_t wam, int16_t offset)
{
    if (abs(offset) > AF_OFFSET_LIMIT) {
        return AF_SCORE_NONE;
    }
    /* level -200..1200, usn/wam 0..1000: never reaches AF_SCORE_NONE */
    return (int16_t)((int32_t)level - usn - wam);
}

void af_reset(af_state_t *s, uint32_t now_ms)
{
    memset(s, 0, sizeof(*s));
    s->hold_until_ms = now_ms + AF_HOLD_MS;
    s->next_check_ms = now_ms;
}

void af_sync(af_state_t *s, const uint8_t *codes, uint8_t n, uint32_t tuned_khz)
{
    for (unsigned i = 0; i < n; i++) {
        const uint32_t khz = af_code_khz(codes[i]);
        if (khz != tuned_khz) {
            add(s, khz);
        }
    }
}

void af_quality(af_state_t *s, int16_t score)
{
    if (score == AF_SCORE_NONE) {
        return;
    }
    if (!s->have_avg) {
        s->avg8 = (int32_t)score * 8;
        s->have_avg = true;
    } else {
        s->avg8 += score - s->avg8 / 8; /* ~8 samples time constant */
    }
}

int16_t af_current(const af_state_t *s)
{
    return s->have_avg ? (int16_t)(s->avg8 / 8) : AF_SCORE_NONE;
}

bool af_poor(const af_state_t *s, const af_params_t *p)
{
    return s->have_avg && af_current(s) < p->trigger;
}

uint32_t af_next_check(af_state_t *s, const af_params_t *p, uint32_t now_ms)
{
    if (!p->enable || s->n == 0 || !af_poor(s, p) || !due(now_ms, s->hold_until_ms) ||
        !due(now_ms, s->next_check_ms)) {
        return 0;
    }
    s->next_check_ms = now_ms + p->interval_ms;
    for (unsigned i = 0; i < s->n; i++) {
        const unsigned k = (s->next + i) % s->n;
        af_entry_t *e = &s->e[k];
        if (e->banned) {
            if (!due(now_ms, e->banned_until_ms)) {
                continue;
            }
            e->banned = false;
        }
        s->next = (uint8_t)((k + 1) % s->n);
        return e->khz;
    }
    return 0; /* all banned */
}

void af_measured(af_state_t *s, uint32_t khz, int16_t score, int16_t level, uint32_t now_ms)
{
    const int i = find(s, khz);
    if (i < 0) {
        return;
    }
    af_entry_t *e = &s->e[i];
    e->score = score;
    e->level = level;
    e->measured_ms = now_ms;
    e->measured = true;
}

uint32_t af_pick(const af_state_t *s, const af_params_t *p, uint32_t now_ms)
{
    if (!p->enable || !s->have_avg) {
        return 0;
    }
    const uint32_t fresh_ms = (uint32_t)(s->n + 1) * p->interval_ms;
    /* candidates must reach both cur + margin and min_score */
    int32_t best = (int32_t)af_current(s) + p->margin;
    if (best < p->min_score) {
        best = p->min_score;
    }
    best--;
    uint32_t khz = 0;
    for (unsigned i = 0; i < s->n; i++) {
        const af_entry_t *e = &s->e[i];
        if (!e->measured || e->score == AF_SCORE_NONE || (uint32_t)(now_ms - e->measured_ms) > fresh_ms ||
            (e->banned && !due(now_ms, e->banned_until_ms))) {
            continue;
        }
        if (e->score > best) {
            best = e->score;
            khz = e->khz;
        }
    }
    return khz;
}

uint32_t af_ban(af_state_t *s, uint32_t khz, uint32_t now_ms, uint32_t base_ms)
{
    const int i = find(s, khz);
    if (i < 0) {
        return 0;
    }
    af_entry_t *e = &s->e[i];
    const unsigned shift = e->strikes < AF_BAN_MAX_SHIFT ? e->strikes : AF_BAN_MAX_SHIFT;
    const uint32_t ms = base_ms << shift;
    if (e->strikes < UINT8_MAX) {
        e->strikes++;
    }
    e->banned = true;
    e->banned_until_ms = now_ms + ms;
    return ms;
}

void af_switched(af_state_t *s, uint32_t from_khz, uint32_t to_khz, uint32_t now_ms)
{
    int16_t score = af_current(s);
    const int i = find(s, to_khz);
    if (i >= 0) {
        if (s->e[i].measured && s->e[i].score != AF_SCORE_NONE) {
            score = s->e[i].score;
        }
        drop(s, (unsigned)i);
    }
    add(s, from_khz);
    if (score != AF_SCORE_NONE) {
        s->avg8 = (int32_t)score * 8;
        s->have_avg = true;
    }
    s->hold_until_ms = now_ms + AF_HOLD_MS;
    s->next_check_ms = now_ms;
}

bool af_pi_match(uint16_t want, uint16_t got, bool reg)
{
    if (want == got) {
        return true;
    }
    if (reg) {
        return false;
    }
    const unsigned a = (want >> 8) & 0xFu, b = (got >> 8) & 0xFu;
    return a >= 4 && b >= 4 && (want & 0xF0FFu) == (got & 0xF0FFu);
}
