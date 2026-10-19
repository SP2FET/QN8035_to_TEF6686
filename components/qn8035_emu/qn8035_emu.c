#include "qn8035_emu.h"

#include <string.h>

#define REG_MASK (QN8035_REG_COUNT - 1u)

typedef struct {
    qn8035_evt_t *buf;
    size_t n;
    size_t max;
} evt_sink_t;

static void emit(evt_sink_t *sink, qn8035_evt_t evt)
{
    if (sink->n < sink->max) {
        sink->buf[sink->n++] = evt;
    }
}

static void set_fsm(qn8035_emu_t *emu, uint8_t fsm)
{
    uint8_t *st = &emu->regs[QN8035_REG_STATUS1];
    *st = (uint8_t)((*st & ~QN8035_STATUS1_FSM_MASK) | (fsm << QN8035_STATUS1_FSM_SHIFT));
}

static void load_defaults(qn8035_emu_t *emu)
{
    memset(emu, 0, sizeof(*emu));
    uint8_t *r = emu->regs;
    /* Reset values from the Rev 0.08 register tables. CH/CH_START/CH_STOP
     * decode to 88.0 / 87.0 / 108.0 MHz, 100 kHz scan step. */
    r[QN8035_REG_CID1] = QN8035_EMU_CID1;
    r[QN8035_REG_CID2] = QN8035_EMU_CID2;
    r[QN8035_REG_CH] = 0x30;
    r[QN8035_REG_CH_START] = 0x1C;
    r[QN8035_REG_CH_STOP] = 0xC0;
    r[QN8035_REG_CH_STEP] = 0x7A;
    r[QN8035_REG_VOL_CTL] = 0x47;
    r[QN8035_REG_XTAL_DIV0] = 0x01;
    r[QN8035_REG_XTAL_DIV1] = 0x08;
    r[QN8035_REG_XTAL_DIV2] = 0x5C;
    r[QN8035_REG_INT_CTRL] = 0x20;
    set_fsm(emu, QN8035_FSM_IDLE);
    emu->tuned_ch = 0xFFFF; /* nothing tuned yet */
}

void qn8035_emu_init(qn8035_emu_t *emu)
{
    load_defaults(emu);
}

static bool is_read_only(uint8_t reg)
{
    switch (reg) {
    case QN8035_REG_SNR:
    case QN8035_REG_RSSISIG:
    case QN8035_REG_STATUS1:
    case QN8035_REG_CID1:
    case QN8035_REG_CID2:
    case QN8035_REG_STATUS2:
        return true;
    default:
        return reg >= QN8035_REG_RDSD0 && reg <= QN8035_REG_RDSD7;
    }
}

static uint16_t current_ch(const qn8035_emu_t *emu)
{
    const uint8_t *r = emu->regs;
    return (uint16_t)(r[QN8035_REG_CH] | ((r[QN8035_REG_CH_STEP] & 0x3u) << 8));
}

static uint32_t scan_step_khz(uint8_t ch_step)
{
    switch ((ch_step >> QN8035_CH_STEP_FSTEP_SHIFT) & 0x3u) {
    case 0:  return 50;
    case 2:  return 200;
    default: return 100; /* 01, and reserved 11 */
    }
}

