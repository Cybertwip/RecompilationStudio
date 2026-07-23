/*
 * starvation_ring.h — Always-on diagnostic ring for capturing SIO/MMIO
 * state immediately before a TCP-starvation hang.
 *
 * Phase 1.0e-e2 debug aid. Scope: diagnostic-only. Records every SIO MMIO
 * event (TX_DATA write, RX read, STAT read, CTRL write) plus a snapshot
 * of all SIO bus state at that moment. Watchdog monitors host wall-clock
 * since last debug_server_poll; if it exceeds a threshold, the ring is
 * flushed to disk and the process aborts cleanly so the dump survives.
 *
 * Diagnostic builds enable it by default. Production PSX_NO_DEBUG_TOOLS
 * builds compile it out completely so slow/thermally-throttled hosts are never
 * terminated by a fixed wall-clock threshold and pay no ring-recording cost.
 * An explicit STARVATION_RING_ENABLED definition still overrides the default.
 */
#ifndef PSXRECOMP_STARVATION_RING_H
#define PSXRECOMP_STARVATION_RING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef STARVATION_RING_ENABLED
#  ifdef PSX_NO_DEBUG_TOOLS
#    define STARVATION_RING_ENABLED 0
#  else
#    define STARVATION_RING_ENABLED 1
#  endif
#endif

#define STARVATION_RING_CAP (1 << 14)  /* 16K entries */

typedef enum {
    SR_EVT_NONE          = 0,
    SR_EVT_TX_DATA_WRITE = 1,
    SR_EVT_RX_DATA_READ  = 2,
    SR_EVT_STAT_READ     = 3,
    SR_EVT_CTRL_WRITE    = 4,
    SR_EVT_MODE_WRITE    = 5,
    SR_EVT_BAUD_WRITE    = 6,
    SR_EVT_SHIFT_START   = 7,
    SR_EVT_BUFFER_LOAD   = 8,
    SR_EVT_TX_DROPPED    = 9,
    SR_EVT_SHIFT_DONE    = 10,
    SR_EVT_ACK_FIRE      = 11,
    SR_EVT_SELECT_ASSERT = 12,
    SR_EVT_SELECT_DEASS  = 13,
    SR_EVT_RESET         = 14,
    SR_EVT_PC_SAMPLE     = 15,  /* periodic PC sample, no MMIO event */
} StarvationEventKind;

typedef struct {
    uint64_t seq;                /* monotonic sequence */
    uint64_t psx_cycle_count;    /* guest cycle clock at event */
    uint64_t host_us;            /* host monotonic time in microseconds */
    uint32_t current_func;       /* g_debug_current_func_addr */
    uint32_t last_store_pc;      /* g_debug_last_store_pc */
    uint16_t sio_ctrl;
    uint16_t sio_stat;
    uint8_t  tx_data;            /* on TX_DATA writes; else 0 */
    uint8_t  rx_data;            /* on RX_DATA reads; else 0 */
    uint8_t  kind;               /* StarvationEventKind */
    uint8_t  in_exception;
    uint8_t  shift_active;
    uint8_t  tx_buffered;
    uint8_t  pending_ack;
    uint8_t  bus_owner;          /* SioBusOwner */
    uint8_t  active_device;      /* DEV_NONE/PAD/MEMCARD */
    uint8_t  mc_state;
    uint8_t  pad_state;
    uint8_t  selected_slot;
    uint8_t  g_sio_timing_active;
    uint8_t  tx_rdy_visible;     /* (sio_stat & 1) at event time */
    uint8_t  tx_em_visible;      /* (sio_stat & 4) at event time */
    uint8_t  pad8;
    uint16_t pad16;
    int      shift_remaining;    /* cycles left on shifter */
    int      ack_remaining;      /* cycles left on ack */
    uint32_t bus_byte_index;
    uint32_t i_stat;
    uint32_t i_mask;
} StarvationEntry;

void starvation_ring_record(uint8_t kind, uint8_t tx, uint8_t rx,
                            uint16_t ctrl, uint16_t stat,
                            int shift_active, int shift_remaining,
                            int tx_buffered, int pending_ack,
                            int ack_remaining,
                            uint8_t bus_owner, uint32_t bus_byte_index,
                            uint8_t active_device, uint8_t mc_state,
                            uint8_t pad_state, uint8_t selected_slot,
                            int g_sio_timing_active);

/* Diagnostic-only watchdog heartbeat. In PSX_NO_DEBUG_TOOLS builds every
 * function in this interface is compiled as a no-op. */
void starvation_watchdog_heartbeat(void);

/* Host-side intentional stalls (in-game pause/settings menu) must pause
 * the watchdog: wall-clock advances while the guest and debug_server_poll
 * do not, so the 4 s threshold would fire on the first post-resume check. */
void starvation_watchdog_pause(void);
void starvation_watchdog_resume(void);

/* Run watchdog check from a hot path (e.g. psx_advance_cycles). */
void starvation_watchdog_check(void);

/* Manual dump-to-file (for testing / TCP probe). Filename can be NULL. */
void starvation_ring_dump(const char *path);

/* Periodic PC sample. Caller responsible for throttling. */
void starvation_ring_pc_sample(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_STARVATION_RING_H */
