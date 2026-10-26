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

static void avg_add(af_avg_t *a, int16_t x, uint8_t span)
{
    if (a->n < span) {
        a->n++;
    }
    a->v16 += ((int32_t)x * 16 - a->v16) / a->n;
}

static int16_t avg_get(const af_avg_t *a)
{
    return a->n ? (int16_t)(a->v16 / 16) : AF_SCORE_NONE;
}

static bool banned(const af_entry_t *e, uint32_t now_ms)
{
    return e->banned && !due(now_ms, e->banned_until_ms);
}

/* Lost its last trial and waits before the next. */
static bool waiting(const af_entry_t *e, uint32_t now_ms)
{
    return e->losses > 0 && !due(now_ms, e->retry_until_ms);
}

/* One round of checks plus three intervals: a candidate that is measured again first delays the
 * others by up to AF_CONFIRM - 1 intervals. */
static uint32_t fresh_ms(const af_state_t *s, const af_params_t *p)
{
    return (uint32_t)(s->n + 3u) * p->interval_ms;
}

static bool fresh(const af_entry_t *e, uint32_t limit_ms, uint32_t now_ms)
{
    return e->measured && (uint32_t)(now_ms - e->measured_ms) <= limit_ms;
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

static af_entry_t new_entry(uint32_t khz)
{
    af_entry_t e = {.khz = khz,
                    .score = AF_SCORE_NONE,
                    .last = AF_SCORE_NONE,
                    .level_avg = AF_SCORE_NONE,
                    .trial_score = AF_SCORE_NONE};
    for (unsigned i = 0; i < AF_CONFIRM; i++) {
        e.usn[i] = UINT16_MAX;
    }
    return e;
}

static bool entry_trusted(const af_entry_t *e, uint32_t now_ms)
{
    return !banned(e, now_ms) &&
           (e->heard || (e->verified && (uint32_t)(now_ms - e->verified_ms) < AF_TRUST_MS));
}

static void add(af_state_t *s, const af_entry_t *e)
{
    if (e->khz == 0 || e->khz == s->tuned.khz || s->n >= AF_MAX || find(s, e->khz) >= 0) {
        return;
    }
    s->e[s->n++] = *e;
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

/* The tuned station's averages start again (another station plays). */
static void tuned_restart(af_state_t *s)
{
    s->cur = (af_avg_t){0};
    s->cur_level = (af_avg_t){0};
    s->ref_n = 0;
    s->ref_pos = 0;
}

/* Mean of the tuned station's last settled scores; the smoothed score while there are none. */
static int16_t ref_mean(const af_state_t *s)
{
    if (s->ref_n == 0) {
        return af_current(s);
    }
    int32_t sum = 0;
    for (unsigned i = 0; i < s->ref_n; i++) {
        sum += s->ref[i];
    }
    return (int16_t)(sum / s->ref_n);
}

uint32_t af_code_khz(uint8_t code)
{
    return (code >= 1 && code <= 204) ? 87500u + 100u * code : 0;
}

uint8_t af_khz_code(uint32_t khz)
{
    if (khz < 87600 || khz > 107900 || (khz - 87500u) % 100u != 0) {
        return 0;
    }
    return (uint8_t)((khz - 87500u) / 100u);
}

int16_t af_score(int16_t level, uint16_t usn, uint16_t wam, int16_t offset)
{
    if (abs(offset) > AF_OFFSET_LIMIT) {
        return AF_SCORE_NONE;
    }
    /* level -200..1200, usn/wam 0..1000: never reaches AF_SCORE_NONE */
    return (int16_t)((int32_t)level - usn - wam);
}

void af_reset(af_state_t *s, uint32_t tuned_khz, uint32_t now_ms)
{
    memset(s, 0, sizeof(*s));
    s->tuned = new_entry(tuned_khz);
    s->hold_until_ms = now_ms + AF_HOLD_MS;
    s->next_check_ms = now_ms;
}

void af_sync(af_state_t *s, const uint8_t *codes, uint8_t n)
{
    for (unsigned i = 0; i < n; i++) {
        const af_entry_t e = new_entry(af_code_khz(codes[i]));
        add(s, &e);
    }
}

void af_quality(af_state_t *s, int16_t score, int16_t level)
{
    if (score == AF_SCORE_NONE) {
        return;
    }
    if (s->trial) {
        s->trial_sum += score;
        s->trial_lsum += level;
        if (s->trial_n < UINT16_MAX) {
            s->trial_n++;
        }
        return;
    }
    avg_add(&s->cur, score, AF_CUR_N);
    avg_add(&s->cur_level, level, AF_CUR_N);
    s->ref[s->ref_pos] = score;
    s->ref_pos = (uint8_t)((s->ref_pos + 1u) % AF_REF_N);
    if (s->ref_n < AF_REF_N) {
        s->ref_n++;
    }
}

int16_t af_current(const af_state_t *s)
{
    return avg_get(&s->cur);
}

int16_t af_current_level(const af_state_t *s)
{
    return avg_get(&s->cur_level);
}

bool af_poor(const af_state_t *s, const af_params_t *p)
{
    return s->cur.n > 0 && af_current(s) < p->trigger;
}

int16_t af_need_level(const af_state_t *s, const af_params_t *p)
{
    const int16_t lvl = af_current_level(s);
    if (lvl == AF_SCORE_NONE) {
        return AF_SCORE_NONE;
    }
    const int32_t need = (int32_t)lvl - p->window;
    return (int16_t)(need > p->min_level ? need : p->min_level);
}

uint32_t af_next_check(af_state_t *s, const af_params_t *p, uint32_t now_ms)
{
    if (!p->enable || s->trial || s->n == 0 || !af_poor(s, p) || !due(now_ms, s->hold_until_ms) ||
        !due(now_ms, s->next_check_ms)) {
        return 0;
    }
    s->next_check_ms = now_ms + p->interval_ms;
    for (unsigned i = 0; i < s->n; i++) {
        af_entry_t *e = &s->e[i];
        if (e->banned && due(now_ms, e->banned_until_ms)) {
            e->banned = false;
        }
    }
    const uint32_t limit = fresh_ms(s, p);
    for (unsigned i = 0; i < s->n; i++) {
        const af_entry_t *e = &s->e[i];
        if (!e->banned && !waiting(e, now_ms) && e->streak > 0 && e->streak < AF_CONFIRM &&
            fresh(e, limit, now_ms)) {
            return e->khz; /* would be tried: confirm it before the round goes on */
        }
    }
    for (unsigned k = 0; k < s->n; k++) {
        const unsigned i = (s->next + k) % s->n;
        if (!s->e[i].banned) {
            s->next = (uint8_t)((i + 1) % s->n);
            return s->e[i].khz;
        }
    }
    return 0; /* all banned */
}

void af_measured(af_state_t *s, const af_params_t *p, uint32_t khz, int16_t score, int16_t level,
                 uint16_t usn, uint32_t now_ms)
{
    const int i = find(s, khz);
    if (i < 0) {
        return;
    }
    af_entry_t *e = &s->e[i];
    /* Off frequency: maybe another station. Long unmeasured: the car has moved on. */
    if (score == AF_SCORE_NONE ||
        (e->measured && (uint32_t)(now_ms - e->measured_ms) > 2u * fresh_ms(s, p))) {
        e->avg = (af_avg_t){0};
        e->lavg = (af_avg_t){0};
        e->streak = 0;
        for (unsigned k = 0; k < AF_CONFIRM; k++) {
            e->usn[k] = UINT16_MAX;
        }
    }
    memmove(&e->usn[1], &e->usn[0], (AF_CONFIRM - 1) * sizeof(e->usn[0]));
    e->usn[0] = score == AF_SCORE_NONE ? UINT16_MAX : usn;
    const int16_t need = af_need_level(s, p);
    if (score != AF_SCORE_NONE && need != AF_SCORE_NONE && level >= need) {
        e->streak = e->streak < UINT8_MAX ? (uint8_t)(e->streak + 1) : UINT8_MAX;
    } else {
        e->streak = 0;
    }
    if (score != AF_SCORE_NONE) {
        avg_add(&e->avg, score, AF_AVG_N);
        avg_add(&e->lavg, level, AF_AVG_N);
    }
    e->score = avg_get(&e->avg);
    e->level_avg = avg_get(&e->lavg);
    e->last = score;
    e->level = level;
    e->measured_ms = now_ms;
    e->measured = true;
}

uint16_t af_usn_min(const af_entry_t *e)
{
    uint16_t m = UINT16_MAX;
    for (unsigned i = 0; i < AF_CONFIRM; i++) {
        m = e->usn[i] < m ? e->usn[i] : m;
    }
    return m;
}

uint32_t af_dwell_left(const af_state_t *s, uint32_t now_ms)
{
    if (!s->switched) {
        return 0;
    }
    const uint32_t end = s->switched_ms + s->dwell_ms;
    return due(now_ms, end) ? 0 : end - now_ms;
}

uint32_t af_pick(const af_state_t *s, const af_params_t *p, uint32_t now_ms)
{
    if (!p->enable || s->trial || !af_poor(s, p) || af_dwell_left(s, now_ms) > 0) {
        return 0;
    }
    const int16_t need = af_need_level(s, p);
    const uint32_t limit = fresh_ms(s, p);
    int16_t best = need;
    uint32_t khz = 0;
    const bool check_ok = !s->checked || due(now_ms, s->check_ms + p->check_gap_ms);
    for (unsigned i = 0; i < s->n; i++) {
        const af_entry_t *e = &s->e[i];
        if (e->streak < AF_CONFIRM || !fresh(e, limit, now_ms) || banned(e, now_ms) ||
            waiting(e, now_ms)) {
            continue;
        }
        if (!entry_trusted(e, now_ms) && (!check_ok || af_usn_min(e) > p->check_usn)) {
            continue;
        }
        if (e->level_avg > best || (khz == 0 && e->level_avg == best)) {
            best = e->level_avg;
            khz = e->khz;
        }
    }
    return khz;
}

bool af_trusted(const af_state_t *s, uint32_t khz, uint32_t now_ms)
{
    const int i = find(s, khz);
    if (i < 0) {
        return false;
    }
    return entry_trusted(&s->e[i], now_ms);
}

void af_set_heard(af_state_t *s, const uint8_t *codes, uint8_t n)
{
    for (unsigned i = 0; i <= s->n; i++) {
        af_entry_t *e = i < s->n ? &s->e[i] : &s->tuned;
        const uint8_t code = af_khz_code(e->khz);
        e->heard = false;
        for (unsigned k = 0; code && k < n; k++) {
            e->heard = e->heard || codes[k] == code;
        }
    }
}

void af_check_started(af_state_t *s, uint32_t now_ms)
{
    s->checked = true;
    s->check_ms = now_ms;
}

void af_verified(af_state_t *s, uint32_t khz, uint32_t now_ms)
{
    const int i = find(s, khz);
    if (i >= 0) {
        s->e[i].verified = true;
        s->e[i].verified_ms = now_ms;
    }
}

uint32_t af_ban(af_state_t *s, uint32_t khz, uint32_t now_ms, uint32_t base_ms, bool other_pi)
{
    const int i = find(s, khz);
    if (i < 0) {
        return 0;
    }
    af_entry_t *e = &s->e[i];
    unsigned shift = e->strikes + (other_pi ? AF_BAN_OTHER_PI_SHIFT : 0u);
    if (shift > AF_BAN_MAX_SHIFT) {
        shift = AF_BAN_MAX_SHIFT;
    }
    const uint32_t ms = base_ms << shift;
    if (e->strikes < UINT8_MAX) {
        e->strikes++;
    }
    e->banned = true;
    e->banned_until_ms = now_ms + ms;
    e->verified = false;
    return ms;
}

void af_trial_begin(af_state_t *s, const af_params_t *p, uint32_t khz, uint32_t now_ms)
{
    s->trial = true;
    s->trial_khz = khz;
    s->trial_end_ms = now_ms + p->trial_ms;
    s->trial_ref = ref_mean(s);
    s->trial_n = 0;
    s->trial_sum = 0;
    s->trial_lsum = 0;
}

bool af_trial_due(const af_state_t *s, const af_params_t *p, uint32_t now_ms)
{
    if (!s->trial || !due(now_ms, s->trial_end_ms)) {
        return false;
    }
    return s->trial_n >= AF_TRIAL_MIN_SAMPLES || due(now_ms, s->trial_end_ms + p->trial_ms);
}

int16_t af_trial_mean(const af_state_t *s)
{
    return s->trial_n ? (int16_t)(s->trial_sum / s->trial_n) : AF_SCORE_NONE;
}

af_trial_result_t af_trial_end(af_state_t *s, const af_params_t *p, uint32_t now_ms)
{
    af_trial_result_t r = {.khz = s->trial_khz,
                           .from_khz = s->tuned.khz,
                           .mean = af_trial_mean(s),
                           .ref = s->trial_ref,
                           .n = s->trial_n};
    if (!s->trial) {
        r.khz = 0;
        return r;
    }
    s->trial = false;
    r.won = r.n > 0 && (r.ref == AF_SCORE_NONE || (int32_t)r.mean >= (int32_t)r.ref + p->gain);
    if (r.won) {
        const int16_t level = (int16_t)(s->trial_lsum / r.n);
        af_switched(s, p, r.from_khz, r.khz, now_ms);
        /* The trial's samples are the new station's first: it is judged from them at once. */
        const uint8_t n = (uint8_t)(r.n < AF_CUR_N ? r.n : AF_CUR_N);
        s->cur = (af_avg_t){.v16 = (int32_t)r.mean * 16, .n = n};
        s->cur_level = (af_avg_t){.v16 = (int32_t)level * 16, .n = n};
        s->tuned.trial_score = r.mean;
        s->tuned.losses = 0;
        return r;
    }
    const int i = find(s, r.khz);
    if (i >= 0) {
        af_entry_t *e = &s->e[i];
        e->trial_score = r.mean;
        if (e->losses < UINT8_MAX) {
            e->losses++;
        }
        const unsigned shift = e->losses - 1u < AF_RETRY_MAX_SHIFT ? e->losses - 1u : AF_RETRY_MAX_SHIFT;
        r.retry_ms = p->retry_ms << shift;
        e->retry_until_ms = now_ms + r.retry_ms;
        e->streak = 0;
    }
    s->hold_until_ms = now_ms + AF_HOLD_MS;
    return r;
}

void af_trial_abort(af_state_t *s, uint32_t now_ms)
{
    s->trial = false;
    s->hold_until_ms = now_ms + AF_HOLD_MS;
}

void af_switched(af_state_t *s, const af_params_t *p, uint32_t from_khz, uint32_t to_khz,
                 uint32_t now_ms)
{
    af_entry_t tuned = new_entry(to_khz);
    const int i = find(s, to_khz);
    if (i >= 0) {
        tuned = s->e[i];
        drop(s, (unsigned)i);
    }
    tuned.banned = false;

    af_entry_t from = s->tuned;
    from.khz = from_khz;
    from.verified = true;
    from.verified_ms = now_ms;
    from.banned = false;
    s->tuned = tuned;
    add(s, &from);
    for (unsigned k = 0; k < s->n; k++) {
        s->e[k].streak = 0;
    }

    s->trial = false;
    tuned_restart(s);
    const bool quick = s->switched && (uint32_t)(now_ms - s->switched_ms) < 2u * s->dwell_ms;
    const uint32_t cap = p->dwell_ms << AF_DWELL_MAX_SHIFT;
    s->dwell_ms = quick ? (2u * s->dwell_ms < cap ? 2u * s->dwell_ms : cap) : p->dwell_ms;
    s->switched = true;
    s->switched_ms = now_ms;
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
