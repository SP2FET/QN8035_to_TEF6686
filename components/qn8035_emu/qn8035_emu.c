#include "qn8035_emu.h"

#include <string.h>

#define REG_MASK  (QN8035_REG_COUNT - 1u)
#define RDS_BYTES (QN8035_REG_RDSD7 - QN8035_REG_RDSD0 + 1)

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

static uint16_t pll_dlt(const uint8_t *r)
{
    return (uint16_t)(((uint16_t)r[QN8035_REG_XTAL_DIV2] << 5) | (r[QN8035_REG_XTAL_DIV1] >> 3));
}

static bool mute_state(const uint8_t *r)
{
    return (r[QN8035_REG_VOL_CTL] & QN8035_VOL_MUTE_EN) || (r[QN8035_REG_MUTE] & QN8035_MUTE_BIT) ||
           (r[QN8035_REG_DAC] & QN8035_DAC_MUTE_BITS) == QN8035_DAC_MUTE_BITS;
}

static void load_defaults(qn8035_emu_t *emu)
{
    /* Undocumented registers the QF30 MCU read-modify-writes, as the captured chip reads them at
     * power-on and after SWRST (docs/headunit-bus-capture.md). */
    static const uint8_t k_undocumented[][2] = {
        {0x1B, 0x70}, {0x1D, 0xE9}, {0x27, 0x65}, {0x2C, 0x6D}, {0x34, 0x30}, {0x35, 0x54},
        {0x36, 0x35}, {0x38, 0x02}, {0x39, 0x20}, {0x3A, 0x45}, {0x3E, 0xB5}, {0x40, 0x0A},
        {0x41, 0xE8}, {0x47, 0x35}, {0x49, 0x75}, {QN8035_REG_MUTE, 0x10}, {QN8035_REG_DAC, 0x10},
        {QN8035_REG_TUNE_GO, 0x33}, {QN8035_REG_CHIP_ID, 0x33},
    };
    const bool explicit_start = emu->explicit_start;
    memset(emu, 0, sizeof(*emu));
    emu->explicit_start = explicit_start;
    uint8_t *r = emu->regs;
    /* Reset values from the Rev 0.08 register tables. CH/CH_START/CH_STOP
     * decode to 88.0 / 87.0 / 108.0 MHz, 100 kHz scan step. The captured chip comes up in
     * standby (Rev 0.08 says SYSTEM1 = 0x01) with CCA.XTAL_INJ set. */
    r[QN8035_REG_SYSTEM1] = QN8035_SYSTEM1_STNBY | QN8035_SYSTEM1_CCA_CH_DIS;
    r[QN8035_REG_CCA] = QN8035_CCA_XTAL_INJ;
    r[QN8035_REG_STATUS1] = QN8035_STATUS1_CAP_SH;
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
    for (size_t i = 0; i < sizeof(k_undocumented) / sizeof(k_undocumented[0]); i++) {
        r[k_undocumented[i][0]] = k_undocumented[i][1];
    }
    set_fsm(emu, QN8035_FSM_STBY);
    emu->tuned_ch = 0xFFFF; /* nothing tuned yet */
    emu->pll_ref = pll_dlt(r);
    emu->muted = mute_state(r);
}

