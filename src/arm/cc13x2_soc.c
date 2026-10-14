/*
 * TI CC13x2 / CC26x2 SoC peripheral bundle — see include/arm/cc13x2_soc.h.
 *
 * Register offsets and bit positions are those of the SimpleLink core SDK
 * device headers (source/ti/devices/cc13x2_cc26x2/inc/hw_*.h), which follow
 * the CC13x2/CC26x2 Technical Reference Manual (SWCU185).  Each behaviour
 * below says where it comes from; where the documentation leaves it open
 * the comment says so instead of guessing silently.
 */
#include "cc13x2_soc.h"
#include "arm_nvic.h"
#include "cpu_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const size_t  cc13x2_rom_image_size;
extern const uint8_t cc13x2_rom_image[];
extern const size_t  cc13x2x7_rom_image_size;
extern const uint8_t cc13x2x7_rom_image[];

/* CC13X2_TRACE=1: log every access to a window modelled only as a plain
 * register store, with the PC — the first thing to read when firmware
 * stalls on a status bit this file does not yet produce.  CC13X2_TRACE=2
 * logs the modelled windows too (not the memories). */
int cc13x2_env_level(int *cache, const char *name) {
    if (__builtin_expect(*cache < 0, 0)) {
        const char *e = getenv(name);
        *cache = e ? (*e ? atoi(e) : 1) : 0;
    }
    return *cache;
}

static int cc13x2_trace = -1;
static bool trace_on(void) {
    return cc13x2_env_level(&cc13x2_trace, "CC13X2_TRACE") != 0;
}

static inline int64_t live_ns(cc13x2_soc_t *soc) {
    arm_cpu_t *cpu = &soc->plat->cpu;
    return arm_cycles_to_ns(cpu->cycles, cpu->cpu_freq_hz);
}

/* Schedule at an absolute time in the cycle clock's ns base (live_ns),
 * rounded up to the first cycle at or after it.  arm_schedule_event_ns
 * converts against cpu->sim_time_ns, which trails the cycle clock by the
 * boot pre-run and the slice in progress, so a deadline computed from
 * live_ns lands late by that gap — first seen here as every RTC wakeup
 * firing 158 ms late. Same reasoning as nrf54l_schedule_at_cycle_ns. */
static void schedule_at(cc13x2_soc_t *soc, cpu_event_t *ev, int64_t fire_ns) {
    arm_cpu_t *cpu = &soc->plat->cpu;
    int64_t now = live_ns(soc);
    int64_t delta = fire_ns > now ? fire_ns - now : 0;
    arm_schedule_event(cpu, ev, cpu->cycles + cpu_ns_to_cycles_ceil(delta, cpu->cpu_freq_hz));
}

static inline uint32_t width_of(cc13x2_soc_t *soc) {
    uint8_t w = soc->plat->cpu.io_width;
    return w ? w : 4;
}

/* Merge an access of the current width into a 32-bit register image. */
static uint32_t merge_write(cc13x2_soc_t *soc, uint32_t old, uint32_t addr,
                            uint32_t val) {
    uint32_t w = width_of(soc);
    if (w >= 4) return val;
    uint32_t sh = (addr & 3u) * 8u;
    uint32_t mask = (w == 2 ? 0xFFFFu : 0xFFu) << sh;
    return (old & ~mask) | ((val << sh) & mask);
}

static void irq_raise(cc13x2_soc_t *soc, int irq) {
    arm_nvic_set_pending(&soc->plat->nvic, irq);
}

/* ============================================================
 * Page table
 * ============================================================ */

enum {
    P_STORE = 0,     /* plain read-back register store */
    P_PRCM,
    P_AON_PMCTL,
    P_AON_RTC,
    P_AON_IOC,
    P_VIMS,
    P_FLASH,
    P_GPIO,
    P_UART0,
    P_DDI0,
    P_ADI2,
    P_ADI3,
    P_ADI4,
    P_AUX_SYSIF,
    P_SMPH,          /* MCU semaphores */
    P_AUX_SMPH,
    P_FCFG1,
    P_CCFG,
    P_ROM,
    P_HAPI,
    P_GPRAM,
    P_RFC_RAM,
    P_WDT,
    P_RFC_PWR,
    P_RFC_DBELL,
    P_TRNG,
    P_RFC_RAT,
    P_AUX_TDC,
    P_AUX_EVCTL,
};

typedef struct {
    uint32_t    base, size;
    const char *name;
    int         kind;
    bool        nonbuf;      /* also reachable through the 0x6xxx_xxxx alias */
} page_desc_t;

/* inc/hw_memmap.h. "nonbuf" marks the windows driverlib reaches through
 * the non-buffered alias (0x6000_0000 + offset; e.g. GPIO's DOUTSET via
 * GPIO_NONBUF_BASE). */
static const page_desc_t page_descs[] = {
    { CC13X2_ROM_BASE,     0x4000,  "ROM",       P_ROM,      false },
    { CC13X2_HAPI_PORT,    0x1000,  "HAPI",      P_HAPI,     false },
    { CC13X2_GPRAM_BASE,   CC13X2_GPRAM_SIZE, "GPRAM", P_GPRAM, false },
    { CC13X2_RFC_RAM_BASE, 0x8000,  "RFC_RAM",   P_RFC_RAM,  false },
    { 0x40000000, 0x1000, "SSI0",       P_STORE,     false },
    { 0x40001000, 0x1000, "UART0",      P_UART0,     false },
    { 0x40002000, 0x1000, "I2C0",       P_STORE,     false },
    { 0x40008000, 0x1000, "SSI1",       P_STORE,     false },
    { 0x4000B000, 0x1000, "UART1",      P_STORE,     false },
    { 0x40010000, 0x1000, "GPT0",       P_STORE,     false },
    { 0x40011000, 0x1000, "GPT1",       P_STORE,     false },
    { 0x40012000, 0x1000, "GPT2",       P_STORE,     false },
    { 0x40013000, 0x1000, "GPT3",       P_STORE,     false },
    { 0x40020000, 0x1000, "UDMA0",      P_STORE,     false },
    { 0x40022000, 0x1000, "GPIO",       P_GPIO,      true  },
    { 0x40024000, 0x1000, "CRYPTO",     P_STORE,     false },
    { 0x40028000, 0x2000, "TRNG",       P_TRNG,      false },
    { 0x40030000, 0x4000, "FLASH",      P_FLASH,     false },
    { 0x40034000, 0x1000, "VIMS",       P_VIMS,      false },
    { 0x40035000, 0x1000, "SRAM_MMR",   P_STORE,     false },
    { 0x40040000, 0x1000, "RFC_PWR",    P_RFC_PWR,   true  },
    { 0x40041000, 0x1000, "RFC_DBELL",  P_RFC_DBELL, true  },
    { 0x40043000, 0x1000, "RFC_RAT",    P_RFC_RAT,   true  },
    { 0x40044000, 0x1000, "RFC_FSCA",   P_STORE,     false },
    { 0x40080000, 0x1000, "WDT",        P_WDT,       false },
    { 0x40081000, 0x1000, "IOC",        P_STORE,     false },
    { 0x40082000, 0x1000, "PRCM",       P_PRCM,      true  },
    { 0x40083000, 0x1000, "EVENT",      P_STORE,     false },
    { 0x40084000, 0x1000, "SMPH",       P_SMPH,      false },
    { 0x40086000, 0x0200, "ADI2",       P_ADI2,      false },
    { 0x40086200, 0x0200, "ADI3",       P_ADI3,      false },
    { 0x40090000, 0x1000, "AON_PMCTL",  P_AON_PMCTL, true  },
    { 0x40092000, 0x1000, "AON_RTC",    P_AON_RTC,   true  },
    { 0x40093000, 0x1000, "AON_EVENT",  P_STORE,     true  },
    { 0x40094000, 0x1000, "AON_IOC",    P_AON_IOC,   true  },
    { 0x40095000, 0x1000, "AON_BATMON", P_STORE,     false },
    { 0x400C1000, 0x1000, "AUX_SPIM",   P_STORE,     false },
    { 0x400C3000, 0x1000, "AUX_TIMER2", P_STORE,     false },
    { 0x400C4000, 0x1000, "AUX_TDC",    P_AUX_TDC,   false },
    { 0x400C5000, 0x1000, "AUX_EVCTL",  P_AUX_EVCTL, false },
    { 0x400C6000, 0x1000, "AUX_SYSIF",  P_AUX_SYSIF, false },
    { 0x400C7000, 0x1000, "AUX_TIMER01",P_STORE,     false },
    { 0x400C8000, 0x1000, "AUX_SMPH",   P_AUX_SMPH,  false },
    { 0x400C9000, 0x1000, "AUX_ANAIF",  P_STORE,     false },
    { 0x400CA000, 0x1000, "AUX_DDI0",   P_DDI0,      false },
    { 0x400CB000, 0x1000, "AUX_ADI4",   P_ADI4,      false },
    { 0x400CC000, 0x4000, "AUX_AIODIO", P_STORE,     false },
    { 0x50000000, 0x1000, "FLASH_CFG",  P_STORE,     false },
    { CC13X2_FCFG1_BASE, CC13X2_FCFG1_SIZE, "FCFG1", P_FCFG1, false },
    { CC13X2_CCFG_BASE,  CC13X2_CCFG_SIZE,  "CCFG",  P_CCFG,  false },
};
#define NUM_PAGE_DESCS ((int)(sizeof(page_descs) / sizeof(page_descs[0])))

static cc13x2_page_t *find_page(cc13x2_soc_t *soc, const char *name) {
    for (int i = 0; i < soc->num_pages; i++)
        if (strcmp(soc->pages[i].name, name) == 0) return &soc->pages[i];
    return NULL;
}

#define REG(page, off)  ((page)->regs[(off) >> 2])

/* ============================================================
 * Factory configuration (FCFG1)
 * ============================================================ */

/* FCFG1 offsets used here (inc/hw_fcfg1.h). */
#define FCFG1_MAC_BLE_0          0x2E8
#define FCFG1_MAC_BLE_1          0x2EC
#define FCFG1_MAC_15_4_0         0x2F0
#define FCFG1_MAC_15_4_1         0x2F4

/* FCFG1 as read from a LAUNCHXL-CC1352R1 (rev E, CC1352R PG2.1) with the
 * XDS110 and openocd:
 *   dump_image fcfg1.bin 0x50001000 0x430
 * with the words that identify that one chip — MAC_BLE_0/1, MAC_15_4_0/1
 * and SHDW_DIE_ID_0..3 — set to the erased value.
 * What the boot path depends on, as driverlib's chipinfo.c and TI's
 * prebuilt drivers decode it:
 *   ICEPICK_DEVICE_ID 0x3BB4102F: PG_REV 3 ("PG2.1 or later"), WAFER_ID
 *     0xBB41 (FAMILY_CC13x2_CC26x2), MANUFACTURER_ID 0x02F (TI) — the JTAG
 *     IDCODE openocd reports.
 *   USER_ID 0x3082F000: CC13 = 1 and PA = 0 (CC1352R), PKG 2 (7x7 RGZ).
 *   MISC_CONF_1.DEVICE_MINOR_REV = 0 -> HWREV_2_1.
 *   TFW_FT % 10000 = 756: Power_init() spins unless it is > 682.
 *   IOCONF.GPIO_CNT = 28: the PIN driver sizes its pin range from it.
 * The trim words are this one chip's; SetupTrimDevice() copies them into
 * analog registers that csim stores and does not interpret.  The address
 * words (MAC_15_4, MAC_BLE) are filled in per node (fcfg1_init). */
