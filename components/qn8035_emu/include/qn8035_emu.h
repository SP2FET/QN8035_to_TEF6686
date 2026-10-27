/**
 * @file
 * QN8035 register-file model: what the head unit sees on the bus.
 *
 * Pure C, no ESP-IDF, no locking: the caller (qn8035_slave) serializes all
 * calls. Bus writes are turned into high-level events (tune, seek, mute...)
 * for the bridge; the bridge feeds tuner state back with the setters.
 *
 * A tune or scan is one operation: it starts with a TUNE/SEEK event, keeps
 * SYSTEM1.CHSC set (the busy flag drivers poll) and ends with
 * qn8035_emu_complete_tune() for that same operation. Completions of an
 * operation that a newer request replaced are ignored.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "qn8035_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CID1 as the head unit's chip reads it (docs/headunit-bus-capture.md): minor revision 3. The QF30
 * MCU starts tunes with CHSC when CID1[1:0] is 1 or 2 and with a pulse on register 0x55
 * otherwise, and mutes with 0x4A unless it is 1. */
#ifndef QN8035_EMU_CID1
#define QN8035_EMU_CID1 0x42
#endif
#define QN8035_EMU_CID2 0x84 /**< product id 100001b, major rev 1 */

#define QN8035_EMU_MAX_EVENTS 8

typedef enum {
    QN8035_EVT_RESET,      /**< SWRST: registers back to defaults */
    QN8035_EVT_POWER,      /**< .on: receive requested (RXREQ, not STNBY) */
    QN8035_EVT_TUNE,       /**< .khz */
    QN8035_EVT_SEEK,       /**< .seek: scan start..stop, first station wins */
    QN8035_EVT_MUTE,       /**< .on: VOL_CTL.MUTE_EN or the undocumented 0x4A/0x4C mutes */
    QN8035_EVT_VOLUME,     /**< .db: 0 .. -47 */
    QN8035_EVT_DEEMPHASIS, /**< .us: 50 or 75 */
    QN8035_EVT_FORCE_MONO, /**< .on */
    QN8035_EVT_RDS_ENABLE, /**< .on */
} qn8035_evt_type_t;

typedef struct {
    qn8035_evt_type_t type;
    uint32_t generation; /**< tune/scan identity, unaffected by unrelated register writes */
    union {
        bool on;
        uint32_t khz;
        int8_t db;
        uint8_t us;
        struct {
            uint32_t start_khz;
            uint32_t stop_khz;
            uint32_t step_khz;
        } seek;
    };
} qn8035_evt_t;

typedef enum {
    QN8035_OP_NONE,
    QN8035_OP_TUNE,
    QN8035_OP_SCAN,
} qn8035_op_t;

typedef struct {
    uint8_t regs[QN8035_REG_COUNT];
    uint32_t revision; /**< increments on data writes, survives software reset */
    uint32_t op_generation;
    uint8_t ptr;       /**< register address for the next read */
    bool receiving;    /**< RXREQ set and not in standby */
    bool muted;        /**< last reported mute state */
    bool volume_set;   /**< the head unit changed the gains since reset: the tuner follows */
    bool rds_upd;      /**< STATUS2.RDS_RXUPD of the last group published */
    bool rds_resync;   /**< tuned since the last group: the next one only sets RDSSYNC */
    uint16_t tuned_ch; /**< CH of the last completed tune (register value) */
    uint8_t op;        /**< qn8035_op_t in progress */
    uint16_t op_ch;    /**< CH register of the tune in progress */
    uint32_t op_khz;   /**< frequency asked for (TUNE event) */
    uint16_t pll_ref;  /**< PLL_DLT the CH raster is nominal at (set while not receiving) */
    /** The driver starts tunes with CHSC or 0x55, so a SYSTEM1 rewrite without CHSC is not a
     * tune: the QF30 MCU rewrites SYSTEM1 halfway through setting a channel. Survives SWRST
     * (it describes the driver, not the chip). */
    bool explicit_start;
} qn8035_emu_t;

/** Power-on state. */
void qn8035_emu_init(qn8035_emu_t *emu);
/** Current controls (the volume once the head unit set it) plus a pending operation, for
 * bounded queue-overrun reconciliation. */
size_t qn8035_emu_controls(const qn8035_emu_t *emu, qn8035_evt_t *out, size_t max);

/**
 * One bus write transaction: data[0] is the register address, the rest are
 * values written with auto-increment. A write of only the address sets the
 * read pointer. Returns the number of events stored in evts.
 */
size_t qn8035_emu_write(qn8035_emu_t *emu, const uint8_t *data, size_t len, qn8035_evt_t *evts,
                        size_t max_evts);

/** One bus read: len bytes from the read pointer, auto-incrementing. */
void qn8035_emu_read(qn8035_emu_t *emu, uint8_t *out, size_t len);

/** The next len bytes a read would return, without moving the pointer (to fill a TX FIFO). */
void qn8035_emu_peek_block(const qn8035_emu_t *emu, uint8_t *out, size_t len);

/** Move the read pointer as if len bytes had been read. */
void qn8035_emu_skip(qn8035_emu_t *emu, size_t len);

static inline uint8_t qn8035_emu_peek(const qn8035_emu_t *emu, uint8_t reg)
{
    return emu->regs[reg % QN8035_REG_COUNT];
}

/**
 * The tuner finished an operation at khz. by_scan says whether it was a scan.
 * Ignored unless it matches the operation in progress (kind, and khz for a
 * tune). Clears CHSC and sets STATUS1.RXCCA_FAIL: for a scan from found, for
 * a tune from found and the CCA level rule (RSSISIG against CCA.RXCCAD), so
 * call qn8035_emu_set_signal() with the new channel's values first. A scan
 * also moves CH to khz. Returns true if the INT pin should pulse (CCA_INT_EN).
 */
bool qn8035_emu_complete_tune(qn8035_emu_t *emu, uint32_t khz, bool found, bool by_scan);

/** Raw register values for RSSISIG / SNR and the stereo flag. */
void qn8035_emu_set_signal(qn8035_emu_t *emu, uint8_t rssi, uint8_t snr, bool stereo);

/** New RDS group. err_mask bit 3 = block A ... bit 0 = block D (STATUS2
 * layout). Ignored unless RDSEN is set. The first group after a tune only sets
 * STATUS2.RDSSYNC: the real chip shows sync about one group before its first
 * group. Returns true if INT should pulse. */
bool qn8035_emu_push_rds(qn8035_emu_t *emu, const uint16_t blocks[4], uint8_t err_mask,
                         bool version_b, bool sync);

void qn8035_emu_set_rds_sync(qn8035_emu_t *emu, bool sync);

#ifdef __cplusplus
}
#endif