void qn8035_emu_init(qn8035_emu_t *emu)
{
    emu->explicit_start = false;
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

/* The frequency CH stands for. Drivers move the PLL step off its nominal value to push a spur
 * off a channel, and the CH raster moves with it: the QF30 MCU asks for 85.7 MHz with
 * XTAL_DIV1/2 = 0x88/0x3B to receive 85.5 MHz. Rounded to the TEF's 10 kHz. */
static uint32_t effective_khz(const qn8035_emu_t *emu, uint16_t ch)
{
    const uint32_t khz = qn8035_ch_to_khz(ch);
    const uint64_t now = QN8035_PLL_DLT_BASE + pll_dlt(emu->regs);
    const uint64_t ref = QN8035_PLL_DLT_BASE + emu->pll_ref;
    if (now == ref) {
        return khz;
    }
    return (uint32_t)((khz * now + ref * 5u) / (ref * 10u)) * 10u;
}

/* The captured chip's QN8035_REG_CAL read-back falls by one at each of these frequencies, from
 * 156 at 87.5 MHz to 134 at 107.5 MHz (412 channels, docs/headunit-bus-capture.md). */
static const uint32_t k_cal_step_khz[] = {
    87800, 88500, 89200, 89950, 90700, 91500, 92300, 93100, 93950, 94800, 95700,
    96600, 97550, 98500, 99500, 100550, 101600, 102700, 103850, 105000, 106250, 107500,
};

static uint8_t cal_readback(uint32_t khz)
{
    if (khz < 87500) {
        /* One point below the band: 191 at 65.0 MHz. */
        const uint32_t v = 156u + ((87500u - khz) * 35u + 11250u) / 22500u;
        return (uint8_t)(v > 255u ? 255u : v);
    }
    uint8_t v = 156;
    for (size_t i = 0; i < sizeof(k_cal_step_khz) / sizeof(k_cal_step_khz[0]) &&
                       khz >= k_cal_step_khz[i]; i++) {
        v--;
    }
    return v;
}

static uint32_t scan_step_khz(uint8_t ch_step)
{
    switch ((ch_step >> QN8035_CH_STEP_FSTEP_SHIFT) & 0x3u) {
    case 0:  return 50;
    case 2:  return 200;
    default: return 100; /* 01, and reserved 11 */
    }
}

/* A new channel is being tuned: nothing measured on the old one is valid any more. */
static void start_op(qn8035_emu_t *emu, qn8035_op_t op, uint16_t ch, uint32_t khz)
{
    uint8_t *r = emu->regs;
    emu->op = (uint8_t)op;
    emu->op_ch = ch;
    emu->op_khz = khz;
    r[QN8035_REG_RSSISIG] = 0;
    r[QN8035_REG_SNR] = 0;
    r[QN8035_REG_STATUS1] |= QN8035_STATUS1_ST_MO_RX;
    memset(&r[QN8035_REG_RDSD0], 0, RDS_BYTES);
    /* Like the captured chip, STATUS2 reads 0 until RDS syncs on the new channel. A QF30 MCU that
     * saw RDS_RXUPD set before then reads the cleared bytes as a group. */
    r[QN8035_REG_STATUS2] = 0;
    emu->rds_upd = false;
    emu->rds_resync = true;
}

/* busy: set CHSC for a driver that started the tune another way but polls CHSC. */
static void request_tune(qn8035_emu_t *emu, evt_sink_t *sink, bool busy)
{
    const uint16_t ch = current_ch(emu);
    const uint32_t khz = effective_khz(emu, ch);
    start_op(emu, QN8035_OP_TUNE, ch, khz);
    if (busy) {
        emu->regs[QN8035_REG_SYSTEM1] |= QN8035_SYSTEM1_CHSC;
    }
    emit(sink, (qn8035_evt_t){.type = QN8035_EVT_TUNE, .khz = khz});
}

static void request_scan(qn8035_emu_t *emu, evt_sink_t *sink)
{
    const uint8_t *r = emu->regs;
    const uint8_t step = r[QN8035_REG_CH_STEP];
    const uint16_t start = (uint16_t)(r[QN8035_REG_CH_START] |
                                      (((step >> QN8035_CH_STEP_START_SHIFT) & 3u) << 8));
    const uint16_t stop = (uint16_t)(r[QN8035_REG_CH_STOP] |
                                     (((step >> QN8035_CH_STEP_STOP_SHIFT) & 3u) << 8));
    start_op(emu, QN8035_OP_SCAN, current_ch(emu), 0);
    set_fsm(emu, QN8035_FSM_RXCCA);
    emit(sink, (qn8035_evt_t){.type = QN8035_EVT_SEEK,
                              .seek = {.start_khz = qn8035_ch_to_khz(start),
                                       .stop_khz = qn8035_ch_to_khz(stop),
                                       .step_khz = scan_step_khz(step)}});
}

/* Returns true after a software reset (the rest of the transaction is moot). */
static bool on_system1(qn8035_emu_t *emu, uint8_t old, evt_sink_t *sink)
{
    uint8_t *r = emu->regs;
    const uint8_t v = r[QN8035_REG_SYSTEM1];

    if (v & QN8035_SYSTEM1_SWRST) {
        load_defaults(emu);
        emit(sink, (qn8035_evt_t){.type = QN8035_EVT_RESET});
        return true;
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
        emu->op = QN8035_OP_NONE;
        return false;
    }

    const uint16_t ch = current_ch(emu);
    if (v & QN8035_SYSTEM1_CHSC) {
        /* CHSC stays set until the bridge reports completion. Writing it back while busy (a
         * read-modify-write of SYSTEM1) does not restart the same request; a new channel does. */
        const bool was_busy = (old & QN8035_SYSTEM1_CHSC) != 0;
        emu->explicit_start = true;
        if (v & QN8035_SYSTEM1_CCA_CH_DIS) {
            if (!was_busy || emu->op != QN8035_OP_TUNE || ch != emu->op_ch ||
                effective_khz(emu, ch) != emu->op_khz) {
                request_tune(emu, sink, false);
            }
        } else if (!was_busy || emu->op != QN8035_OP_SCAN) {
            request_scan(emu, sink);
        }
    } else if (!emu->explicit_start && (v & QN8035_SYSTEM1_CCA_CH_DIS) &&
               (powered_up || ch != emu->tuned_ch) &&
               !(emu->op == QN8035_OP_TUNE && ch == emu->op_ch)) {
        /* Some drivers retune by rewriting SYSTEM1 without CHSC. */
        request_tune(emu, sink, false);
    }
    return false;
}

static void on_tune_go(qn8035_emu_t *emu, uint8_t old, evt_sink_t *sink)
{
    const uint8_t v = emu->regs[QN8035_REG_TUNE_GO];
    if (emu->receiving && (v & ~old & QN8035_TUNE_GO_BIT)) {
        emu->explicit_start = true;
        request_tune(emu, sink, true); /* CHSC set: the MCU polls it before judging the channel */
    }
}

static void on_vol_ctl(qn8035_emu_t *emu, uint8_t old, evt_sink_t *sink)
{
    const uint8_t v = emu->regs[QN8035_REG_VOL_CTL];
    const uint8_t changed = v ^ old;

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

static void update_mute(qn8035_emu_t *emu, evt_sink_t *sink)
{
    const bool m = mute_state(emu->regs);
    if (m != emu->muted) {
        emu->muted = m;
        emit(sink, (qn8035_evt_t){.type = QN8035_EVT_MUTE, .on = m});
    }
}

size_t qn8035_emu_write(qn8035_emu_t *emu, const uint8_t *data, size_t len, qn8035_evt_t *evts,
                        size_t max_evts)
{
    if (len == 0) {
        return 0;
    }
    evt_sink_t sink = {.buf = evts, .n = 0, .max = evts ? max_evts : 0};
    uint8_t *r = emu->regs;
    const uint8_t old_system1 = r[QN8035_REG_SYSTEM1];
    const uint8_t old_vol = r[QN8035_REG_VOL_CTL];
    const uint8_t old_go = r[QN8035_REG_TUNE_GO];
    bool system1_written = false, vol_written = false, mute_written = false;
    bool go_written = false, xtal_written = false, cal_written = false;

    uint8_t reg = data[0] & REG_MASK;
    for (size_t i = 1; i < len; i++) {
        if (!is_read_only(reg)) {
            r[reg] = data[i];
        }
        system1_written |= reg == QN8035_REG_SYSTEM1;
        vol_written |= reg == QN8035_REG_VOL_CTL;
        mute_written |= reg == QN8035_REG_VOL_CTL || reg == QN8035_REG_MUTE || reg == QN8035_REG_DAC;
        go_written |= reg == QN8035_REG_TUNE_GO;
        xtal_written |= reg >= QN8035_REG_XTAL_DIV0 && reg <= QN8035_REG_XTAL_DIV2;
        cal_written |= reg == QN8035_REG_CAL;
        reg = (reg + 1u) & REG_MASK;
    }
    emu->ptr = reg;

    /* The clock is configured before receiving: that PLL step is the nominal one. Changes
     * while receiving are spur workarounds that move the raster (effective_khz). */
    if (xtal_written && !emu->receiving) {
        emu->pll_ref = pll_dlt(r);
    }
    /* A calibration start: the QF30 MCU reads the result 0.3 ms later. */
    if (cal_written && (r[QN8035_REG_CAL] & QN8035_CAL_START)) {
        r[QN8035_REG_CAL] = cal_readback(effective_khz(emu, current_ch(emu)));
    }
    /* Side effects after the whole transaction, so a burst that writes
     * SYSTEM1 together with the channel registers sees the new channel. */
    if (system1_written && on_system1(emu, old_system1, &sink)) {
        return sink.n;
    }
    if (go_written) {
        on_tune_go(emu, old_go, &sink);
    }
    if (vol_written) {
        on_vol_ctl(emu, old_vol, &sink);
    }
    if (mute_written) {
        update_mute(emu, &sink);
    }
    return sink.n;
}

void qn8035_emu_read(qn8035_emu_t *emu, uint8_t *out, size_t len)
{
    qn8035_emu_peek_block(emu, out, len);
    qn8035_emu_skip(emu, len);
}

void qn8035_emu_peek_block(const qn8035_emu_t *emu, uint8_t *out, size_t len)
{
    uint8_t p = emu->ptr;
    for (size_t i = 0; i < len; i++) {
        out[i] = emu->regs[p];
        p = (p + 1u) & REG_MASK;
    }
}

void qn8035_emu_skip(qn8035_emu_t *emu, size_t len)
{
    emu->ptr = (uint8_t)((emu->ptr + len) & REG_MASK);
}

static bool cca_level_ok(const uint8_t *r)
{
    const int dbuv = (int)r[QN8035_REG_RSSISIG] - QN8035_RSSI_DBUV_OFFSET;
    return dbuv > (int)(r[QN8035_REG_CCA] & QN8035_CCA_RXCCAD_MASK) - 10;
}

bool qn8035_emu_complete_tune(qn8035_emu_t *emu, uint32_t khz, bool found, bool by_scan)
{
    uint8_t *r = emu->regs;
    if (emu->op == QN8035_OP_NONE || (emu->op == QN8035_OP_SCAN) != by_scan ||
        (!by_scan && khz != emu->op_khz)) {
        return false; /* replaced by a newer request, or not one of ours */
    }
    const bool was_busy = (r[QN8035_REG_SYSTEM1] & QN8035_SYSTEM1_CHSC) != 0;

    if (by_scan) {
        const uint16_t ch = qn8035_khz_to_ch(khz);
        r[QN8035_REG_CH] = (uint8_t)(ch & 0xFF);
        r[QN8035_REG_CH_STEP] = (uint8_t)((r[QN8035_REG_CH_STEP] & ~0x3u) | ((ch >> 8) & 0x3u));
        emu->tuned_ch = ch;
    } else {
        emu->tuned_ch = emu->op_ch; /* CH stays what the driver wrote */
        found = found && cca_level_ok(r);
    }
    emu->op = QN8035_OP_NONE;
    r[QN8035_REG_SYSTEM1] &= (uint8_t)~QN8035_SYSTEM1_CHSC;

    uint8_t st = r[QN8035_REG_STATUS1] | QN8035_STATUS1_RXAGCSET;
    st = found ? (uint8_t)(st & ~QN8035_STATUS1_RXCCA_FAIL) : (uint8_t)(st | QN8035_STATUS1_RXCCA_FAIL);
    r[QN8035_REG_STATUS1] = st;
    if (emu->receiving) {
        set_fsm(emu, QN8035_FSM_RECEIVING);
    }
    return was_busy && (r[QN8035_REG_INT_CTRL] & QN8035_INT_CCA_INT_EN);
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
    if (emu->rds_resync) {
        emu->rds_resync = false;
        r[QN8035_REG_STATUS2] = QN8035_STATUS2_RDSSYNC;
        return false;
    }
    for (unsigned i = 0; i < 4; i++) {
        r[QN8035_REG_RDSD0 + 2 * i] = (uint8_t)(blocks[i] >> 8);
        r[QN8035_REG_RDSD0 + 2 * i + 1] = (uint8_t)(blocks[i] & 0xFF);
    }
    emu->rds_upd = !emu->rds_upd;
    uint8_t st2 = emu->rds_upd ? QN8035_STATUS2_RDS_RXUPD : 0;
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