static const uint32_t fcfg1_launchpad[268] = {
    /* 0x000 */ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    /* 0x018 */ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    /* 0x030 */ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    /* 0x048 */ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    /* 0x060 */ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    /* 0x078 */ 0x00000000, 0x00000000, 0xec29a8ae, 0x00a2ba8a, 0x0028a800, 0xa2a80000,
    /* 0x090 */ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xffffff00, 0xffffffff,
    /* 0x0a8 */ 0x03040588, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x0c0 */ 0xffffffff, 0x79054f01, 0x77048f01, 0x8bffd54d, 0x2830105f, 0x5ffff07f,
    /* 0x0d8 */ 0x2ffff07f, 0x5ffff07f, 0x2840405f, 0x584ff05f, 0x582ed05f, 0x2840005f,
    /* 0x0f0 */ 0x5840005f, 0x5840205f, 0x2850b05f, 0x5850b05f, 0x582f605f, 0x5840605f,
    /* 0x108 */ 0x67c7cf73, 0xffff29f1, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x120 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x138 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x150 */ 0xffffffff, 0xd1871174, 0xffffffff, 0xffffffff, 0xffffff05, 0x00060ef1,
    /* 0x168 */ 0x7f3a0015, 0x0017002b, 0x4c644c64, 0x0a0a2000, 0x02c10200, 0x0200f000,
    /* 0x180 */ 0x00000001, 0x00050014, 0x0fa00010, 0x00000fa0, 0x0a020804, 0x06080701,
    /* 0x198 */ 0x06080808, 0x31f6061e, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x1b0 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x0014ffff, 0xffff000c,
    /* 0x1c8 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x1e0 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x1f8 */ 0xffffffff, 0xffffffff, 0x0000ffff, 0xffffffff, 0x09a2ffff, 0xffffffff,
    /* 0x210 */ 0x0000ffff, 0xffffffff, 0x0962ffff, 0xffffffff, 0x0000ffff, 0xffffffff,
    /* 0x228 */ 0x09efffff, 0xffffffff, 0x0000ffff, 0xffffffff, 0x0b8bffff, 0xffffffff,
    /* 0x240 */ 0x0000ffff, 0xffffffff, 0x0b13ffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x258 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x270 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x288 */ 0xffffffff, 0xffffffff, 0xfffffffe, 0x3082f000, 0xffc5c500, 0xff000000,
    /* 0x2a0 */ 0xfff2c500, 0xffffffff, 0x0003e703, 0x2f2f2ff3, 0x00112c03, 0xca57f87f,
    /* 0x2b8 */ 0xfaf8e0fb, 0xf4f2f0fc, 0xe8fff9f8, 0x9651a6df, 0xffffff2f, 0xffffffff,
    /* 0x2d0 */ 0xffffffff, 0xffffffff, 0xfffffffd, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x2e8 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x000017bc, 0x000017ff,
    /* 0x300 */ 0xffffffff, 0xff27fe07, 0x98989f9f, 0xffff393b, 0x00000003, 0x00010001,
    /* 0x318 */ 0x3bb4102f, 0x00000028, 0x0000cfff, 0x8f90917f, 0x8b8c8d8e, 0x8788898a,
    /* 0x330 */ 0x83848586, 0x9e7f7f7f, 0x9a9b9c9d, 0x96979899, 0x92939495, 0xffffff1c,
    /* 0x348 */ 0xd0952dac, 0x3460f483, 0xdc07ff58, 0xffffffff, 0xffffffff, 0x0000815b,
    /* 0x360 */ 0x0000842a, 0x00000000, 0x00fc00fd, 0x0000c08c, 0xff7b828e, 0x6b8b0303,
    /* 0x378 */ 0xff483f47, 0xffffc3ff, 0xffffffff, 0xff1e551e, 0xf5eee5fd, 0xf00ffffe,
    /* 0x390 */ 0xffffffff, 0xffffffff, 0xe08403f8, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x3a8 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x00000000,
    /* 0x3c0 */ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xffffffff, 0xffffffff,
    /* 0x3d8 */ 0xffffffff, 0xffffffff, 0x00000001, 0x00000000, 0x00000000, 0x77000000,
    /* 0x3f0 */ 0xc0800001, 0x2af108aa, 0x00000ee5, 0x0a036b82, 0x92c5df93, 0x6a544fb3,
    /* 0x408 */ 0xffffc420, 0xfffe70ff, 0xffffffff, 0xffffffff, 0x30efa141, 0x30f5bbc4,
    /* 0x420 */ 0x04c10011, 0x09ac04ff, 0x05980012, 0x0b260017,
};

#define FCFG1_USER_ID            0x294
#define FCFG1_ICEPICK_DEVICE_ID  0x318

/* The parts.  The CC1352R is the LaunchPad above, as read.  The CC1312R7
 * takes the same FCFG1 with the two words that say which chip this is
 * replaced, as the CC13x2x7/CC26x2x7 driverlib decodes them (chipinfo.c):
 *   ICEPICK_DEVICE_ID 0x1BB7702F: PG_REV 1 (case 1, "PG1.1 or later"),
 *     WAFER_ID 0xBB77 (FAMILY_CC13x2x7_CC26x2x7; any other wafer ID makes
 *     ThisLibraryIsFor_CC13x2x7_CC26x2x7_HaltIfViolated() spin), TI.
 *   USER_ID 0x10828000: PROTOCOL 0x8 with CC13 = 1 and PA = 0
 *     (CHIP_TYPE_CC1312R7), PKG 2 (7x7 RGZ), PG_REV 1.
 *   RFCMODEHWOPT 0x29: modes 0 (RF_MODE_AUTO), 3 and 5, i.e. the
 *     proprietary protocol bit and no BLE or IEEE 802.15.4 (chipinfo.c
 *     reads bits [3:1]).
 * Not read from silicon: no CC1312R7 has been dumped yet.  The trim words
 * are the CC1352R's; the model does not interpret them. */
const cc13x2_part_t cc13x2_part_cc1352r = {
    .name = "CC1352R", .flash_size = 352u * 1024u,
    .icepick_id = 0x3BB4102Fu, .user_id = 0x3082F000u, .rfcmodehwopt = 0x2Fu,
    .rom = cc13x2_rom_image, .rom_size = &cc13x2_rom_image_size,
};
const cc13x2_part_t cc13x2_part_cc1312r7 = {
    .name = "CC1312R7", .flash_size = 704u * 1024u,
    .icepick_id = 0x1BB7702Fu, .user_id = 0x10828000u, .rfcmodehwopt = 0x29u,
    .rom = cc13x2x7_rom_image, .rom_size = &cc13x2x7_rom_image_size,
};

static void fcfg1_init(cc13x2_soc_t *soc) {
    memset(soc->fcfg1, 0, sizeof(soc->fcfg1));
    memcpy(soc->fcfg1, fcfg1_launchpad, sizeof(fcfg1_launchpad));
    soc->fcfg1[FCFG1_ICEPICK_DEVICE_ID / 4] = soc->part->icepick_id;
    soc->fcfg1[FCFG1_USER_ID / 4]           = soc->part->user_id;
    uint32_t *f = soc->fcfg1;
    f[FCFG1_MAC_15_4_0 / 4]        = soc->mac_15_4[0];
    f[FCFG1_MAC_15_4_1 / 4]        = soc->mac_15_4[1];
    f[FCFG1_MAC_BLE_0 / 4]         = soc->mac_15_4[0];
    f[FCFG1_MAC_BLE_1 / 4]         = 0xFFFF0000u | (soc->mac_15_4[1] & 0xFFFFu);
}

void cc13x2_soc_set_ieee_addr(cc13x2_soc_t *soc, const uint8_t addr[8]) {
    /* ieee-addr.c reads the 64-bit address from MAC_15_4_0 (low word) and
     * MAC_15_4_1, most significant byte last in memory, and reverses it. */
    soc->mac_15_4[0] = (uint32_t)addr[7] | ((uint32_t)addr[6] << 8) |
                       ((uint32_t)addr[5] << 16) | ((uint32_t)addr[4] << 24);
    soc->mac_15_4[1] = (uint32_t)addr[3] | ((uint32_t)addr[2] << 8) |
                       ((uint32_t)addr[1] << 16) | ((uint32_t)addr[0] << 24);
    soc->fcfg1[FCFG1_MAC_15_4_0 / 4] = soc->mac_15_4[0];
    soc->fcfg1[FCFG1_MAC_15_4_1 / 4] = soc->mac_15_4[1];
}

/* ============================================================
 * Oscillators (AUX_DDI0_OSC) — inc/hw_ddi_0_osc.h
 * ============================================================ */

#define DDI0_CTL0     0x00
#define DDI0_STAT0    0x3C
#define DDI0_STAT1    0x40
#define DDI0_STAT2    0x44

#define CTL0_SCLK_HF_SRC_SEL   0x00000001u
#define CTL0_SCLK_LF_SRC_SEL_S 2

/* XOSC_HF start-up: the TRM gives ~200-500 us for the 48 MHz crystal path;
 * TI's Power driver allows for it by polling. 300 us is a mid value. */
#define XOSC_HF_STARTUP_NS     300000

/* PRCM oscillator interrupt (OSCIMSC 0x290 / OSCRIS 0x294 / OSCICR 0x298,
 * inc/hw_prcm.h): HFSRCPEND [7] latches when an SCLK_HF switch becomes
 * possible, LFSRCDONE [6] when SCLK_LF reaches its selected source. TI's
 * Power driver commits the HF switch from the INT_OSC_COMB handler
 * (PowerCC26X2.c oscillatorISR -> OSCHfSourceSwitch -> HAPI). */
#define OSCRIS_HFSRCPEND  0x80u
#define OSCRIS_LFSRCDONE  0x40u
static void osc_irq_latch(cc13x2_soc_t *soc, uint32_t bits);
static bool osc_switch_pending(cc13x2_soc_t *soc);

static void osc_xosc_ready_cb(void *user, cpu_event_t *ev) {
    (void)ev;
    cc13x2_soc_t *soc = user;
    soc->osc.xosc_hf_starting = false;
    soc->osc.xosc_hf_ready = true;
    if (osc_switch_pending(soc)) osc_irq_latch(soc, OSCRIS_HFSRCPEND);
}

/* A write to CTL0 that selects XOSC_HF as the SCLK_HF source starts the
 * crystal; the switch itself is committed by the ROM's HFSourceSafeSwitch
 * (HAPI port below) once STAT0.PENDINGSCLKHFSWITCHING reports it ready. */
static void osc_ctl0_changed(cc13x2_soc_t *soc) {
    uint32_t ctl0 = soc->ddi0[DDI0_CTL0 / 4];
    bool want_xosc = (ctl0 & CTL0_SCLK_HF_SRC_SEL) != 0;
    if (want_xosc && !soc->osc.xosc_hf_ready && !soc->osc.xosc_hf_starting) {
        soc->osc.xosc_hf_starting = true;
        soc->osc.xosc_hf_event.callback = osc_xosc_ready_cb;
        soc->osc.xosc_hf_event.user_data = soc;
        schedule_at(soc, &soc->osc.xosc_hf_event, live_ns(soc) + XOSC_HF_STARTUP_NS);
    }
    if (!want_xosc && soc->osc.hf_src == 0) {
        /* Back on RCOSC_HF with nothing pending: the crystal may stop. */
        if (soc->osc.xosc_hf_starting)
            arm_cancel_event(&soc->plat->cpu, &soc->osc.xosc_hf_event);
        soc->osc.xosc_hf_starting = false;
        soc->osc.xosc_hf_ready = false;
    }
    /* SCLK_LF follows its selection directly. The LF crystal's real
     * start-up is hundreds of ms and qualified by hardware; nothing in the
     * firmware waits on it except through STAT0.SCLK_LF_SRC, and csim's
     * RTC runs at exactly 32768 Hz whatever the source. */
    int lf = (int)((ctl0 >> CTL0_SCLK_LF_SRC_SEL_S) & 3u);
    if (lf != soc->osc.lf_src) {
        soc->osc.lf_src = lf;
        osc_irq_latch(soc, OSCRIS_LFSRCDONE);
    }
    if (want_xosc && soc->osc.xosc_hf_ready && osc_switch_pending(soc))
        osc_irq_latch(soc, OSCRIS_HFSRCPEND);
}

static bool osc_switch_pending(cc13x2_soc_t *soc) {
    int sel = (soc->ddi0[DDI0_CTL0 / 4] & CTL0_SCLK_HF_SRC_SEL) ? 1 : 0;
    if (sel == soc->osc.hf_src) return false;
    return sel == 0 || soc->osc.xosc_hf_ready;
}

static uint32_t osc_status(cc13x2_soc_t *soc, uint32_t off) {
    switch (off) {
    case DDI0_STAT0: {
        uint32_t v = 0;
        v |= (uint32_t)soc->osc.lf_src << 29;           /* SCLK_LF_SRC */
        v |= (uint32_t)soc->osc.hf_src << 28;           /* SCLK_HF_SRC */
        v |= 1u << 22;                                  /* RCOSC_HF_EN */
        if (soc->osc.lf_src == 2) v |= 1u << 21;        /* RCOSC_LF_EN */
        if (soc->osc.lf_src == 3) v |= 1u << 20;        /* XOSC_LF_EN */
        v |= (1u << 19) | (1u << 18);                   /* CLK_DCDC_RDY(_ACK) */
        if (soc->osc.xosc_hf_ready) v |= 1u << 15;      /* XOSC_HF_EN */
        if (osc_switch_pending(soc)) v |= 1u;           /* PENDINGSCLKHFSWITCHING */
        return v;
    }
    case DDI0_STAT1:
        /* All derived clocks enabled [14:8] and good [7:0]. */
        return 0x00007FFFu;
    case DDI0_STAT2:
        /* XOSC_HF_AMPGOOD | FREQGOOD | RF_FREQGOOD once running. */
        return soc->osc.xosc_hf_ready ? 0x7u : 0x0u;
    }
    return soc->ddi0[off / 4];
}

