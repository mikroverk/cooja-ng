/*
 * CC13x2 / CC26x2 RF core — a command-level model of the radio CPU behind
 * the doorbell (TRM SWCU185 chapter "Radio"; driverlib rf_mailbox.h,
 * rf_common_cmd.h, rf_prop_cmd.h, rf_ieee_cmd.h, rf_data_entry.h).
 *
 * The host firmware (TI's RF driver under Contiki-NG) never touches radio
 * registers: it hands command structures in RAM to the RF core through
 * RFC_DBELL:CMDR and waits for CMDSTA/RFACKIFG and the CPE interrupt flags.
 * This model executes those commands: it boots on PWMCLKEN (BOOT_DONE),
 * answers direct and immediate commands at once, runs radio operations
 * with their status words, pNextOp chaining and COMMAND_DONE /
 * LAST_COMMAND_DONE flags, keeps the radio timer (RAT, 4 MHz) locked to
 * the AON RTC, and moves frames between RX/TX buffers and the medium.
 *
 * Proprietary mode (sub-GHz 802.15.4g, CMD_PROP_*) and IEEE mode
 * (CMD_IEEE_*) are implemented.  On air a frame is the byte stream the
 * medium framers parse: preamble, sync word, PHR, payload, CRC.
 */
#ifndef CC13X2_RFC_H
#define CC13X2_RFC_H

#include <stdbool.h>
#include <stdint.h>
#include "cpu_event.h"
#include "sim_host.h"

struct cc13x2_soc;

typedef void (*cc13x2_rfc_tx_fn)(void *user, uint8_t byte);

typedef struct cc13x2_rfc_op {
    uint32_t    addr;          /* radio operation in RAM; 0 = slot idle */
    uint16_t    cmd;
    cpu_event_t ev;            /* start / completion of this operation */
    int         phase;         /* per-command step the event advances */
    bool        stop_req;      /* CMD_STOP received while busy */
} cc13x2_rfc_op_t;

typedef struct cc13x2_rfc {
    struct cc13x2_soc *soc;

    /* RFC_PWR / RFC_DBELL registers (inc/hw_rfc_pwr.h, hw_rfc_dbell.h). */
    uint32_t    pwmclken;
    uint32_t    cmdsta;
    uint32_t    hwifg, hwien;
    uint32_t    cpeifg, cpeien, cpeisl;
    uint32_t    ackifg;
    uint32_t    sysgpoctl;
    bool        booted;
    cpu_event_t boot_ev;

    /* Radio timer: RATCNT = rat0 + 4 MHz x (time on the AON RTC). */
    bool        rat_running;
    uint32_t    rat0;

    /* Radio configuration from the setup and FS commands. */
    bool        setup_done;
    bool        prop;              /* proprietary (sub-GHz) vs IEEE */
    int         pream_bytes;       /* CMD_PROP_RADIO_DIV_SETUP preamConf */
    int         sw_bits;           /* formatConf.nSwBits */
    int         whiten_mode;       /* formatConf.whitenMode */
    uint32_t    freq_khz;          /* CMD_FS */
    bool        synth_ok;          /* synthesizer programmed (FS / IEEE RX channel) */
    int         channel;           /* medium channel last pushed, -1 none */
    int         radio_idx;         /* medium slot: 1 sub-GHz, 0 2.4 GHz */

    /* Operation slots: background (prop, IEEE RX) and foreground (IEEE). */
    cc13x2_rfc_op_t bg, fg;
    cpu_event_t     chain_ev;      /* deferred pNextOp start (op_finish) */
    uint32_t        chain_next;
    int             chain_depth;   /* synchronous chaining in progress */

    /* Receiver (prop RX_ADV). */
    struct {
        bool     active;           /* an RX operation is listening */
        uint32_t op;               /* that operation */
        int      state;            /* RX_HUNT / RX_HDR / RX_PAYLOAD / RX_CRC */
        uint32_t shift;            /* sliding sync-word register */
        int      hdr_count;
        uint16_t hdr;
        int      n;                /* payload bytes expected */
        int      crc_len;
        int      got;
        uint8_t  buf[256];         /* payload */
        uint8_t  crc[4];
        int8_t   rssi;
        int64_t  sync_end_ns;      /* end of the sync word on air */
        int64_t  last_byte_ns;
    } rx;

    /* IEEE mode: the receiver configuration latched when CMD_IEEE_RX
     * starts (and changed by CMD_IEEE_MOD_FILT / MOD_CCA), the foreground
     * operation suspending it, and the auto-ACK transmitter. */
    struct {
        uint16_t    filt_opt;          /* frameFiltOpt */
        uint8_t     frame_types;
        uint8_t     cca_opt;
        int8_t      cca_thr;
        uint64_t    ext_addr;
        uint16_t    short_addr;
        uint16_t    pan_id;
        bool        bg_suspended;      /* a foreground op owns the radio */
        bool        ack_listen;        /* CMD_IEEE_RX_ACK waiting for its ACK */
        uint8_t     ack_seq;
        uint8_t     ack_frame[5];      /* auto-ACK MPDU being sent */
        cpu_event_t ack_ev;
        int64_t     mute_until_ns;     /* own ACK on air: deaf until then */
        uint16_t    csma_lfsr;         /* CMD_IEEE_CSMA randomState 0: own stream */
    } ieee;

    /* Host side: the medium. */
    cc13x2_rfc_tx_fn   tx_cb;
    void              *tx_user[2];       /* per medium slot */
    sim_channel_energy_fn energy_cb;   /* the channel's energy (RSSI, CCA) */
    void                 *energy_user;

    int         node_id;           /* for CC13X2_RFC_TRACE lines */

    /* Counters for the end-of-run report and tests. */
    uint64_t    stat_tx, stat_rx_ok, stat_rx_crc_err, stat_rx_buf_full, stat_cmds;
} cc13x2_rfc_t;

void cc13x2_rfc_init(cc13x2_rfc_t *rfc, struct cc13x2_soc *soc);
void cc13x2_rfc_reset(cc13x2_rfc_t *rfc);

/* Register windows (offsets within RFC_PWR / RFC_DBELL / RFC_RAT). */
uint32_t cc13x2_rfc_pwr_read(cc13x2_rfc_t *rfc, uint32_t off);
void     cc13x2_rfc_pwr_write(cc13x2_rfc_t *rfc, uint32_t off, uint32_t val);
uint32_t cc13x2_rfc_dbell_read(cc13x2_rfc_t *rfc, uint32_t off);
void     cc13x2_rfc_dbell_write(cc13x2_rfc_t *rfc, uint32_t off, uint32_t val);
uint32_t cc13x2_rfc_rat_read(cc13x2_rfc_t *rfc, uint32_t off);

/* The RF core power domain changed (PRCM PDCTL0/1 RFC). */
void cc13x2_rfc_domain_changed(cc13x2_rfc_t *rfc, bool on);

/* One on-air byte from the medium, and the end of a stream that died. */
void cc13x2_rfc_receive_byte(cc13x2_rfc_t *rfc, uint8_t byte, int8_t rssi);
void cc13x2_rfc_rx_stall(cc13x2_rfc_t *rfc);

void cc13x2_rfc_set_tx_listener(cc13x2_rfc_t *rfc, cc13x2_rfc_tx_fn cb,
                                void *user_subghz, void *user_24ghz);
void cc13x2_rfc_set_channel_energy_query(cc13x2_rfc_t *rfc,
                                         sim_channel_energy_fn cb, void *user);

#endif /* CC13X2_RFC_H */
