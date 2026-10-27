/* The runner extracts the actual snapshot function; platform storage/time are mocked. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "sysmon.h"

#define CLOCK_SET_AFTER 1600000000
#define SLOT_MAGIC 0x51544546u
typedef struct {
    uint32_t magic, seq;
    uint8_t elf[8];
    sysmon_state_t st;
    uint32_t crc;
} slot_t;
static slot_t s_slots[2];
static struct {sysmon_state_t last; uint32_t seq;} s;
static unsigned reads, mode;
static uint8_t elf_id[8];
static const uint8_t *build_id(void) {return elf_id;}
static uint32_t slot_crc(const slot_t *sl) {return 0;}
static int64_t esp_timer_get_time(void) {return 1000000;}
bool qn8035_slave_snapshot(qn8035_emu_t *out) {
    memset(out, 0, sizeof(*out));
    out->revision = mode == 1 ? ++reads : 2;
    out->op = mode == 3 ? QN8035_OP_TUNE : QN8035_OP_NONE;
    out->tuned_ch = 700;
    return true;
}
bool radio_get_restart_settings(radio_settings_t *out) {
    memset(out, 0, sizeof(*out));
    out->khz = 95000;
    return mode != 2;
}
#include "snapshot_under_test.inc"

static void init(unsigned m) {
    memset(&s, 0, sizeof(s));mode=m;reads=0;
    s.last.radio_valid=s.last.qn_valid=true;
    s.last.radio.khz=90000;s.last.qn.revision=1;s.last.qn.tuned_ch=600;
}
int main(void) {
    for (unsigned m=1;m<=3;m++) {
        init(m);snapshot();
        assert(s.last.radio.khz==90000 && s.last.qn.revision==1);
        assert(s_slots[s.seq&1].st.qn.tuned_ch==600);
    }
    puts("PASS racing, unsettled and pending-operation snapshots retain the previous pair");
    init(0);snapshot();
    assert(s.last.radio.khz==95000 && s.last.qn.revision==2);
    puts("PASS settled stable radio/model pair is recorded together");
}