/* HAPI port: the stand-in ROM's HFSourceSafeSwitch commits the switch. */
static void hapi_write(cc13x2_soc_t *soc, uint32_t off, uint32_t val) {
    if (off == 0 && val == 1) {
        if (osc_switch_pending(soc))
            soc->osc.hf_src = (soc->ddi0[DDI0_CTL0 / 4] & CTL0_SCLK_HF_SRC_SEL) ? 1 : 0;
        return;
    }
    if (off == 4) {
        /* tools/cc13x2-rom/hapi.c rom_unimplemented: val is its caller. */
        fprintf(stderr, "[cc13x2] node %d: call into a ROM slot the stand-in ROM does "
                "not implement, from 0x%08x; the CPU is parked in a bkpt loop\n",
                soc->rfc.node_id, val & ~1u);
        return;
    }
    fprintf(stderr, "[cc13x2] HAPI port: unknown request 0x%x at +0x%x\n", val, off);
}

/* DDI: 32-bit registers.  DIR 0x000, SET 0x080, CLR 0x100, and masked
 * writes MASK4B 0x200 (byte per nibble), MASK8B 0x300 (halfword per byte),
 * MASK16B 0x400 (word per halfword): each n-bit unit of the register file
 * maps to a 2n-bit write carrying {mask, data} (inc/hw_ddi.h,
 * driverlib/ddi.c DDI16BitWrite / DDI16BitfieldWrite). */
static void ddi0_write(cc13x2_soc_t *soc, uint32_t off, uint32_t val) {
    uint32_t *r = soc->ddi0;
    uint32_t nregs = sizeof(soc->ddi0);   /* bytes of register file */
    if (off < 0x080) {
        if (off < nregs)
            r[off / 4] = merge_write(soc, r[off / 4], off, val);
    } else if (off < 0x100) {
        uint32_t o = off - 0x080;
        if (o < nregs) r[o / 4] |= merge_write(soc, 0, o, val);
    } else if (off < 0x200) {
        uint32_t o = off - 0x100;
        if (o < nregs) r[o / 4] &= ~merge_write(soc, 0, o, val);
    } else if (off < 0x300) {            /* 4-bit: one byte per nibble */
        uint32_t o = off - 0x200, nib = o;        /* nibble index in file */
        uint32_t reg = nib / 8, sh = (nib % 8) * 4;
        uint32_t mask = ((val >> 4) & 0xFu) << sh, data = (val & 0xFu) << sh;
        if (reg * 4 < nregs) r[reg] = (r[reg] & ~mask) | (data & mask);
    } else if (off < 0x400) {            /* 8-bit: one halfword per byte */
        uint32_t byte = (off - 0x300) / 2;
        uint32_t reg = byte / 4, sh = (byte % 4) * 8;
        uint32_t mask = ((val >> 8) & 0xFFu) << sh, data = (val & 0xFFu) << sh;
        if (reg * 4 < nregs) r[reg] = (r[reg] & ~mask) | (data & mask);
    } else if (off < 0x500) {            /* 16-bit: one word per halfword */
        uint32_t half = (off - 0x400) / 4;
        uint32_t reg = half / 2, sh = (half % 2) * 16;
        uint32_t mask = ((val >> 16) & 0xFFFFu) << sh, data = (val & 0xFFFFu) << sh;
        if (reg * 4 < nregs) r[reg] = (r[reg] & ~mask) | (data & mask);
    }
    uint32_t reg_touched = (off < 0x200) ? (off & 0x7F) :
                           (off < 0x300) ? ((off - 0x200) / 8) * 4 :
                           (off < 0x400) ? ((off - 0x300) / 8) * 4 :
                                           ((off - 0x400) / 8) * 4;
    if (reg_touched == DDI0_CTL0) osc_ctl0_changed(soc);
}

static uint32_t ddi0_read(cc13x2_soc_t *soc, uint32_t off) {
    if (off >= 0x080) return 0;          /* the write-only windows read 0 */
    uint32_t aligned = off & ~3u;
    if (aligned >= sizeof(soc->ddi0)) return 0;
    return osc_status(soc, aligned) >> ((off & 3u) * 8u);
}

/* ADI: sixteen 8-bit registers.  DIR 0x00, SET 0x10, CLR 0x20, SLAVESTAT
 * 0x30, SLAVECONF 0x38, MASK4B 0x40 (byte per nibble), MASK8B 0x60
 * (halfword per register), MASK16B 0x80 (word per register pair)
 * (inc/hw_adi.h, driverlib/adi.h). */
static void adi_write(cc13x2_soc_t *soc, uint8_t *r, uint32_t off, uint32_t val) {
    uint32_t w = width_of(soc);
    if (off < 0x30) {
        uint32_t win = off & 0x30, base = off & 0x0F;
        for (uint32_t i = 0; i < w && base + i < 16; i++) {
            uint8_t b = (uint8_t)(val >> (8 * i));
            if (win == 0x00) r[base + i] = b;
            else if (win == 0x10) r[base + i] |= b;
            else r[base + i] &= (uint8_t)~b;
        }
    } else if (off >= 0x40 && off < 0x60) {
        uint32_t nib = off - 0x40, reg = nib / 2, sh = (nib % 2) * 4;
        uint8_t mask = (uint8_t)(((val >> 4) & 0xFu) << sh);
        uint8_t data = (uint8_t)((val & 0xFu) << sh);
        r[reg] = (uint8_t)((r[reg] & ~mask) | (data & mask));
    } else if (off >= 0x60 && off < 0x80) {
        uint32_t reg = (off - 0x60) / 2;
        uint8_t mask = (uint8_t)(val >> 8), data = (uint8_t)val;
        r[reg] = (uint8_t)((r[reg] & ~mask) | (data & mask));
    } else if (off >= 0x80 && off < 0xA0) {
        uint32_t reg = ((off - 0x80) / 4) * 2;
        uint16_t mask = (uint16_t)(val >> 16), data = (uint16_t)val;
        for (int i = 0; i < 2; i++) {
            uint8_t m = (uint8_t)(mask >> (8 * i)), d = (uint8_t)(data >> (8 * i));
            r[reg + i] = (uint8_t)((r[reg + i] & ~m) | (d & m));
        }
    }
}

static uint32_t adi_read(uint8_t *r, uint32_t off) {
    if (off < 0x10) {
        uint32_t v = 0;
        for (uint32_t i = 0; i < 4 && off + i < 16; i++)
            v |= (uint32_t)r[off + i] << (8 * i);
        return v;
    }
    if (off == 0x30) return 0;   /* SLAVESTAT: no request outstanding */
    return 0;
}

/* ============================================================
 * PRCM — inc/hw_prcm.h
 * ============================================================ */

#define PRCM_CLKLOADCTL   0x028
#define PRCM_PDCTL0       0x12C
#define PRCM_PDCTL0RFC    0x130
#define PRCM_PDCTL0SERIAL 0x134
#define PRCM_PDCTL0PERIPH 0x138
#define PRCM_PDSTAT0      0x140
#define PRCM_PDSTAT0RFC   0x144
#define PRCM_PDSTAT0SER   0x148
#define PRCM_PDSTAT0PER   0x14C
#define PRCM_PDCTL1       0x17C
#define PRCM_PDCTL1CPU    0x184
#define PRCM_PDCTL1RFC    0x188
#define PRCM_PDCTL1VIMS   0x18C
#define PRCM_PDSTAT1      0x194
#define PRCM_PDSTAT1BUS   0x198
#define PRCM_PDSTAT1RFC   0x19C
#define PRCM_PDSTAT1CPU   0x1A0
#define PRCM_PDSTAT1VIMS  0x1A4
#define PRCM_RFCMODEHWOPT 0x1D4

/* PDCTL0: RFC_ON bit0, SERIAL_ON bit1, PERIPH_ON bit2.
 * PDCTL1: CPU_ON bit1, RFC_ON bit2, VIMS_MODE bit3.
 * Power domains switch in a few us on silicon; the driverlib callers poll
 * PDSTAT, so reporting the new state at once is indistinguishable. */
static bool rfc_powered(cc13x2_soc_t *soc) {
    return (soc->pd0 & 1u) || (soc->pd1 & 4u);
}

#define PRCM_OSCIMSC 0x290
#define PRCM_OSCRIS  0x294
#define PRCM_OSCICR  0x298

static void osc_irq_update(cc13x2_soc_t *soc) {
    cc13x2_page_t *p = soc->pg_prcm;
    if (p && (REG(p, PRCM_OSCRIS) & REG(p, PRCM_OSCIMSC)))
        irq_raise(soc, CC13X2_IRQ_OSC_COMB);
}

static void osc_irq_latch(cc13x2_soc_t *soc, uint32_t bits) {
    cc13x2_page_t *p = soc->pg_prcm;
    if (!p) return;
    REG(p, PRCM_OSCRIS) |= bits;
    osc_irq_update(soc);
}

static uint32_t prcm_read(cc13x2_soc_t *soc, cc13x2_page_t *p, uint32_t off) {
    switch (off & ~3u) {
    case PRCM_CLKLOADCTL: return 0x2u;                 /* LOAD_DONE */
    case PRCM_PDCTL0:     return soc->pd0;
    case PRCM_PDCTL0RFC:  return soc->pd0 & 1u;
    case PRCM_PDCTL0SERIAL: return (soc->pd0 >> 1) & 1u;
    case PRCM_PDCTL0PERIPH: return (soc->pd0 >> 2) & 1u;
    case PRCM_PDSTAT0:    return (soc->pd0 & 6u) | (rfc_powered(soc) ? 1u : 0u);
    case PRCM_PDSTAT0RFC: return rfc_powered(soc) ? 1u : 0u;
    case PRCM_PDSTAT0SER: return (soc->pd0 >> 1) & 1u;
    case PRCM_PDSTAT0PER: return (soc->pd0 >> 2) & 1u;
    case PRCM_PDCTL1:     return soc->pd1;
    case PRCM_PDSTAT1:
        return 0x10u | 0x08u | 0x02u | (rfc_powered(soc) ? 0x04u : 0u);
    case PRCM_PDSTAT1BUS: case PRCM_PDSTAT1CPU: case PRCM_PDSTAT1VIMS:
        return 1u;
    case PRCM_PDSTAT1RFC: return rfc_powered(soc) ? 1u : 0u;
    }
    return REG(p, off & ~3u) >> ((off & 3u) * 8u);
}

static void prcm_write_impl(cc13x2_soc_t *soc, cc13x2_page_t *p, uint32_t off, uint32_t val);

static void wdt_set_standby(cc13x2_soc_t *soc, bool standby);

static void prcm_write(cc13x2_soc_t *soc, cc13x2_page_t *p, uint32_t off, uint32_t val) {
    bool was = rfc_powered(soc);
    prcm_write_impl(soc, p, off, val);
    bool on = rfc_powered(soc);
    if (on != was) cc13x2_rfc_domain_changed(&soc->rfc, on);
    wdt_set_standby(soc, !(soc->pd0 & 4u));    /* PERIPH domain off = standby */
}

static void prcm_write_impl(cc13x2_soc_t *soc, cc13x2_page_t *p, uint32_t off, uint32_t val) {
    uint32_t a = off & ~3u;
    switch (a) {
    case PRCM_CLKLOADCTL: return;
    case PRCM_PDCTL0:     soc->pd0 = val & 7u; return;
    case PRCM_PDCTL0RFC:  soc->pd0 = (soc->pd0 & ~1u) | (val & 1u); return;
    case PRCM_PDCTL0SERIAL: soc->pd0 = (soc->pd0 & ~2u) | ((val & 1u) << 1); return;
    case PRCM_PDCTL0PERIPH: soc->pd0 = (soc->pd0 & ~4u) | ((val & 1u) << 2); return;
    case PRCM_PDCTL1:     soc->pd1 = val & 0xEu; return;
    case PRCM_PDCTL1CPU:  soc->pd1 = (soc->pd1 & ~2u) | ((val & 1u) << 1); return;
    case PRCM_PDCTL1RFC:  soc->pd1 = (soc->pd1 & ~4u) | ((val & 1u) << 2); return;
    case PRCM_PDCTL1VIMS: soc->pd1 = (soc->pd1 & ~8u) | ((val & 1u) << 3); return;
    case PRCM_OSCRIS:     return;                          /* read-only */
    case PRCM_OSCICR:     REG(p, PRCM_OSCRIS) &= ~val; return;
    case PRCM_OSCIMSC:
        REG(p, a) = merge_write(soc, REG(p, a), off, val) & 0xFFu;
        osc_irq_update(soc);
        return;
    }
    REG(p, a) = merge_write(soc, REG(p, a), off, val);
}

