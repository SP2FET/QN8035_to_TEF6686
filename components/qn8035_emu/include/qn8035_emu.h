/*
 * QN8035 register-file model: what the head unit sees on the bus.
 *
 * Pure C, no ESP-IDF, no locking: the caller (qn8035_slave) serializes all
 * calls. Bus writes are turned into high-level events (tune, seek, mute...)
 * for the bridge; the bridge feeds tuner state back with the setters.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "qn8035_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CID1 of a real part is not documented anywhere we have. Capture it from the
 * head unit's chip (read register 0x05) and put it here. */
#ifndef QN8035_EMU_CID1
#define QN8035_EMU_CID1 0x00
#endif
#define QN8035_EMU_CID2 0x84 /* product id 100001b, major rev 1 */

#define QN8035_EMU_MAX_EVENTS 8

typedef enum {
    QN8035_EVT_RESET,      /* SWRST: registers back to defaults */
    QN8035_EVT_POWER,      /* .on: receive requested (RXREQ, not STNBY) */
    QN8035_EVT_TUNE,       /* .khz */
    QN8035_EVT_SEEK,       /* .seek: scan start..stop, first station wins */
    QN8035_EVT_MUTE,       /* .on */
    QN8035_EVT_VOLUME,     /* .db: 0 .. -47 */
    QN8035_EVT_DEEMPHASIS, /* .us: 50 or 75 */
    QN8035_EVT_FORCE_MONO, /* .on */
    QN8035_EVT_RDS_ENABLE, /* .on */
} qn8035_evt_type_t;

typedef struct {
    qn8035_evt_type_t type;
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

typedef struct {
    uint8_t regs[QN8035_REG_COUNT];
    uint8_t ptr;       /* register address for the next read */
    bool receiving;    /* RXREQ set and not in standby */
    uint16_t tuned_ch; /* channel last reported by the tuner */
} qn8035_emu_t;

/* Power-on state. */
void qn8035_emu_init(qn8035_emu_t *emu);

/*
 * One bus write transaction: data[0] is the register address, the rest are
 * values written with auto-increment. A write of only the address sets the
 * read pointer. Returns the number of events stored in evts.
 */
size_t qn8035_emu_write(qn8035_emu_t *emu, const uint8_t *data, size_t len, qn8035_evt_t *evts,
                        size_t max_evts);

/* One bus read: len bytes from the read pointer, auto-incrementing. */
void qn8035_emu_read(qn8035_emu_t *emu, uint8_t *out, size_t len);

static inline uint8_t qn8035_emu_peek(const qn8035_emu_t *emu, uint8_t reg)
{
    return emu->regs[reg % QN8035_REG_COUNT];
}

/* Tuner finished a tune or scan at khz. Clears CHSC, sets RXCCA_FAIL to
 * !found. Returns true if the INT pin should pulse (CCA_INT_EN). */
bool qn8035_emu_complete_tune(qn8035_emu_t *emu, uint32_t khz, bool found);

/* Raw register values for RSSISIG / SNR and the stereo flag. */
void qn8035_emu_set_signal(qn8035_emu_t *emu, uint8_t rssi, uint8_t snr, bool stereo);

/* New RDS group. err_mask bit 3 = block A ... bit 0 = block D (STATUS2
 * layout). Ignored unless RDSEN is set. Returns true if INT should pulse. */
bool qn8035_emu_push_rds(qn8035_emu_t *emu, const uint16_t blocks[4], uint8_t err_mask,
                         bool version_b, bool sync);

void qn8035_emu_set_rds_sync(qn8035_emu_t *emu, bool sync);

#ifdef __cplusplus
}
#endif
