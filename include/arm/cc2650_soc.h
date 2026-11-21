/*
 * TI CC2650 (CC26x0 family) SoC memory model (SmartRF06EB).
 *
 * Cortex-M3 at 48 MHz, 128 KB flash at 0x0, 20 KB SRAM at 0x20000000,
 * GPRAM (flash cache / VIMS) 8 KB at 0x11000000, and RF core RAM 4 KB at
 * 0x21000000. csim does not carry TI's ROM: the stand-in mapped there is
 * driverlib built from the SimpleLink SDK sources behind the ROM API tables
 * (tools/cc26x0-rom/), plus the HAPI table.
 *
 * Plugs into `arm_platform_t` through `cc2650_soc_ops`.
 */
#ifndef CC2650_SOC_H
#define CC2650_SOC_H

#include "arm_platform.h"

/* --- Memory map (TRM SWCU117I, "Memory Map"; driverlib inc/hw_memmap.h) --- */
#define CC2650_ROM_BASE         0x10000000u
#define CC2650_ROM_SIZE         0x1cc00u      /* real ROM; the stand-in image is smaller */
#define CC2650_HAPI_PORT        0x10040000u   /* csim-only, see tools/ti-simplelink-rom/hapi.c */
#define CC2650_GPRAM_BASE       0x11000000u
#define CC2650_GPRAM_SIZE       0x2000u
#define CC2650_RFC_RAM_BASE     0x21000000u
#define CC2650_RFC_RAM_SIZE     0x1000u
#define CC2650_FCFG1_BASE       0x50001000u
#define CC2650_FCFG1_SIZE       0x400u
#define CC2650_CCFG_BASE        0x50003000u
#define CC2650_CCFG_SIZE        0x1000u   /* mirrors the last flash sector */
#define CC2650_PERIPH_BASE      0x40000000u
#define CC2650_PERIPH_SIZE      0x10000000u

/* --- External interrupt numbers (vector - 16; inc/hw_ints.h) --- */
#define CC2650_IRQ_AON_RTC_COMB   4   /* INT_AON_RTC_COMB = 20 */
#define CC2650_IRQ_UART0          5   /* INT_UART0_COMB = 21 */

#define CC2650_UART_WIRE  1024           /* console input in flight, bytes */

struct cc2650_soc;

/* One peripheral window with a plain register store behind it; `kind` picks
 * the behaviour layered on top (CC2650_P_* in the .c). */
typedef struct cc2650_page {
    struct cc2650_soc *soc;
    uint32_t    base;          /* canonical 0x4xxx_xxxx / 0x5xxx_xxxx base */
    uint32_t    size;
    const char *name;
    int         kind;
    uint32_t   *regs;          /* size / 4 words */
} cc2650_page_t;

/* AON RTC.  The 64-bit counter SEC:SUBSEC advances 2^17 (1/32768 s) per
 * SCLK_LF edge; it is computed from the live clock, not stored.  On CC26x0
 * this is the system tick: TimerP/ClockP run on it, not GPT0. */
typedef struct cc2650_rtc {
    bool        enabled;
    int64_t     zero_ns;       /* live ns at which the counter read 0 */
    uint32_t    ctl;
    uint32_t    evflags;
    uint32_t    chctl;
    uint32_t    cmp[3];
    uint32_t    ch2cmpinc;
    uint32_t    ch1capt;
    uint32_t    subsec_latch;  /* SUBSEC as of the last SEC read */
    cpu_event_t ch_event[3];   /* per-channel compare crossing */
} cc2650_rtc_t;

/* Oscillator state behind DDI0 OSC: which source drives SCLK_HF / LF. */
typedef struct cc2650_osc {
    int         hf_src;        /* 0 = RCOSC_HF, 1 = XOSC_HF */
    int         lf_src;        /* 0 RCOSC_HF/1536, 1 XOSC_HF/1536, 2 RCOSC_LF, 3 XOSC_LF */
    bool        xosc_hf_ready;
    cpu_event_t xosc_hf_event;
    bool        xosc_hf_starting;
} cc2650_osc_t;

/* UART0: TI's PL011-derived UART with a 32-byte TX FIFO drained at the
 * programmed baud rate.  Unlike the CC13x2, the CC26x0 UART has no
 * end-of-transmission (EOT) interrupt. */
typedef struct cc2650_uart {
    uint32_t    ibrd, fbrd, lcrh, ctl, ifls, imsc, ris, dmactl;
    int         tx_count;                  /* bytes in the TX FIFO + shifter */
    uint8_t     tx_fifo[32];
    int         tx_head;
    bool        tx_busy;                   /* a byte is in the shifter */
    cpu_event_t tx_event;                  /* shifter done */
    uint8_t     rx_fifo[32];
    int         rx_head, rx_count;
    uint8_t     wire[CC2650_UART_WIRE];    /* console input not yet arrived */
    int         wire_head, wire_count;
    cpu_event_t rx_event;                  /* next character arrives */
    arm_uart_tx_callback tx_cb;
    void       *tx_user;
} cc2650_uart_t;

typedef struct cc2650_soc {
    arm_platform_t *plat;

    /* Memories the CPU core does not map itself. */
    uint8_t         gpram[CC2650_GPRAM_SIZE];
    uint8_t         rfc_ram[CC2650_RFC_RAM_SIZE];
    uint32_t        fcfg1[CC2650_FCFG1_SIZE / 4];

    /* Register windows; see the page table in cc2650_soc.c. */
    cc2650_page_t  *pages;
    int             num_pages;

    /* Analog register file (DDI0 OSC: 32-bit registers). */
    uint32_t        ddi0[0x20];

    /* PRCM power-domain state (PDCTL0 / PDCTL1). */
    uint32_t        pd0, pd1;

    cc2650_rtc_t    rtc;
    cc2650_osc_t    osc;
    cc2650_uart_t   uart0;

    /* Per-node identity the harness seeds before reset (FCFG1 MAC). */
    uint32_t        mac_15_4[2];
} cc2650_soc_t;

extern const arm_soc_ops_t cc2650_soc_ops;

static inline cc2650_soc_t *arm_platform_cc2650(arm_platform_t *plat) {
    return (plat && plat->config && plat->config->soc_ops == &cc2650_soc_ops)
           ? (cc2650_soc_t *)plat->soc : NULL;
}

/* Seed the IEEE 802.15.4 address the factory config reports (FCFG1
 * MAC_15_4_0/1). Contiki builds its EUI-64 from it unless CCFG overrides. */
void cc2650_soc_set_ieee_addr(cc2650_soc_t *soc, const uint8_t addr[8]);

/* Enqueue console input on UART0's receive wire (baud-paced delivery). */
int cc2650_uart_feed_rx(cc2650_soc_t *soc, const uint8_t *buf, int len);

#endif /* CC2650_SOC_H */