/* ============================================================
 * AON_PMCTL — inc/hw_aon_pmctl.h
 * ============================================================ */

#define AON_PMCTL_PWRSTAT  0x14
#define AON_PMCTL_RESETCTL 0x28
#define AON_PMCTL_SLEEPCTL 0x2C
#define RESETCTL_SYSRESET  0x80000000u

static void aon_pmctl_write(cc13x2_soc_t *soc, cc13x2_page_t *p, uint32_t off, uint32_t val) {
    uint32_t a = off & ~3u;
    uint32_t v = merge_write(soc, REG(p, a), off, val);
    if (a == AON_PMCTL_RESETCTL) {
        if (v & RESETCTL_SYSRESET) {
            /* SysCtrlSystemReset(): a full system reset. RESET_SRC[3:1]
             * reads SYSRESET (6) on the next boot. */
            REG(p, a) = (REG(p, a) & ~0xEu) | 0xCu;
            arm_request_reset(&soc->plat->cpu);
            return;
        }
        /* BOOT_DET_x_SET / _CLR [25:24],[17:16] act on BOOT_DET [13:12]. */
        uint32_t bd = (REG(p, a) >> 12) & 3u;
        bd |= (v >> 16) & 3u;
        bd &= ~((v >> 24) & 3u);
        v = (v & ~0x3000u) | (bd << 12);
        v &= ~0xEu;                      /* RESET_SRC is read-only */
        v |= REG(p, a) & 0xEu;
    }
    REG(p, a) = v;
}

static uint32_t aon_pmctl_read(cc13x2_soc_t *soc, cc13x2_page_t *p, uint32_t off) {
    (void)soc;
    uint32_t a = off & ~3u;
    if (a == AON_PMCTL_PWRSTAT)
        /* AUX_RESET_DONE | AUX_BUS_RESET_DONE; JTAG_PD_ON stays 0 — no
         * debugger is attached, and TI's Power driver treats a powered
         * JTAG domain as a reason to stay out of standby. */
        return 0x3u >> ((off & 3u) * 8u);
    return REG(p, a) >> ((off & 3u) * 8u);
}

/* ============================================================
 * AON RTC — inc/hw_aon_rtc.h, driverlib/aon_rtc.c
 * ============================================================ */

#define RTC_CTL      0x00
#define RTC_EVFLAGS  0x04
#define RTC_SEC      0x08
#define RTC_SUBSEC   0x0C
#define RTC_SUBSECINC 0x10
#define RTC_CHCTL    0x14
#define RTC_CH0CMP   0x18
#define RTC_CH1CMP   0x1C
#define RTC_CH2CMP   0x20
#define RTC_CH2CMPINC 0x24
#define RTC_CH1CAPT  0x28
#define RTC_SYNC     0x2C
#define RTC_TIME     0x30
#define RTC_SYNCLF   0x34

#define RTC_CTL_EN       0x00000001u
#define RTC_CTL_RESET    0x00000080u
#define RTC_CTL_COMB_S   16
/* EVFLAGS / CHCTL channel bits: CH0 bit0, CH1 bit8, CH2 bit16. */
static const uint32_t rtc_ch_bit[3] = { 1u << 0, 1u << 8, 1u << 16 };

/* 32768 Hz SCLK_LF ticks since the counter was zeroed. */
static uint64_t rtc_ticks(cc13x2_soc_t *soc) {
    if (!soc->rtc.enabled) return 0;
    int64_t el = live_ns(soc) - soc->rtc.zero_ns;
    if (el < 0) el = 0;
    return ((uint64_t)el * 4096u) / 125000000u;        /* el * 32768 / 1e9 */
}

static int64_t rtc_tick_ns(cc13x2_soc_t *soc, uint64_t tick) {
    return soc->rtc.zero_ns + (int64_t)((tick * 125000000u + 4095u) / 4096u);
}

/* ============================================================
 * Services for the RF core model
 * ============================================================ */

int64_t cc13x2_live_ns(cc13x2_soc_t *soc) { return live_ns(soc); }

/* Nanoseconds on the AON RTC at a live time: what its 64-bit counter
 * measures, unquantised — the RAT runs from the 48 MHz clock but is kept
 * locked to the RTC (CMD_SYNC_START_RAT). */
int64_t cc13x2_rtc_ns_at(cc13x2_soc_t *soc, int64_t ns) {
    if (!soc->rtc.enabled) return ns;
    int64_t el = ns - soc->rtc.zero_ns;
    return el < 0 ? 0 : el;
}

void cc13x2_schedule_at(cc13x2_soc_t *soc, cpu_event_t *ev, int64_t fire_ns) {
    schedule_at(soc, ev, fire_ns);
}

void cc13x2_cancel(cc13x2_soc_t *soc, cpu_event_t *ev) {
    arm_cancel_event(&soc->plat->cpu, ev);
}

void cc13x2_irq(cc13x2_soc_t *soc, int irq) { irq_raise(soc, irq); }

bool cc13x2_rfc_domain_on(cc13x2_soc_t *soc) { return rfc_powered(soc); }

/* The 16.16 view the compare channels and rtimer use (counter bits 47:16). */
static uint32_t rtc_time1616(uint64_t ticks) {
    return (uint32_t)(ticks << 1);
}

static void rtc_update_irq(cc13x2_soc_t *soc) {
    uint32_t comb = (soc->rtc.ctl >> RTC_CTL_COMB_S) & 7u;
    uint32_t hit = 0;
    for (int i = 0; i < 3; i++)
        if ((comb & (1u << i)) && (soc->rtc.evflags & rtc_ch_bit[i])) hit = 1;
    if (hit) irq_raise(soc, CC13X2_IRQ_AON_RTC_COMB);
}

static void rtc_arm_channel(cc13x2_soc_t *soc, int ch, bool armed_now);

/* CC13X2_RTC_TRACE=1: compare writes, arming decisions and channel events. */
static bool rtc_trace(void) {
    static int t = -1;
    return cc13x2_env_level(&t, "CC13X2_RTC_TRACE") != 0;
}

static void rtc_ch_cb(void *user, cpu_event_t *ev) {
    cc13x2_soc_t *soc = user;
    int ch = (int)(ev - soc->rtc.ch_event);
    if (!(soc->rtc.chctl & rtc_ch_bit[ch])) return;
    if (rtc_trace())
        fprintf(stderr, "[rtc %d %10.6f] ch%d event (time 0x%08x cmp 0x%08x)\n", soc->rfc.node_id,
                (double)live_ns(soc) / 1e9, ch, rtc_time1616(rtc_ticks(soc)),
                soc->rtc.cmp[ch]);
    soc->rtc.evflags |= rtc_ch_bit[ch];
    if (ch == 2 && (soc->rtc.chctl & (1u << 18))) {      /* CH2_CONT_EN */
        soc->rtc.cmp[2] += soc->rtc.ch2cmpinc;
        rtc_arm_channel(soc, 2, false);
    }
    rtc_update_irq(soc);
}

/* A channel event is generated when the 16.16 time "is reaching or
 * exceeding" the compare value, and "an event will be scheduled to occur as
 * soon as possible when writing to CH0CMP provided that the channel is
 * enabled and the new value matches any time between next RTC value and
 * 1 second in the past" (inc/hw_aon_rtc.h, CH0CMP / EVFLAGS.CH0).
 *
 * So: a future compare fires when the counter reaches it (the counter steps
 * by 2 in the 16.16 view, so an odd value fires on the edge crossing it); a
 * compare armed up to one second behind the counter fires at once; one
 * further behind is treated as future and waits for the 32-bit view to wrap.
 * "Armed" is a CHxCMP write or the channel being enabled: driverlib's
 * rtimer path writes the compare and then enables the channel
 * (rtimer-arch.c), and TI's TimerP race — a higher-priority ISR running
 * between its read of the counter and its compare write — lands compares in
 * the past exactly this way.
 *
 * Every event is registered one SCLK_LF edge after the match. Not stated in
 * the TRM; it is what TimerP's arithmetic needs (compare = threshold >> 16,
 * and its ISR treats a wakeup as spurious unless RTC64 / period64 has reached
 * the service tick, which an event on the matching edge fails).
 *
 * All of this is measured on a LaunchPad (tools/cc13x2-hwcheck, devices/
 * cc1352r1-launchpad/HARDWARE-COMPARISON.md): the flag appears at cmp+2
 * (even compare) or cmp+3 (odd), and a compare written or enabled from 2
 * units to 0.875 s in the past fires on the next edge, 1.5 s does not. */
static void rtc_arm_channel(cc13x2_soc_t *soc, int ch, bool armed_now) {
    arm_cpu_t *cpu = &soc->plat->cpu;
    cpu_event_t *ev = &soc->rtc.ch_event[ch];
    arm_cancel_event(cpu, ev);
    if (!soc->rtc.enabled || !(soc->rtc.chctl & rtc_ch_bit[ch])) return;
    uint64_t now = rtc_ticks(soc);
    uint32_t t1616 = rtc_time1616(now);
    uint32_t diff = soc->rtc.cmp[ch] - t1616;
    uint64_t target;
    const char *why = "";
    if (diff != 0 && diff <= 0x80000000u) {
        target = now + (diff + 1u) / 2u + 1u;             /* reach, + 1 edge */
    } else if (armed_now && (uint32_t)(t1616 - soc->rtc.cmp[ch]) <= 0x10000u) {
        target = now + 1u;                                 /* as soon as possible */
        why = " past: as soon as possible";
    } else {
        if (rtc_trace())
            fprintf(stderr, "[rtc %d %10.6f] ch%d cmp 0x%08x now 0x%08x: behind, not re-armed\n",
                    soc->rfc.node_id, (double)live_ns(soc) / 1e9, ch,
                    soc->rtc.cmp[ch], t1616);
        return;
    }
    if (rtc_trace())
        fprintf(stderr, "[rtc %d %10.6f] ch%d arm cmp 0x%08x now 0x%08x%s\n",
                soc->rfc.node_id, (double)live_ns(soc) / 1e9, ch,
                soc->rtc.cmp[ch], t1616, why);
    ev->callback = rtc_ch_cb;
    ev->user_data = soc;
    schedule_at(soc, ev, rtc_tick_ns(soc, target));
}

static void rtc_rearm_all(cc13x2_soc_t *soc) {
    for (int i = 0; i < 3; i++) rtc_arm_channel(soc, i, false);
}

static uint32_t aon_rtc_read(cc13x2_soc_t *soc, uint32_t off) {
    uint32_t a = off & ~3u, v = 0;
    uint64_t t;
    switch (a) {
    case RTC_CTL:      v = soc->rtc.ctl; break;
    case RTC_EVFLAGS:  v = soc->rtc.evflags; break;
    case RTC_SEC:
        t = rtc_ticks(soc);
        soc->rtc.subsec_latch = (uint32_t)(t << 17);
        v = (uint32_t)(t >> 15);
        break;
    case RTC_SUBSEC:   v = soc->rtc.subsec_latch; break;
    case RTC_SUBSECINC: v = 0x800000u; break;   /* 2^38 / 32768: exactly 32768 Hz */
    case RTC_CHCTL:    v = soc->rtc.chctl; break;
    case RTC_CH0CMP:   v = soc->rtc.cmp[0]; break;
    case RTC_CH1CMP:   v = soc->rtc.cmp[1]; break;
    case RTC_CH2CMP:   v = soc->rtc.cmp[2]; break;
    case RTC_CH2CMPINC: v = soc->rtc.ch2cmpinc; break;
    case RTC_CH1CAPT:  v = soc->rtc.ch1capt; break;
    case RTC_SYNC:     v = 0; break;
    case RTC_TIME:     v = rtc_time1616(rtc_ticks(soc)); break;
    case RTC_SYNCLF:   v = (uint32_t)(rtc_ticks(soc) & 1u); break;
    }
    return v >> ((off & 3u) * 8u);
}