static void on_system1(qn8035_emu_t *emu, uint8_t old, evt_sink_t *sink)
{
    uint8_t *r = emu->regs;
    const uint8_t v = r[QN8035_REG_SYSTEM1];

    if (v & QN8035_SYSTEM1_SWRST) {
        load_defaults(emu);
        emit(sink, (qn8035_evt_t){.type = QN8035_EVT_RESET});
        return;
    }
    r[QN8035_REG_SYSTEM1] &= (uint8_t)~QN8035_SYSTEM1_RECAL; /* self-clearing */

    const bool rx = (v & QN8035_SYSTEM1_RXREQ) && !(v & QN8035_SYSTEM1_STNBY);
    const bool powered_up = rx && !emu->receiving;
    if (rx != emu->receiving) {
        emu->receiving = rx;
        set_fsm(emu, rx ? QN8035_FSM_RECEIVING
                        : (v & QN8035_SYSTEM1_STNBY) ? QN8035_FSM_STBY : QN8035_FSM_IDLE);
        emit(sink, (qn8035_evt_t){.type = QN8035_EVT_POWER, .on = rx});
    }
    if ((v ^ old) & QN8035_SYSTEM1_RDSEN) {
        emit(sink, (qn8035_evt_t){.type = QN8035_EVT_RDS_ENABLE,
                                  .on = (v & QN8035_SYSTEM1_RDSEN) != 0});
    }
    if ((v ^ old) & QN8035_SYSTEM1_FORCE_MO) {
        emit(sink, (qn8035_evt_t){.type = QN8035_EVT_FORCE_MONO,
                                  .on = (v & QN8035_SYSTEM1_FORCE_MO) != 0});
    }
    if (!rx) {
        r[QN8035_REG_SYSTEM1] &= (uint8_t)~QN8035_SYSTEM1_CHSC; /* nothing to do in standby */
        return;
    }

    const uint16_t ch = current_ch(emu);
    if (v & QN8035_SYSTEM1_CHSC) {
        /* CHSC stays set until the bridge reports completion. */
        if (v & QN8035_SYSTEM1_CCA_CH_DIS) {
            emit(sink, (qn8035_evt_t){.type = QN8035_EVT_TUNE, .khz = qn8035_ch_to_khz(ch)});
        } else {
            const uint8_t step = r[QN8035_REG_CH_STEP];
            const uint16_t start = (uint16_t)(r[QN8035_REG_CH_START] |
                                              (((step >> QN8035_CH_STEP_START_SHIFT) & 3u) << 8));
            const uint16_t stop = (uint16_t)(r[QN8035_REG_CH_STOP] |
                                             (((step >> QN8035_CH_STEP_STOP_SHIFT) & 3u) << 8));
            set_fsm(emu, QN8035_FSM_RXCCA);
            emit(sink, (qn8035_evt_t){.type = QN8035_EVT_SEEK,
                                      .seek = {.start_khz = qn8035_ch_to_khz(start),
                                               .stop_khz = qn8035_ch_to_khz(stop),
                                               .step_khz = scan_step_khz(step)}});
        }
    } else if ((v & QN8035_SYSTEM1_CCA_CH_DIS) && (powered_up || ch != emu->tuned_ch)) {
        /* Some drivers retune by rewriting SYSTEM1 without CHSC. */
        emit(sink, (qn8035_evt_t){.type = QN8035_EVT_TUNE, .khz = qn8035_ch_to_khz(ch)});
    }
}

static void on_vol_ctl(qn8035_emu_t *emu, uint8_t old, evt_sink_t *sink)
{
    const uint8_t v = emu->regs[QN8035_REG_VOL_CTL];
    const uint8_t changed = v ^ old;

    if (changed & QN8035_VOL_MUTE_EN) {
        emit(sink, (qn8035_evt_t){.type = QN8035_EVT_MUTE, .on = (v & QN8035_VOL_MUTE_EN) != 0});
    }
    if (changed & QN8035_VOL_TC) {
        emit(sink, (qn8035_evt_t){.type = QN8035_EVT_DEEMPHASIS,
                                  .us = (v & QN8035_VOL_TC) ? 75 : 50});
    }
    if (changed & (QN8035_VOL_GAIN_DIG_MASK | QN8035_VOL_GAIN_ANA_MASK)) {
        const int ana_db = ((int)(v & QN8035_VOL_GAIN_ANA_MASK) - 7) * 6;
        int dig = (v & QN8035_VOL_GAIN_DIG_MASK) >> QN8035_VOL_GAIN_DIG_SHIFT;
        dig = dig > 5 ? 5 : dig;
        emit(sink, (qn8035_evt_t){.type = QN8035_EVT_VOLUME, .db = (int8_t)(ana_db - dig)});
    }
}

size_t qn8035_emu_write(qn8035_emu_t *emu, const uint8_t *data, size_t len, qn8035_evt_t *evts,
                        size_t max_evts)
{
    if (len == 0) {
        return 0;
    }
    evt_sink_t sink = {.buf = evts, .n = 0, .max = evts ? max_evts : 0};
    const uint8_t old_system1 = emu->regs[QN8035_REG_SYSTEM1];
    const uint8_t old_vol = emu->regs[QN8035_REG_VOL_CTL];
    bool system1_written = false;
    bool vol_written = false;

    uint8_t reg = data[0] & REG_MASK;
    for (size_t i = 1; i < len; i++) {
        if (!is_read_only(reg)) {
            emu->regs[reg] = data[i];
        }
        system1_written |= reg == QN8035_REG_SYSTEM1;
        vol_written |= reg == QN8035_REG_VOL_CTL;
        reg = (reg + 1u) & REG_MASK;
    }
    emu->ptr = reg;

    /* Side effects after the whole transaction, so a burst that writes
     * SYSTEM1 together with the channel registers sees the new channel. */
    if (system1_written) {
        on_system1(emu, old_system1, &sink);
    }
    if (vol_written) {
        on_vol_ctl(emu, old_vol, &sink);
    }
    return sink.n;
}

