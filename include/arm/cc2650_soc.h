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

typedef struct cc2650_soc {
    arm_platform_t *plat;

    /* Memories the CPU core does not map itself. */
    uint8_t         gpram[CC2650_GPRAM_SIZE];
    uint8_t         rfc_ram[CC2650_RFC_RAM_SIZE];
    uint32_t        fcfg1[CC2650_FCFG1_SIZE / 4];

    /* Register windows; see the page table in cc2650_soc.c. */
    cc2650_page_t  *pages;
    int             num_pages;

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

#endif /* CC2650_SOC_H */