static void aon_rtc_write(cc13x2_soc_t *soc, uint32_t off, uint32_t val) {
    uint32_t a = off & ~3u;
    switch (a) {
    case RTC_CTL: {
        uint32_t v = merge_write(soc, soc->rtc.ctl, off, val);
        bool reset = (v & RTC_CTL_RESET) != 0;
        if (reset) {
            /* RESET: the counter returns to 0; the bit self-clears. */
            soc->rtc.zero_ns = live_ns(soc);
            v &= ~RTC_CTL_RESET;
        }
        bool en = (v & RTC_CTL_EN) != 0;
        if (en && !soc->rtc.enabled) {
            soc->rtc.zero_ns = live_ns(soc);
            soc->rtc.enabled = true;
        } else if (!en) {
            soc->rtc.enabled = false;
        }
        bool timebase = reset || ((soc->rtc.ctl ^ v) & RTC_CTL_EN);
        soc->rtc.ctl = v;
        if (timebase) rtc_rearm_all(soc);   /* counter started, stopped or zeroed */
        rtc_update_irq(soc);
        return;
    }
    case RTC_EVFLAGS:
        soc->rtc.evflags &= ~merge_write(soc, 0, off, val);
        return;
    case RTC_CHCTL: {
        /* Only a channel whose enable changes is affected: re-evaluating an
         * untouched one could cancel an event already due (the rtimer ISR
         * disables channel 1 at the instant channel 0's counter reached its
         * compare, and the re-evaluation dropped channel 0's pending event —
         * TI's ClockP then never woke again and the watchdog reset the node). */
        uint32_t was = soc->rtc.chctl;
        soc->rtc.chctl = merge_write(soc, soc->rtc.chctl, off, val);
        for (int i = 0; i < 3; i++) {
            bool on_before = (was & rtc_ch_bit[i]) != 0;
            bool on_now = (soc->rtc.chctl & rtc_ch_bit[i]) != 0;
            if (on_before != on_now) rtc_arm_channel(soc, i, on_now);
        }
        return;
    }
    case RTC_CH0CMP: case RTC_CH1CMP: case RTC_CH2CMP: {
        int ch = (int)(a - RTC_CH0CMP) / 4;
        soc->rtc.cmp[ch] = merge_write(soc, soc->rtc.cmp[ch], off, val);
        rtc_arm_channel(soc, ch, true);
        return;
    }
    case RTC_CH2CMPINC:
        soc->rtc.ch2cmpinc = merge_write(soc, soc->rtc.ch2cmpinc, off, val);
        return;
    case RTC_SYNC:
        return;
    }
}

/* ============================================================
 * GPIO — inc/hw_gpio.h
 * ============================================================ */

#define GPIO_DOUT31_0    0x80
#define GPIO_DOUTSET     0x90
#define GPIO_DOUTCLR     0xA0
#define GPIO_DOUTTGL     0xB0
#define GPIO_DIN31_0     0xC0
#define GPIO_DOE31_0     0xD0
#define GPIO_EVFLAGS     0xE0
#define GPIO_PIN_MASK    0x7FFFFFFFu

/* IOC:IOCFGn, one word per DIO at 4 x n (inc/hw_ioc.h). */
#define IOCFG_PULL_CTL_M   0x00006000u
#define IOCFG_PULL_UP      0x00004000u
#define IOCFG_EDGE_DET_NEG 0x00010000u
#define IOCFG_EDGE_DET_POS 0x00020000u
#define IOCFG_EDGE_IRQ_EN  0x00040000u
#define IOCFG_IE           0x20000000u
#define IRQ_AON_GPIO_EDGE  0           /* INT_AON_GPIO_EDGE (16) - 16 */

static uint32_t iocfg(cc13x2_soc_t *soc, int dio) {
    return soc->pg_ioc ? REG(soc->pg_ioc, (uint32_t)dio * 4u) : 0;
}

/* The level on each pad: driven from outside (a button), else by the
 * output driver, else by the IOCFG pull (an unpulled input reads 0). */
static uint32_t gpio_levels(cc13x2_soc_t *soc) {
    cc13x2_gpio_t *g = &soc->gpio;
    uint32_t v = g->dout & g->doe, pulled = 0;
    for (int dio = 0; dio < CC13X2_NUM_DIO; dio++)
        if ((iocfg(soc, dio) & IOCFG_PULL_CTL_M) == IOCFG_PULL_UP) pulled |= 1u << dio;
    v |= pulled & ~g->doe;
    v = (v & ~g->din_forced) | (g->din_ext & g->din_forced);
    return v & GPIO_PIN_MASK;
}

/* DIN: the pad level of input-enabled DIOs; an output reads back its own
 * level (driverlib's GPIO_readDio on an output pin). */
static uint32_t gpio_din(cc13x2_soc_t *soc) {
    uint32_t ie = soc->gpio.doe;
    for (int dio = 0; dio < CC13X2_NUM_DIO; dio++)
        if (iocfg(soc, dio) & IOCFG_IE) ie |= 1u << dio;
    return gpio_levels(soc) & ie;
}

/* An outside level on a DIO (button, `gpio` in the shell); level < 0 lets
 * the pad follow its driver or pull again.  An edge the IOCFG selects sets
 * EVFLAGS and, with EDGE_IRQ_EN, the AON GPIO edge interrupt. */
int cc13x2_soc_set_input_pin(cc13x2_soc_t *soc, int dio, int level) {
    if (dio < 0 || dio >= CC13X2_NUM_DIO) return -1;
    cc13x2_gpio_t *g = &soc->gpio;
    uint32_t bit = 1u << dio;
    uint32_t before = gpio_levels(soc);
    if (level < 0) {
        g->din_forced &= ~bit;
    } else {
        g->din_forced |= bit;
        g->din_ext = level ? (g->din_ext | bit) : (g->din_ext & ~bit);
    }
    uint32_t after = gpio_levels(soc);
    if (!((before ^ after) & bit)) return 0;
    uint32_t cfg = iocfg(soc, dio);
    uint32_t det = (after & bit) ? IOCFG_EDGE_DET_POS : IOCFG_EDGE_DET_NEG;
    if (cfg & det) {
        g->evflags |= bit;
        if (cfg & IOCFG_EDGE_IRQ_EN) irq_raise(soc, IRQ_AON_GPIO_EDGE);
    }
    return 0;
}

static uint32_t gpio_read(cc13x2_soc_t *soc, uint32_t off) {
    cc13x2_gpio_t *g = &soc->gpio;
    uint32_t a = off & ~3u, v = 0;
    if (a < 0x20) {
        /* DOUTn_m: one byte per DIO, bit 0 = the DIO's output. */
        uint32_t dio = a;
        for (int i = 0; i < 4; i++)
            v |= ((g->dout >> (dio + (uint32_t)i)) & 1u) << (8 * i);
    } else switch (a) {
        case GPIO_DOUT31_0: v = g->dout; break;
        case GPIO_DIN31_0:  v = gpio_din(soc); break;
        case GPIO_DOE31_0:  v = g->doe; break;
        case GPIO_EVFLAGS:  v = g->evflags; break;
    }
    return v >> ((off & 3u) * 8u);
}

/* CSIM_GPIO_TRACE=1: timestamped DOUT changes (same format as nRF54L15). */
static void gpio_trace(cc13x2_soc_t *soc, uint32_t before) {
    static int on = -1;
    if (!cc13x2_env_level(&on, "CSIM_GPIO_TRACE") || before == soc->gpio.dout) return;
    fprintf(stderr, "[GPIO] %9.3f DOUT 0x%08x -> 0x%08x (changed 0x%08x)\n",
            (double)live_ns(soc) / 1e9, before, soc->gpio.dout,
            before ^ soc->gpio.dout);
}

static void gpio_write_impl(cc13x2_soc_t *soc, uint32_t off, uint32_t val);

static void gpio_write(cc13x2_soc_t *soc, uint32_t off, uint32_t val) {
    uint32_t before = soc->gpio.dout;
    gpio_write_impl(soc, off, val);
    gpio_trace(soc, before);
}

static void gpio_write_impl(cc13x2_soc_t *soc, uint32_t off, uint32_t val) {
    cc13x2_gpio_t *g = &soc->gpio;
    uint32_t a = off & ~3u;
    if (off < 0x20) {
        /* Byte-wide per-DIO writes: bit 0 of each byte drives its DIO. */
        uint32_t w = width_of(soc);
        for (uint32_t i = 0; i < w; i++) {
            uint32_t dio = off + i;
            if (dio >= CC13X2_NUM_DIO) break;
            uint32_t bit = 1u << dio;
            g->dout = ((val >> (8 * i)) & 1u) ? (g->dout | bit) : (g->dout & ~bit);
        }
        return;
    }
    uint32_t v = merge_write(soc, 0, off, val);
    switch (a) {
    case GPIO_DOUT31_0: g->dout = merge_write(soc, g->dout, off, val) & GPIO_PIN_MASK; break;
    case GPIO_DOUTSET:  g->dout |= v & GPIO_PIN_MASK; break;
    case GPIO_DOUTCLR:  g->dout &= ~v; break;
    case GPIO_DOUTTGL:  g->dout ^= v & GPIO_PIN_MASK; break;
    case GPIO_DOE31_0:  g->doe = merge_write(soc, g->doe, off, val) & GPIO_PIN_MASK; break;
    case GPIO_EVFLAGS:  g->evflags &= ~v; break;
    }
}

/* ============================================================
 * TRNG — inc/hw_trng.h
 *
 * Zephyr's entropy driver (and TI's TRNG driver) enable the generator and
 * take each 64-bit number from OUT0/OUT1 when IRQFLAGSTAT.RDY raises the
 * TRNG interrupt; clearing RDY starts the next one.  The numbers come from
 * a xorshift64 stream seeded from the node's IEEE address, so a run is
 * reproducible and nodes differ.  TRNG_GEN_NS is a placeholder for the
 * FRO sampling time (set by CFG0/CTL.STARTUP_CYCLES on silicon).
 * ============================================================ */

#define TRNG_OUT0         0x000
#define TRNG_OUT1         0x004
#define TRNG_IRQFLAGSTAT  0x008
#define TRNG_IRQFLAGMASK  0x00C
#define TRNG_IRQFLAGCLR   0x010
#define TRNG_CTL          0x014
#define TRNG_IRQSTATMASK  0x1FD8
#define TRNG_IRQSET       0x1FEC
#define TRNG_SWRESET      0x1FF0
#define TRNG_IRQSTAT      0x1FF8
#define TRNG_RDY          0x1u
#define TRNG_CTL_EN       0x400u
#define TRNG_GEN_NS       1000000

static void trng_update_irq(cc13x2_soc_t *soc) {
    if (soc->trng.flags & soc->trng.mask) irq_raise(soc, CC13X2_IRQ_TRNG);
}

static void trng_ready_cb(void *user, cpu_event_t *ev) {
    (void)ev;
    cc13x2_soc_t *soc = user;
    cc13x2_trng_t *t = &soc->trng;
    t->generating = false;
    if (!t->enabled) return;
    if (!t->state)
        t->state = (((uint64_t)soc->mac_15_4[1] << 32) | soc->mac_15_4[0]) ^
                   0x9E3779B97F4A7C15ull;
    uint64_t x = t->state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    t->state = x;
    t->out[0] = (uint32_t)x;
    t->out[1] = (uint32_t)(x >> 32);
    t->flags |= TRNG_RDY;
    trng_update_irq(soc);
}

static void trng_kick(cc13x2_soc_t *soc) {
    cc13x2_trng_t *t = &soc->trng;
    if (!t->enabled || t->generating || (t->flags & TRNG_RDY)) return;
    t->generating = true;
    t->event.callback = trng_ready_cb;
    t->event.user_data = soc;
    schedule_at(soc, &t->event, live_ns(soc) + TRNG_GEN_NS);
}

static uint32_t trng_read(cc13x2_soc_t *soc, cc13x2_page_t *p, uint32_t off) {
    cc13x2_trng_t *t = &soc->trng;
    switch (off & ~3u) {
    case TRNG_OUT0:        return t->out[0];
    case TRNG_OUT1:        return t->out[1];
    case TRNG_IRQFLAGSTAT: return t->flags;
    case TRNG_IRQFLAGMASK: return t->mask;
    case TRNG_IRQSTATMASK:
    case TRNG_IRQSTAT:     return t->flags & t->mask;
    case TRNG_SWRESET:     return 0;                   /* reset completes at once */
    default:               return REG(p, off & ~3u);
    }
}

static void trng_write(cc13x2_soc_t *soc, cc13x2_page_t *p, uint32_t off, uint32_t val) {
    cc13x2_trng_t *t = &soc->trng;
    switch (off & ~3u) {
    case TRNG_IRQFLAGMASK: t->mask = val & 3u; break;
    case TRNG_IRQFLAGCLR:  t->flags &= ~val; break;
    case TRNG_IRQSET:      t->flags |= val & 3u; break;
    case TRNG_CTL:
        REG(p, TRNG_CTL) = val;
        t->enabled = (val & TRNG_CTL_EN) != 0;
        if (!t->enabled && t->generating) {
            arm_cancel_event(&soc->plat->cpu, &t->event);
            t->generating = false;
        }
        break;
    case TRNG_SWRESET:
        if (val & 1u) {
            if (t->generating) arm_cancel_event(&soc->plat->cpu, &t->event);
            uint64_t st = t->state;
            memset(t, 0, sizeof(*t));
            t->state = st;                             /* the stream continues */
            REG(p, TRNG_CTL) = 0;
        }
        break;
    default:
        REG(p, off & ~3u) = val;
        break;
    }
    trng_kick(soc);
    trng_update_irq(soc);
}