void qn8035_emu_read(qn8035_emu_t *emu, uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        out[i] = emu->regs[emu->ptr];
        emu->ptr = (emu->ptr + 1u) & REG_MASK;
    }
}

bool qn8035_emu_complete_tune(qn8035_emu_t *emu, uint32_t khz, bool found)
{
    uint8_t *r = emu->regs;
    const uint16_t ch = qn8035_khz_to_ch(khz);
    const bool was_scanning = (r[QN8035_REG_SYSTEM1] & QN8035_SYSTEM1_CHSC) != 0;

    r[QN8035_REG_CH] = (uint8_t)(ch & 0xFF);
    r[QN8035_REG_CH_STEP] = (uint8_t)((r[QN8035_REG_CH_STEP] & ~0x3u) | ((ch >> 8) & 0x3u));
    r[QN8035_REG_SYSTEM1] &= (uint8_t)~QN8035_SYSTEM1_CHSC;
    emu->tuned_ch = ch;

    uint8_t st = r[QN8035_REG_STATUS1] | QN8035_STATUS1_RXAGCSET;
    st = found ? (uint8_t)(st & ~QN8035_STATUS1_RXCCA_FAIL) : (uint8_t)(st | QN8035_STATUS1_RXCCA_FAIL);
    r[QN8035_REG_STATUS1] = st;
    if (emu->receiving) {
        set_fsm(emu, QN8035_FSM_RECEIVING);
    }

    /* New channel: old RDS data is stale. */
    memset(&r[QN8035_REG_RDSD0], 0, QN8035_REG_RDSD7 - QN8035_REG_RDSD0 + 1);
    r[QN8035_REG_STATUS2] = 0;

    return was_scanning && (r[QN8035_REG_INT_CTRL] & QN8035_INT_CCA_INT_EN);
}

void qn8035_emu_set_signal(qn8035_emu_t *emu, uint8_t rssi, uint8_t snr, bool stereo)
{
    uint8_t *r = emu->regs;
    r[QN8035_REG_RSSISIG] = rssi;
    r[QN8035_REG_SNR] = snr;
    r[QN8035_REG_STATUS1] = stereo ? (uint8_t)(r[QN8035_REG_STATUS1] & ~QN8035_STATUS1_ST_MO_RX)
                                   : (uint8_t)(r[QN8035_REG_STATUS1] | QN8035_STATUS1_ST_MO_RX);
}

bool qn8035_emu_push_rds(qn8035_emu_t *emu, const uint16_t blocks[4], uint8_t err_mask,
                         bool version_b, bool sync)
{
    uint8_t *r = emu->regs;
    if (!(r[QN8035_REG_SYSTEM1] & QN8035_SYSTEM1_RDSEN)) {
        return false;
    }
    for (unsigned i = 0; i < 4; i++) {
        r[QN8035_REG_RDSD0 + 2 * i] = (uint8_t)(blocks[i] >> 8);
        r[QN8035_REG_RDSD0 + 2 * i + 1] = (uint8_t)(blocks[i] & 0xFF);
    }
    uint8_t st2 = (uint8_t)((r[QN8035_REG_STATUS2] ^ QN8035_STATUS2_RDS_RXUPD) & QN8035_STATUS2_RDS_RXUPD);
    st2 |= err_mask & 0x0Fu;
    if (version_b) {
        st2 |= QN8035_STATUS2_RDSC0C1;
    }
    if (sync) {
        st2 |= QN8035_STATUS2_RDSSYNC;
    }
    r[QN8035_REG_STATUS2] = st2;
    return (r[QN8035_REG_INT_CTRL] & QN8035_INT_RDS_INT_EN) != 0;
}

void qn8035_emu_set_rds_sync(qn8035_emu_t *emu, bool sync)
{
    uint8_t *st2 = &emu->regs[QN8035_REG_STATUS2];
    *st2 = sync ? (uint8_t)(*st2 | QN8035_STATUS2_RDSSYNC) : (uint8_t)(*st2 & ~QN8035_STATUS2_RDSSYNC);
}