/* ============================================================
 * UART0 — inc/hw_uart.h (TI's PL011-derived UART)
 * ============================================================ */

#define UART_DR    0x00
#define UART_RSR   0x04
#define UART_FR    0x18
#define UART_IBRD  0x24
#define UART_FBRD  0x28
#define UART_LCRH  0x2C
#define UART_CTL   0x30
#define UART_IFLS  0x34
#define UART_IMSC  0x38
#define UART_RIS   0x3C
#define UART_MIS   0x40
#define UART_ICR   0x44
#define UART_DMACTL 0x48

#define UART_CTL_UARTEN 0x001u
#define UART_CTL_RXE    0x200u

#define UART_INT_EOT  0x800u
#define UART_INT_OE   0x400u
#define UART_INT_RT   0x040u
#define UART_INT_TX   0x020u
#define UART_INT_RX   0x010u
#define UART_FIFO     32

/* FIFO level thresholds, IFLS TXSEL [2:0] / RXSEL [5:3]: 1/8 .. 7/8. */
static int uart_level(uint32_t sel) {
    static const int lv[5] = { 4, 8, 16, 24, 28 };
    return sel < 5 ? lv[sel] : 16;
}

static void uart_update_irq(cc13x2_soc_t *soc) {
    if (soc->uart0.ris & soc->uart0.imsc)
        irq_raise(soc, CC13X2_IRQ_UART0);
}

/* Character time from the programmed divisor: UARTCLK (= the 48 MHz
 * peripheral clock) / (16 * (IBRD + FBRD/64)) baud, 10 bits per frame
 * for 8N1 (LCRH: WLEN [6:5], PEN, STP2 add bits). */
static int64_t uart_char_ns(cc13x2_uart_t *u) {
    /* The divisor in 1/64ths: IBRD.FBRD, 115200 at 48 MHz until it is set. */
    uint64_t div64 = (uint64_t)u->ibrd * 64u + u->fbrd;
    if (div64 < 64u) div64 = 26u * 64u + 3u;
    int bits = 1 + 5 + (int)((u->lcrh >> 5) & 3u) + ((u->lcrh & 2u) ? 1 : 0) +
               ((u->lcrh & 8u) ? 2 : 1);
    /* bits / baud = bits * 16 * (div64 / 64) / 48 MHz = bits * div64 / 192 MHz */
    return (int64_t)((uint64_t)bits * div64 * 125u / 24u);
}

static void uart_start_next(cc13x2_soc_t *soc);

static void uart_tx_done_cb(void *user, cpu_event_t *ev) {
    (void)ev;
    cc13x2_soc_t *soc = user;
    cc13x2_uart_t *u = &soc->uart0;
    u->tx_busy = false;
    u->tx_count--;
    int thr = uart_level(u->ifls & 7u);
    int queued = u->tx_count;
    if (queued <= thr) u->ris |= UART_INT_TX;
    if (queued == 0) u->ris |= UART_INT_EOT;
    uart_start_next(soc);
    uart_update_irq(soc);
}

static void uart_start_next(cc13x2_soc_t *soc) {
    cc13x2_uart_t *u = &soc->uart0;
    if (u->tx_busy || u->tx_count == 0) return;
    uint8_t b = u->tx_fifo[u->tx_head];
    u->tx_head = (u->tx_head + 1) % UART_FIFO;
    u->tx_busy = true;
    if (u->tx_cb) u->tx_cb(u->tx_user, b);
    u->tx_event.callback = uart_tx_done_cb;
    u->tx_event.user_data = soc;
    schedule_at(soc, &u->tx_event, live_ns(soc) + uart_char_ns(u));
}

static uint32_t uart_read(cc13x2_soc_t *soc, uint32_t off) {
    cc13x2_uart_t *u = &soc->uart0;
    uint32_t a = off & ~3u, v = 0;
    switch (a) {
    case UART_DR:
        if (u->rx_count > 0) {
            v = u->rx_fifo[u->rx_head];
            u->rx_head = (u->rx_head + 1) % UART_FIFO;
            u->rx_count--;
            if (u->rx_count < uart_level((u->ifls >> 3) & 7u)) u->ris &= ~UART_INT_RX;
            if (u->rx_count == 0) u->ris &= ~UART_INT_RT;
        }
        break;
    case UART_FR:
        if (u->tx_count == 0) v |= 0x80;               /* TXFE */
        if (u->rx_count == UART_FIFO) v |= 0x40;       /* RXFF */
        if (u->tx_count >= UART_FIFO + 1) v |= 0x20;   /* TXFF (FIFO + shifter) */
        if (u->rx_count == 0) v |= 0x10;               /* RXFE */
        if (u->tx_count > 0) v |= 0x08;                /* BUSY */
        break;
    case UART_IBRD:  v = u->ibrd; break;
    case UART_FBRD:  v = u->fbrd; break;
    case UART_LCRH:  v = u->lcrh; break;
    case UART_CTL:   v = u->ctl; break;
    case UART_IFLS:  v = u->ifls; break;
    case UART_IMSC:  v = u->imsc; break;
    case UART_RIS:   v = u->ris; break;
    case UART_MIS:   v = u->ris & u->imsc; break;
    case UART_DMACTL: v = u->dmactl; break;
    }
    return v >> ((off & 3u) * 8u);
}

static void uart_write(cc13x2_soc_t *soc, uint32_t off, uint32_t val) {
    cc13x2_uart_t *u = &soc->uart0;
    uint32_t a = off & ~3u;
    switch (a) {
    case UART_DR:
        if (u->tx_count < UART_FIFO + 1) {
            int slot = (u->tx_head + u->tx_count - (u->tx_busy ? 1 : 0)) % UART_FIFO;
            u->tx_fifo[slot] = (uint8_t)val;
            u->tx_count++;
            u->ris &= ~UART_INT_EOT;
            if (u->tx_count > uart_level(u->ifls & 7u)) u->ris &= ~UART_INT_TX;
            uart_start_next(soc);
        }
        break;
    case UART_RSR:   break;
    case UART_IBRD:  u->ibrd = val & 0xFFFFu; break;
    case UART_FBRD:  u->fbrd = val & 0x3Fu; break;
    case UART_LCRH:  u->lcrh = val & 0xFFu; break;
    case UART_CTL:   u->ctl = val & 0xFFFFu; break;
    case UART_IFLS:  u->ifls = val & 0x3Fu; break;
    case UART_IMSC:  u->imsc = val & 0xFF2u; break;
    case UART_ICR:   u->ris &= ~val; break;
    case UART_DMACTL: u->dmactl = val & 7u; break;
    }
    uart_update_irq(soc);
}

/* Console input arrives on the wire at the programmed baud rate, one
 * character time apart, so a line longer than the 32-byte RX FIFO reaches
 * firmware that keeps reading it.  A character that finds the FIFO full is
 * lost and sets the overrun flag, as on silicon.  So is one that arrives
 * while the receiver is off (CTL.UARTEN or CTL.RXE clear): it passes on the
 * wire unsampled, without touching the FIFO or the flags. */
static void uart_rx_pace_cb(void *user, cpu_event_t *ev) {
    (void)ev;
    cc13x2_soc_t *soc = user;
    cc13x2_uart_t *u = &soc->uart0;
    if (u->wire_count == 0) return;
    uint8_t b = u->wire[u->wire_head];
    u->wire_head = (u->wire_head + 1) % CC13X2_UART_WIRE;
    u->wire_count--;
    if ((u->ctl & (UART_CTL_UARTEN | UART_CTL_RXE)) !=
        (UART_CTL_UARTEN | UART_CTL_RXE)) {
        /* Receiver off: the character is lost. */
    } else if (u->rx_count < UART_FIFO) {
        u->rx_fifo[(u->rx_head + u->rx_count) % UART_FIFO] = b;
        u->rx_count++;
        if (u->rx_count >= uart_level((u->ifls >> 3) & 7u)) u->ris |= UART_INT_RX;
        u->ris |= UART_INT_RT;
    } else {
        u->ris |= UART_INT_OE;
    }
    uart_update_irq(soc);
    if (u->wire_count > 0)
        schedule_at(soc, &u->rx_event, live_ns(soc) + uart_char_ns(u));
}

int cc13x2_uart_feed_rx(cc13x2_soc_t *soc, const uint8_t *buf, int len) {
    cc13x2_uart_t *u = &soc->uart0;
    bool idle = u->wire_count == 0;
    int n = 0;
    for (; n < len && u->wire_count < CC13X2_UART_WIRE; n++) {
        u->wire[(u->wire_head + u->wire_count) % CC13X2_UART_WIRE] = buf[n];
        u->wire_count++;
    }
    if (idle && u->wire_count > 0) {
        u->rx_event.callback = uart_rx_pace_cb;
        u->rx_event.user_data = soc;
        schedule_at(soc, &u->rx_event, live_ns(soc) + uart_char_ns(u));
    }
    return n;
}

/* ============================================================
 * Watchdog — inc/hw_wdt.h, driverlib/watchdog.h
 * ============================================================ */

#define WDT_LOAD   0x000
#define WDT_VALUE  0x004
#define WDT_CTL    0x008
#define WDT_ICR    0x00C
#define WDT_RIS    0x010
#define WDT_MIS    0x014
#define WDT_LOCK   0xC00
#define WDT_CTL_INTEN   0x1u
#define WDT_CTL_RESEN   0x2u
#define WDT_UNLOCK_KEY  0x1ACCE551u
#define WDT_HZ          1500000

static void wdt_arm(cc13x2_soc_t *soc);

static uint32_t wdt_value(cc13x2_soc_t *soc) {
    cc13x2_wdt_t *w = &soc->wdt;
    if (!(w->ctl & WDT_CTL_INTEN)) return w->load;
    int64_t el = (w->paused ? w->paused_ns : live_ns(soc)) - w->anchor_ns;
    if (el < 0) el = 0;
    uint64_t ticks = ((uint64_t)el * WDT_HZ) / 1000000000u;
    return ticks >= w->load ? 0 : w->load - (uint32_t)ticks;
}

/* Expiry: the first one latches RIS and interrupts; one with RIS still set
 * resets the chip if RESEN (TRM, "Watchdog Timer"). The counter reloads
 * from LOAD either way. */
static void wdt_cb(void *user, cpu_event_t *ev) {
    (void)ev;
    cc13x2_soc_t *soc = user;
    cc13x2_wdt_t *w = &soc->wdt;
    if (w->ris && (w->ctl & WDT_CTL_RESEN)) {
        /* A watchdog reset is always worth knowing about in a simulation. */
        fprintf(stderr, "[cc13x2] watchdog reset at %.6f s (pc 0x%08x)\n",
                (double)live_ns(soc) / 1e9, soc->plat->cpu.reg[ARM_PC]);
        cc13x2_page_t *p = soc->pg_aon_pmctl;
        if (p) REG(p, AON_PMCTL_RESETCTL) = (REG(p, AON_PMCTL_RESETCTL) & ~0xEu) | 0xEu; /* WARMRESET */
        arm_request_reset(&soc->plat->cpu);
        return;
    }
    w->ris = true;
    w->anchor_ns = live_ns(soc);
    irq_raise(soc, CC13X2_IRQ_WDT);   /* CTL.INTTYPE NMI routing: not modelled */
    wdt_arm(soc);
}

static void wdt_arm(cc13x2_soc_t *soc) {
    cc13x2_wdt_t *w = &soc->wdt;
    arm_cancel_event(&soc->plat->cpu, &w->event);
    if (!(w->ctl & WDT_CTL_INTEN) || w->paused) return;
    w->event.callback = wdt_cb;
    w->event.user_data = soc;
    int64_t period = (int64_t)(((uint64_t)w->load * 1000000000u + WDT_HZ - 1) / WDT_HZ);
    schedule_at(soc, &w->event, w->anchor_ns + period);
}

/* Standby pauses the watchdog (TI WatchdogCC26XX: "the Watchdog Timer is
 * paused when the device enters standby"; Contiki's clock-arch.c relies on
 * it, capping only *idle* sleeps at the watchdog timeout). Standby is the
 * state SysCtrlStandby() enters by powering off the PERIPH (and SERIAL)
 * domains before deep sleep, which SysCtrlIdle() does not; the Power driver
 * powers them back on after the wakeup. */
static void wdt_set_standby(cc13x2_soc_t *soc, bool standby) {
    cc13x2_wdt_t *w = &soc->wdt;
    if (standby == w->paused) return;
    if (standby) {
        w->paused = true;
        w->paused_ns = live_ns(soc);
        arm_cancel_event(&soc->plat->cpu, &w->event);
    } else {
        w->paused = false;
        w->anchor_ns += live_ns(soc) - w->paused_ns;
        wdt_arm(soc);
    }
}

static uint32_t wdt_read(cc13x2_soc_t *soc, uint32_t off) {
    cc13x2_wdt_t *w = &soc->wdt;
    uint32_t v = 0;
    switch (off & ~3u) {
    case WDT_LOAD:  v = w->load; break;
    case WDT_VALUE: v = wdt_value(soc); break;
    case WDT_CTL:   v = w->ctl; break;
    case WDT_RIS:   v = w->ris ? 1u : 0u; break;
    case WDT_MIS:   v = (w->ris && (w->ctl & WDT_CTL_INTEN)) ? 1u : 0u; break;
    case WDT_LOCK:  v = w->locked ? 1u : 0u; break;
    }
    return v >> ((off & 3u) * 8u);
}

static void wdt_write(cc13x2_soc_t *soc, uint32_t off, uint32_t val) {
    cc13x2_wdt_t *w = &soc->wdt;
    uint32_t a = off & ~3u;
    if (a == WDT_LOCK) { w->locked = (val != WDT_UNLOCK_KEY); return; }
    if (w->locked) return;                     /* write-protected */
    switch (a) {
    case WDT_LOAD:                             /* writing LOAD reloads */
        w->load = val;
        w->anchor_ns = w->paused ? w->paused_ns : live_ns(soc);
        break;
    case WDT_CTL: {
        bool was = (w->ctl & WDT_CTL_INTEN) != 0;
        w->ctl = (val & 0x107u) | (w->ctl & WDT_CTL_INTEN);  /* INTEN sticks */
        if (!was && (w->ctl & WDT_CTL_INTEN)) w->anchor_ns = live_ns(soc);
        break;
    }
    case WDT_ICR:                              /* clears RIS and reloads */
        w->ris = false;
        w->anchor_ns = w->paused ? w->paused_ns : live_ns(soc);
        break;
    default:
        return;
    }
    wdt_arm(soc);
}

/* ============================================================
 * AUX TDC + AUX event controller — inc/hw_aux_tdc.h, hw_aux_evctl.h;
 * driven by kernel/.../power/PowerCC26X2_calibrateRCOSC.c
 * ============================================================ */

#define TDC_CTL          0x00
#define TDC_STAT         0x04
#define TDC_RESULT       0x08
#define TDC_TRIGCNTLOAD  0x18
#define TDC_CMD_CLR      0u
#define TDC_CMD_RUN_SYNC 1u
#define TDC_CMD_RUN      2u
#define TDC_CMD_ABORT    3u
#define TDC_STAT_DONE    0x40u
#define TDC_STAT_IDLE    0x06u

#define EVCTL_EVTOMCUFLAGS     0x30
#define EVCTL_EVTOMCUFLAGSCLR  0x38
#define EVCTL_COMBEVTOMCUMASK  0x3C
#define EVCTL_AUX_TDC_DONE     0x08u

static void aux_comb_update(cc13x2_soc_t *soc) {
    if (soc->aux_evflags & soc->aux_combmask)
        irq_raise(soc, CC13X2_IRQ_AUX_COMB);
}

/* The calibration measures TRIGCNTLOAD periods of ACLK_REF (DDI0 CTL0
 * ACLK_REF_SRC_SEL [6:4]: 0 = RCOSC_HF/1536 = 31.25 kHz, 2 = RCOSC_LF)
 * in 48 MHz units (both edges of XOSC_HF/2): an exact RCOSC_HF reads 1536
 * (RCOSC_HF_PERFECT_TDC_VALUE) and 32 RCOSC_LF periods 46875, which
 * updateSubSecInc turns into SUBSECINC 0x800000. csim's oscillators are
 * exact, so the calibration finds nothing to trim. */
static void tdc_done_cb(void *user, cpu_event_t *ev) {
    (void)ev;
    cc13x2_soc_t *soc = user;
    soc->tdc.running = false;
    soc->tdc.done = true;
    soc->aux_evflags |= EVCTL_AUX_TDC_DONE;
    aux_comb_update(soc);
}

static void tdc_run(cc13x2_soc_t *soc, bool sync_start) {
    uint32_t ref = (soc->ddi0[DDI0_CTL0 / 4] >> 4) & 7u;
    uint64_t f_ref = (ref == 2 || ref == 3) ? 32768u : 31250u;      /* Hz */
    uint64_t periods = soc->tdc.trigcntload ? soc->tdc.trigcntload : 1u;
    soc->tdc.result = (uint32_t)((periods * 48000000u + f_ref / 2u) / f_ref);
    soc->tdc.done = false;
    soc->tdc.running = true;
    /* A synchronised start waits for the next reference edge first. */
    uint64_t edges = periods + (sync_start ? 1u : 0u);
    soc->tdc.event.callback = tdc_done_cb;
    soc->tdc.event.user_data = soc;
    schedule_at(soc, &soc->tdc.event,
                live_ns(soc) + (int64_t)(edges * 1000000000u / f_ref));
}

static uint32_t tdc_read(cc13x2_soc_t *soc, cc13x2_page_t *p, uint32_t off) {
    uint32_t a = off & ~3u, v;
    switch (a) {
    case TDC_STAT:   v = (soc->tdc.done ? TDC_STAT_DONE : 0) |
                         (soc->tdc.running ? 0x08u : TDC_STAT_IDLE); break;
    case TDC_RESULT: v = soc->tdc.result; break;
    default:         v = REG(p, a); break;
    }
    return v >> ((off & 3u) * 8u);
}

static void tdc_write(cc13x2_soc_t *soc, cc13x2_page_t *p, uint32_t off, uint32_t val) {
    uint32_t a = off & ~3u;
    REG(p, a) = merge_write(soc, REG(p, a), off, val);
    if (a == TDC_TRIGCNTLOAD) soc->tdc.trigcntload = REG(p, a);
    if (a != TDC_CTL) return;
    switch (val & 3u) {
    case TDC_CMD_CLR:
        soc->tdc.result = 0;
        soc->tdc.done = false;
        break;
    case TDC_CMD_RUN_SYNC: tdc_run(soc, true); break;
    case TDC_CMD_RUN:      tdc_run(soc, false); break;
    case TDC_CMD_ABORT:
        arm_cancel_event(&soc->plat->cpu, &soc->tdc.event);
        soc->tdc.running = false;
        break;
    }
}

static uint32_t evctl_read(cc13x2_soc_t *soc, cc13x2_page_t *p, uint32_t off) {
    uint32_t a = off & ~3u, v;
    switch (a) {
    case EVCTL_EVTOMCUFLAGS:    v = soc->aux_evflags; break;
    case EVCTL_COMBEVTOMCUMASK: v = soc->aux_combmask; break;
    default:                    v = REG(p, a); break;
    }
    return v >> ((off & 3u) * 8u);
}

static void evctl_write(cc13x2_soc_t *soc, cc13x2_page_t *p, uint32_t off, uint32_t val) {
    uint32_t a = off & ~3u;
    switch (a) {
    case EVCTL_EVTOMCUFLAGSCLR: soc->aux_evflags &= ~merge_write(soc, 0, off, val); break;
    case EVCTL_COMBEVTOMCUMASK:
        soc->aux_combmask = merge_write(soc, soc->aux_combmask, off, val);
        aux_comb_update(soc);
        break;
    default: REG(p, a) = merge_write(soc, REG(p, a), off, val); break;
    }
}

/* ============================================================
 * Small blocks
 * ============================================================ */

/* VIMS STAT mirrors CTL.MODE; mode changes complete at once (STAT bit3
 * MODE_CHANGING never reads 1). inc/hw_vims.h. */
static uint32_t vims_read(cc13x2_page_t *p, uint32_t off) {
    uint32_t a = off & ~3u;
    if (a == 0x0) return (REG(p, 0x4) & 3u) >> ((off & 3u) * 8u);
    return REG(p, a) >> ((off & 3u) * 8u);
}

/* FLASH controller: identification and an always-idle state machine.
 * FLASH_SIZE.SECTORS = the part's 8 KB sectors (44 = 352 KB on the CC1352R,
 * 88 = 704 KB on the CC1312R7); FCFG_B0_SSIZE0 = that many sectors of 8 KB
 * (B0_SECT_SIZE in KB). inc/hw_flash.h. */
#define FLASH_STAT          0x001C
#define FLASH_FLASH_SIZE    0x002C
#define FLASH_FSM_STAT      0x2208
#define FLASH_FCFG_B0_SSIZE0 0x2430
static uint32_t flash_read(cc13x2_page_t *p, uint32_t off) {
    uint32_t a = off & ~3u, v;
    switch (a) {
    case FLASH_FLASH_SIZE:     v = p->soc->part->flash_size / CC13X2_FLASH_SECTOR; break;
    case FLASH_FCFG_B0_SSIZE0:
        v = ((p->soc->part->flash_size / CC13X2_FLASH_SECTOR) << 16) | 8u; break;
    case FLASH_STAT:           v = 0u; break;       /* not busy */
    case FLASH_FSM_STAT:       v = 0x4u; break;     /* INV_DAT idle */
    default:                   v = REG(p, a); break;
    }
    return v >> ((off & 3u) * 8u);
}

/* Semaphores (MCU SMPH and AUX_SMPH): reading SMPHn returns 1 and claims
 * it if it was free, 0 if taken; writing 1 releases. */
static uint32_t smph_read(cc13x2_page_t *p, uint32_t off, uint32_t nsem) {
    uint32_t a = off & ~3u, idx = a / 4;
    if (idx < nsem) {
        uint32_t free = (REG(p, a) == 0);
        REG(p, a) = 1;
        return free;
    }
    if (a >= 0x800 && (a - 0x800) / 4 < nsem)          /* PEEKn */
        return REG(p, a - 0x800) ? 0u : 1u;
    return REG(p, a);
}
static void smph_write(cc13x2_page_t *p, uint32_t off, uint32_t val, uint32_t nsem) {
    uint32_t a = off & ~3u;
    if (a / 4 < nsem) {
        if (val & 1u) REG(p, a) = 0;
        return;
    }
    REG(p, a) = val;
}

/* ============================================================
 * IO dispatch
 * ============================================================ */

/* Offset into the window. The non-buffered alias sits 0x2000_0000 above
 * the canonical base, which the mask folds away. */
static uint32_t page_off(cc13x2_page_t *p, uint32_t addr) {
    return (addr - p->base) & 0x0FFFFFFFu;
}

static void trace_access(cc13x2_page_t *p, uint32_t off, bool wr, uint32_t v) {
    if (!trace_on()) return;
    if (p->kind != P_STORE && cc13x2_trace < 2) return;
    if (p->kind == P_ROM || p->kind == P_GPRAM || p->kind == P_RFC_RAM) return;
    arm_cpu_t *cpu = &p->soc->plat->cpu;
    fprintf(stderr, "[cc13x2] %12.6f %c %-10s +0x%03x %s 0x%08x  pc=0x%08x\n",
            (double)live_ns(p->soc) / 1e9, wr ? 'W' : 'R', p->name, off,
            wr ? "<-" : "->", v, cpu->reg[ARM_PC]);
}

static int page_read_impl(void *user, uint32_t addr) {
    cc13x2_page_t *p = user;
    cc13x2_soc_t *soc = p->soc;
    uint32_t off = page_off(p, addr);
    uint32_t v;
    switch (p->kind) {
    case P_ROM:
        v = 0;
        for (int i = 0; i < 4; i++)
            if (off + (uint32_t)i < *soc->part->rom_size)
                v |= (uint32_t)soc->part->rom[off + (uint32_t)i] << (8 * i);
        return (int)v;
    case P_GPRAM:
        memcpy(&v, &soc->gpram[off & ~3u], 4);
        return (int)(v >> ((off & 3u) * 8u));
    case P_RFC_RAM:
        if (off >= CC13X2_RFC_RAM_SIZE) return 0;
        memcpy(&v, &soc->rfc_ram[off & ~3u], 4);
        return (int)(v >> ((off & 3u) * 8u));
    case P_FCFG1:
        return (int)(soc->fcfg1[(off & ~3u) / 4] >> ((off & 3u) * 8u));
    case P_CCFG: {
        /* CCFG is the last 8 KB flash sector seen through this window:
         * CCFG_O_* run 0x1FA8..0x1FFF (inc/hw_ccfg.h), the CCFG struct the
         * image links at 0x57FA8 (352 KB) or 0xAFFA8 (704 KB). */
        arm_cpu_t *cpu = &soc->plat->cpu;
        return (int)(arm_read32(cpu, (soc->part->flash_size - CC13X2_CCFG_SIZE) + (off & ~3u))
                     >> ((off & 3u) * 8u));
    }
    case P_PRCM:      return (int)prcm_read(soc, p, off);
    case P_AON_PMCTL: return (int)aon_pmctl_read(soc, p, off);
    case P_AON_RTC:   return (int)aon_rtc_read(soc, off);
    case P_GPIO:      return (int)gpio_read(soc, off);
    case P_UART0:     return (int)uart_read(soc, off);
    case P_TRNG:      return (int)(trng_read(soc, p, off) >> ((off & 3u) * 8u));
    case P_DDI0:      return (int)ddi0_read(soc, off);
    case P_ADI2:      return (int)adi_read(soc->adi2, off);
    case P_ADI3:      return (int)adi_read(soc->adi3, off);
    case P_ADI4:      return (int)adi_read(soc->adi4, off);
    case P_VIMS:      return (int)vims_read(p, off);
    case P_WDT:       return (int)wdt_read(soc, off);
    case P_RFC_PWR:   return (int)cc13x2_rfc_pwr_read(&soc->rfc, off);
    case P_RFC_DBELL: return (int)cc13x2_rfc_dbell_read(&soc->rfc, off);
    case P_RFC_RAT:   return (int)cc13x2_rfc_rat_read(&soc->rfc, off);
    case P_AUX_TDC:   return (int)tdc_read(soc, p, off);
    case P_AUX_EVCTL: return (int)evctl_read(soc, p, off);
    case P_FLASH:     return (int)flash_read(p, off);
    case P_SMPH:      return (int)smph_read(p, off, 32);
    case P_AUX_SMPH:  return (int)smph_read(p, off, 8);
    case P_AUX_SYSIF:
        /* Request/acknowledge pairs that complete at once (inc/hw_aux_sysif.h):
         * OPMODEACK (0x04) follows OPMODEREQ (0x00); RTCSUBSECINCCTL (0x84)
         * UPD_ACK [1] follows UPD_REQ [0] (driverlib/setup_rom.c
         * SetupSetAonRtcSubSecInc). The SUBSECINC value itself — the
         * RCOSC_LF trim of the RTC rate — is accepted and not applied:
         * csim's RTC counts at exactly 32768 Hz. */
        if ((off & ~3u) == 0x04) return (int)REG(p, 0x00);
        if ((off & ~3u) == 0x54 || (off & ~3u) == 0x58) {
            /* TDCCLKCTL / TDCREFCLKCTL: ACK [1] follows REQ [0]. */
            uint32_t r = REG(p, off & ~3u) & 1u;
            return (int)((r | (r << 1)) >> ((off & 3u) * 8u));
        }
        if ((off & ~3u) == 0x84) {
            uint32_t r = REG(p, 0x84) & 1u;
            return (int)((r | (r << 1)) >> ((off & 3u) * 8u));
        }
        break;
    case P_AON_IOC:
    case P_HAPI:
    case P_STORE:
        break;
    }
    return (int)(REG(p, off & ~3u) >> ((off & 3u) * 8u));
}

static int page_read(void *user, uint32_t addr) {
    cc13x2_page_t *p = user;
    int v = page_read_impl(user, addr);
    trace_access(p, page_off(p, addr), false, (uint32_t)v);
    return v;
}

static void page_write_impl(void *user, uint32_t addr, uint32_t val) {
    cc13x2_page_t *p = user;
    cc13x2_soc_t *soc = p->soc;
    uint32_t off = page_off(p, addr);
    uint32_t w = width_of(soc);
    switch (p->kind) {
    case P_ROM: case P_FCFG1: case P_CCFG:
        return;                                         /* read-only */
    case P_HAPI:      hapi_write(soc, off, val); return;
    case P_GPRAM:
        for (uint32_t i = 0; i < w && off + i < CC13X2_GPRAM_SIZE; i++)
            soc->gpram[off + i] = (uint8_t)(val >> (8 * i));
        return;
    case P_RFC_RAM:
        for (uint32_t i = 0; i < w && off + i < CC13X2_RFC_RAM_SIZE; i++)
            soc->rfc_ram[off + i] = (uint8_t)(val >> (8 * i));
        return;
    case P_PRCM:      prcm_write(soc, p, off, val); return;
    case P_AON_PMCTL: aon_pmctl_write(soc, p, off, val); return;
    case P_AON_RTC:   aon_rtc_write(soc, off, val); return;
    case P_GPIO:      gpio_write(soc, off, val); return;
    case P_UART0:     uart_write(soc, off, val); return;
    case P_TRNG:      trng_write(soc, p, off, val); return;
    case P_DDI0:      ddi0_write(soc, off, val); return;
    case P_ADI2:      adi_write(soc, soc->adi2, off, val); return;
    case P_ADI3:      adi_write(soc, soc->adi3, off, val); return;
    case P_ADI4:      adi_write(soc, soc->adi4, off, val); return;
    case P_WDT:       wdt_write(soc, off, val); return;
    case P_RFC_PWR:   cc13x2_rfc_pwr_write(&soc->rfc, off, val); return;
    case P_RFC_DBELL: cc13x2_rfc_dbell_write(&soc->rfc, off, val); return;
    case P_RFC_RAT:   return;
    case P_AUX_TDC:   tdc_write(soc, p, off, val); return;
    case P_AUX_EVCTL: evctl_write(soc, p, off, val); return;
    case P_SMPH:      smph_write(p, off, val, 32); return;
    case P_AUX_SMPH:  smph_write(p, off, val, 8); return;
    default: break;
    }
    uint32_t a = off & ~3u;
    REG(p, a) = merge_write(soc, REG(p, a), off, val);
}

static void page_write(void *user, uint32_t addr, uint32_t val) {
    cc13x2_page_t *p = user;
    trace_access(p, page_off(p, addr), true, val);
    page_write_impl(user, addr, val);
}

/* ============================================================
 * Lifecycle
 * ============================================================ */

/* Reset values the boot path depends on. */
/* power_on: the cold reset at power-up.  Any other reset (watchdog,
 * SYSRESETREQ) leaves the AON domain alone: the RTC keeps counting and
 * keeps its compare channels, as on silicon. */
static void soc_reset_state(cc13x2_soc_t *soc, bool power_on) {
    arm_cpu_t *cpu = &soc->plat->cpu;
    arm_cancel_event(cpu, &soc->osc.xosc_hf_event);
    arm_cancel_event(cpu, &soc->uart0.tx_event);
    arm_cancel_event(cpu, &soc->uart0.rx_event);
    for (int i = 0; i < 3; i++) arm_cancel_event(cpu, &soc->rtc.ch_event[i]);
    arm_cancel_event(cpu, &soc->wdt.event);
    arm_cancel_event(cpu, &soc->tdc.event);
    memset(&soc->tdc, 0, sizeof(soc->tdc));
    arm_cancel_event(cpu, &soc->trng.event);
    uint64_t trng_state = soc->trng.state;     /* a reset does not replay it */
    memset(&soc->trng, 0, sizeof(soc->trng));
    soc->trng.state = trng_state;
    soc->aux_evflags = 0;
    soc->aux_combmask = 0;
    cc13x2_rfc_reset(&soc->rfc);

    for (int i = 0; i < soc->num_pages; i++) {
        cc13x2_page_t *p = &soc->pages[i];
        if (p->regs && p->kind != P_AON_PMCTL &&
            (power_on || p->kind != P_AON_RTC))
            memset(p->regs, 0, p->size);
    }
    memset(soc->ddi0, 0, sizeof(soc->ddi0));
    memset(soc->adi2, 0, sizeof(soc->adi2));
    memset(soc->adi3, 0, sizeof(soc->adi3));
    memset(soc->adi4, 0, sizeof(soc->adi4));
    if (power_on) memset(&soc->rtc, 0, sizeof(soc->rtc));
    else rtc_rearm_all(soc);
    memset(&soc->osc, 0, sizeof(soc->osc));
    memset(&soc->wdt, 0, sizeof(soc->wdt));
    soc->wdt.load = 0xFFFFFFFFu;               /* reset value; locked = false */
    arm_uart_tx_callback cb = soc->uart0.tx_cb;
    void *cbu = soc->uart0.tx_user;
    memset(&soc->uart0, 0, sizeof(soc->uart0));
    soc->uart0.tx_cb = cb;
    soc->uart0.tx_user = cbu;
    uint32_t din_ext = soc->gpio.din_ext, din_forced = soc->gpio.din_forced;
    memset(&soc->gpio, 0, sizeof(soc->gpio));
    soc->gpio.din_ext = din_ext;
    soc->gpio.din_forced = din_forced;

    /* Boot ROM leaves the MCU domains on; the RF core is off. */
    soc->pd0 = 0x6u;          /* SERIAL | PERIPH */
    soc->pd1 = 0xAu;          /* CPU | VIMS */

    /* SetupTrimDevice() takes the cold-reset path when AON_IOC:IOCLATCH.EN
     * and AON_PMCTL:SLEEPCTL.IO_PAD_SLEEP_DIS are both 1 (driverlib/setup.c). */
    cc13x2_page_t *p;
    if ((p = find_page(soc, "AON_IOC"))) REG(p, 0x0C) = 1u;
    if ((p = soc->pg_aon_pmctl)) {
        REG(p, AON_PMCTL_SLEEPCTL) = 1u;
    }
    /* VIMS: cache mode, as the boot ROM leaves it. */
    if ((p = find_page(soc, "VIMS"))) REG(p, 0x4) = 1u;
    /* PRCM.RFCMODEHWOPT: the RF modes this part may select (chipinfo.c
     * reads bits [3:1] as the protocol vector: BLE, 15.4, proprietary;
     * RF_open() refuses RF_MODE_AUTO (0) unless bit 0 is set).
     * 0x2F as read from the CC1352R1 LaunchPad; see cc13x2_part_t. */
    if ((p = soc->pg_prcm)) REG(p, PRCM_RFCMODEHWOPT) = soc->part->rfcmodehwopt;
    /* SysTick, NVIC: arm_platform_reset_peripherals. */
}

static void cc13x2_soc_init(arm_platform_t *plat) {
    cc13x2_soc_t *soc = calloc(1, sizeof(*soc));
    plat->soc = soc;
    soc->plat = plat;
    soc->part = (plat->config && plat->config->soc_part)
                    ? plat->config->soc_part : &cc13x2_part_cc1352r;
    arm_cpu_t *cpu = &plat->cpu;

    soc->num_pages = NUM_PAGE_DESCS;
    soc->pages = calloc((size_t)soc->num_pages, sizeof(cc13x2_page_t));
    for (int i = 0; i < soc->num_pages; i++) {
        const page_desc_t *d = &page_descs[i];
        cc13x2_page_t *p = &soc->pages[i];
        p->soc = soc;
        p->base = d->base;
        p->size = d->size;
        p->name = d->name;
        p->kind = d->kind;
        if (d->kind != P_ROM && d->kind != P_GPRAM && d->kind != P_RFC_RAM &&
            d->kind != P_FCFG1 && d->kind != P_CCFG)
            p->regs = calloc(d->size / 4, sizeof(uint32_t));
        arm_register_io(cpu, d->base, d->size, page_read, page_write, p);
        if (d->nonbuf)
            arm_register_io(cpu, d->base | 0x20000000u, d->size,
                            page_read, page_write, p);
    }
    soc->pg_prcm = find_page(soc, "PRCM");
    soc->pg_aon_pmctl = find_page(soc, "AON_PMCTL");
    soc->pg_ioc = find_page(soc, "IOC");

    /* Per-node identity default: a distinct but fixed address until the
     * harness seeds one. */
    soc->mac_15_4[0] = 0x00000001u;
    soc->mac_15_4[1] = 0x00124B00u;
    fcfg1_init(soc);

    plat->host.cpu = cpu;
    cc13x2_rfc_init(&soc->rfc, soc);
    soc_reset_state(soc, true);
}

static void cc13x2_soc_reset(arm_platform_t *plat) {
    cc13x2_soc_t *soc = plat->soc;
    if (!soc) return;
    soc_reset_state(soc, false);
}

static void cc13x2_soc_destroy(arm_platform_t *plat) {
    cc13x2_soc_t *soc = plat->soc;
    if (!soc) return;
    for (int i = 0; i < soc->num_pages; i++) free(soc->pages[i].regs);
    free(soc->pages);
    free(soc);
    plat->soc = NULL;
}

static void cc13x2_soc_set_console(arm_platform_t *plat,
                                   arm_uart_tx_callback cb, void *user_data) {
    cc13x2_soc_t *soc = plat->soc;
    soc->uart0.tx_cb = cb;
    soc->uart0.tx_user = user_data;
}

const arm_soc_ops_t cc13x2_soc_ops = {
    .name        = "cc13x2",
    .init        = cc13x2_soc_init,
    .destroy     = cc13x2_soc_destroy,
    .reset       = cc13x2_soc_reset,
    .set_console = cc13x2_soc_set_console,
};
