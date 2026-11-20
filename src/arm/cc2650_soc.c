/*
 * TI CC2650 (CC26x0 family) SoC memory model — see include/arm/cc2650_soc.h.
 *
 * Step 6.1 skeleton: maps the SoC's memory windows (FCFG1, CCFG, GPRAM,
 * RFC RAM) and the peripheral region as plain register stores, and seeds
 * the factory config (FCFG1) from a real CC2650 dump with a per-node MAC
 * overlay.  Behavioural peripherals (UART0, GPIO, AON RTC, PRCM, WDT, the
 * RF core) are added in Step 6.3.
 *
 * Register offsets and bit positions are those of the SimpleLink core SDK
 * device headers (source/ti/devices/cc26x0_cc13x0/inc/hw_*.h), which follow
 * the CC13x0/CC26x0 Technical Reference Manual (SWCU117I).
 */
#include "cc2650_soc.h"
#include "arm_nvic.h"
#include "cpu_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The stand-in ROM image, built from the driverlib by
 * tools/ti-simplelink-rom/build-rom.sh (see src/arm/cc2650_rom_image.c). */
extern const size_t  cc2650_rom_image_size;
extern const uint8_t cc2650_rom_image[];

/* ============================================================
 * Page table
 * ============================================================ */

enum {
    P_STORE = 0,     /* plain read-back register store */
    P_FCFG1,
    P_CCFG,
    P_GPRAM,
    P_RFC_RAM,
    P_ROM,
    P_HAPI,
    P_DDI0,
    P_PRCM,
    P_AON_RTC,
    P_SMPH,
    P_AUX_SMPH,
};

typedef struct {
    uint32_t    base, size;
    const char *name;
    int         kind;
} page_desc_t;

/* inc/hw_memmap.h.  The peripheral region is one plain store for now;
 * individual peripheral windows (UART0, GPIO, AON RTC, PRCM, ...) are
 * split out in Step 6.3. */
static const page_desc_t page_descs[] = {
    { CC2650_ROM_BASE,     CC2650_ROM_SIZE,   "ROM",     P_ROM,     },
    { CC2650_HAPI_PORT,    0x1000,            "HAPI",    P_HAPI,    },
    { CC2650_GPRAM_BASE,   CC2650_GPRAM_SIZE, "GPRAM",   P_GPRAM,   },
    { CC2650_RFC_RAM_BASE, CC2650_RFC_RAM_SIZE, "RFC_RAM", P_RFC_RAM, },
    { CC2650_PERIPH_BASE,  CC2650_PERIPH_SIZE, "PERIPH",  P_STORE,   },
    /* Behavioural peripheral windows, layered over the PERIPH store
     * (smallest region wins on overlap). */
    { 0x40082000, 0x1000, "PRCM",    P_PRCM,    },
    { 0x40084000, 0x1000, "SMPH",    P_SMPH,    },
    { 0x40092000, 0x1000, "AON_RTC", P_AON_RTC, },
    { 0x400C8000, 0x1000, "AUX_SMPH", P_AUX_SMPH, },
    { 0x400CA000, 0x1000, "DDI0",    P_DDI0,    },
    { CC2650_FCFG1_BASE,   CC2650_FCFG1_SIZE, "FCFG1",   P_FCFG1,   },
    { CC2650_CCFG_BASE,    CC2650_CCFG_SIZE,  "CCFG",    P_CCFG,    },
};
#define NUM_PAGE_DESCS ((int)(sizeof(page_descs) / sizeof(page_descs[0])))

#define REG(page, off)  ((page)->regs[(off) >> 2])

/* Offset into the window. */
static uint32_t page_off(cc2650_page_t *p, uint32_t addr) {
    return (addr - p->base) & 0x0FFFFFFFu;
}

static inline uint32_t width_of(cc2650_soc_t *soc) {
    uint8_t w = soc->plat->cpu.io_width;
    return w ? w : 4;
}

/* Merge an access of the current width into a 32-bit register image. */
static uint32_t merge_write(cc2650_soc_t *soc, uint32_t old, uint32_t addr,
                            uint32_t val) {
    uint32_t w = width_of(soc);
    if (w >= 4) return val;
    uint32_t sh = (addr & 3u) * 8u;
    uint32_t mask = (w == 2 ? 0xFFFFu : 0xFFu) << sh;
    return (old & ~mask) | ((val << sh) & mask);
}

/* Live time in ns on the cycle clock. */
static inline int64_t live_ns(cc2650_soc_t *soc) {
    arm_cpu_t *cpu = &soc->plat->cpu;
    return arm_cycles_to_ns(cpu->cycles, cpu->cpu_freq_hz);
}

/* Schedule at an absolute time in the cycle clock's ns base (live_ns),
 * rounded up to the first cycle at or after it.  arm_schedule_event_ns
 * converts against cpu->sim_time_ns, which trails the cycle clock by the
 * boot pre-run and the slice in progress, so a deadline computed from
 * live_ns lands late by that gap. Same reasoning as cc13x2_schedule_at. */
static void schedule_at(cc2650_soc_t *soc, cpu_event_t *ev, int64_t fire_ns) {
    arm_cpu_t *cpu = &soc->plat->cpu;
    int64_t now = live_ns(soc);
    int64_t delta = fire_ns > now ? fire_ns - now : 0;
    arm_schedule_event(cpu, ev, cpu->cycles + cpu_ns_to_cycles_ceil(delta, cpu->cpu_freq_hz));
}

static void irq_raise(cc2650_soc_t *soc, int irq) {
    arm_nvic_set_pending(&soc->plat->nvic, irq);
}

static uint32_t soc_flash_size(const cc2650_soc_t *soc) {
    return soc->plat->cpu.flash_end - soc->plat->cpu.flash_base;
}

/* ============================================================
 * Factory configuration (FCFG1)
 * ============================================================ */

/* FCFG1 offsets used here (inc/hw_fcfg1.h). */
#define FCFG1_MAC_BLE_0          0x2E8
#define FCFG1_MAC_BLE_1          0x2EC
#define FCFG1_MAC_15_4_0         0x2F0
#define FCFG1_MAC_15_4_1         0x2F4

/* FCFG1 as read from a CC2650EM on a SmartRF06EB with the XDS110 and
 * openocd:
 *   dump_image fcfg1.bin 0x50001000 0x400
 * with the words that identify that one chip — MAC_BLE_0/1, MAC_15_4_0/1
 * and SHDW_DIE_ID_0..3 — set to the erased value (0xFFFFFFFF).
 * What the boot path depends on, as driverlib's chipinfo.c and TI's
 * prebuilt drivers decode it:
 *   USER_ID 0x8002F000: CC26 = 1 and PA = 0 (CC2650), PKG 2.
 *   ICEPICK_DEVICE_ID 0x00000003: chip family.
 *   IOCONF.GPIO_CNT = 31: the PIN driver sizes its pin range from it.
 * The trim words are this one chip's; SetupTrimDevice() copies them into
 * analog registers that csim stores and does not interpret.  The address
 * words (MAC_15_4, MAC_BLE) are filled in per node (fcfg1_init). */
static const uint32_t fcfg1_cc2650[256] = {
    /* 0x000 */ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    /* 0x018 */ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    /* 0x030 */ 0x0b2bacae, 0x0002ca80, 0xffffffff, 0xffffffff, 0x0b2bbca6, 0xb2a80000,
    /* 0x048 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x060 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x078 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x090 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x0a8 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x0c0 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x0d8 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x0f0 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x108 */ 0xffffffff, 0x00000000, 0x00000000, 0x00000000, 0xffffffff, 0xffffffff,
    /* 0x120 */ 0xffffffff, 0xffffffff, 0x00000001, 0x00000000, 0xc0800105, 0x2b4d0caa,
    /* 0x138 */ 0x127c0ee0, 0x02037383, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x150 */ 0xffffffff, 0xc729087e, 0xffffffff, 0xffffffff, 0xffffff00, 0x00108851,
    /* 0x168 */ 0x7f3a0025, 0x00380072, 0x17331a33, 0x0a0a2000, 0x026e0200, 0x0200f000,
    /* 0x180 */ 0x00000001, 0x00050014, 0x0fa00010, 0x00000fa0, 0x0a020804, 0x08080a01,
    /* 0x198 */ 0x080a0608, 0x8ee0a36a, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x1b0 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x0001ffff, 0x0001ffff,
    /* 0x1c8 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x1e0 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x1f8 */ 0xffffffff, 0xffffffff, 0x0000ffff, 0xffffffff, 0x2596ffff, 0xffffffff,
    /* 0x210 */ 0x0000ffff, 0xffffffff, 0x2522ffff, 0xffffffff, 0x0000ffff, 0xffffffff,
    /* 0x228 */ 0x2520ffff, 0xffffffff, 0x0000ffff, 0xffffffff, 0x2444ffff, 0xffffffff,
    /* 0x240 */ 0x0000ffff, 0xffffffff, 0x3555ffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x258 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x270 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    /* 0x288 */ 0xffffffff, 0xffffffff, 0xfffffffe, 0x8002f000, 0xffc500c5, 0xff000000,
    /* 0x2a0 */ 0xfff2c500, 0xffffffff, 0x0003e703, 0x2f2f2ff3, 0x00112003, 0xc64af47f,
    /* 0x2b8 */ 0xfaf8e0fb, 0xf3fdf0fc, 0xe8fff7ff, 0x9270c2af, 0xffffff2f, 0xffffffff,
    /* 0x2d0 */ 0xffffffff, 0xffffffff, 0xfffffffd, 0xfffffffd, 0xffffffff, 0xffffffff,
    /* 0x2e8 */ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x000013bc, 0x000013ff,
    /* 0x300 */ 0xffffffff, 0xfe07fe07, 0x98989f9f, 0xffffff33, 0x00000003, 0x01fe01fe,
    /* 0x318 */ 0x8b99a02f, 0x00000024, 0x0000c600, 0x8f90917f, 0x8b8c8d8e, 0x8788898a,
    /* 0x330 */ 0x83848586, 0x9e808182, 0x9a9b9c9d, 0x96979899, 0x92939495, 0xffffff1f,
    /* 0x348 */ 0xd0952c62, 0x3460f41e, 0xfc00ff64, 0x7b031feb, 0xffffff41, 0xffff81c0,
    /* 0x360 */ 0xffff83d8, 0xffff0000, 0xfffbfffd, 0xff00ff8b, 0xff7b828e, 0x6b8b0303,
    /* 0x378 */ 0xff183f47, 0xffffc3ff, 0xfffc014d, 0xff1e551e, 0xfffffffd, 0xf00f0000,
    /* 0x390 */ 0x00000000, 0xffffffff, 0xe00403f8, 0x080ba608, 0x0c10a50a, 0x1218a20d,
    /* 0x3a8 */ 0x1c259c14, 0x2e3b9021, 0x4c627a3b, 0x789e706b, 0xade1809a, 0x00000000,
    /* 0x3c0 */ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    /* 0x3d8 */ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    /* 0x3f0 */ 0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

static void fcfg1_init(cc2650_soc_t *soc) {
    memset(soc->fcfg1, 0, sizeof(soc->fcfg1));
    memcpy(soc->fcfg1, fcfg1_cc2650, sizeof(fcfg1_cc2650));
    uint32_t *f = soc->fcfg1;
    f[FCFG1_MAC_15_4_0 / 4]        = soc->mac_15_4[0];
    f[FCFG1_MAC_15_4_1 / 4]        = soc->mac_15_4[1];
    f[FCFG1_MAC_BLE_0 / 4]         = soc->mac_15_4[0];
    f[FCFG1_MAC_BLE_1 / 4]         = 0xFFFF0000u | (soc->mac_15_4[1] & 0xFFFFu);
}

void cc2650_soc_set_ieee_addr(cc2650_soc_t *soc, const uint8_t addr[8]) {
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
 *
 * CC26x0 offsets differ from CC13x2: STAT0 is at 0x34 (not 0x3C) and
 * there are no CLK_DCDC_RDY bits (the CC26x0 has no DCDC).  There is no
 * PRCM oscillator interrupt on CC26x0 (no OSCIMSC/OSCRIS/OSCICR), so the
 * HF switch is committed by the firmware polling STAT0.PENDINGSCLKHFSWITCHING
 * and then calling the ROM's HFSourceSafeSwitch (HAPI port below).
 * ============================================================ */

#define DDI0_CTL0     0x00
#define DDI0_STAT0    0x34
#define DDI0_STAT1    0x38
#define DDI0_STAT2    0x3C

#define CTL0_SCLK_HF_SRC_SEL   0x00000001u
#define CTL0_SCLK_LF_SRC_SEL_S 2

/* Value from cc13x2_soc.c:
 * XOSC_HF start-up: the TRM gives ~200-500 us for the 48 MHz crystal path;
 * TI's Power driver allows for it by polling. 300 us is a mid value.
 * TODO: estimated timing — see plan "Further considerations" */
#define XOSC_HF_STARTUP_NS     300000

static bool osc_switch_pending(cc2650_soc_t *soc);

static void osc_xosc_ready_cb(void *user, cpu_event_t *ev) {
    (void)ev;
    cc2650_soc_t *soc = user;
    soc->osc.xosc_hf_starting = false;
    soc->osc.xosc_hf_ready = true;
}

/* A write to CTL0 that selects XOSC_HF as the SCLK_HF source starts the
 * crystal; the switch itself is committed by the ROM's HFSourceSafeSwitch
 * (HAPI port below) once STAT0.PENDINGSCLKHFSWITCHING reports it ready. */
static void osc_ctl0_changed(cc2650_soc_t *soc) {
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
    if (lf != soc->osc.lf_src)
        soc->osc.lf_src = lf;
}

static bool osc_switch_pending(cc2650_soc_t *soc) {
    int sel = (soc->ddi0[DDI0_CTL0 / 4] & CTL0_SCLK_HF_SRC_SEL) ? 1 : 0;
    if (sel == soc->osc.hf_src) return false;
    return sel == 0 || soc->osc.xosc_hf_ready;
}

static uint32_t osc_status(cc2650_soc_t *soc, uint32_t off) {
    switch (off) {
    case DDI0_STAT0: {
        uint32_t v = 0;
        v |= (uint32_t)soc->osc.lf_src << 29;           /* SCLK_LF_SRC */
        v |= (uint32_t)soc->osc.hf_src << 28;           /* SCLK_HF_SRC */
        v |= 1u << 22;                                  /* RCOSC_HF_EN */
        if (soc->osc.lf_src == 2) v |= 1u << 21;        /* RCOSC_LF_EN */
        if (soc->osc.lf_src == 3) v |= 1u << 20;        /* XOSC_LF_EN */
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
static void hapi_write(cc2650_soc_t *soc, uint32_t off, uint32_t val) {
    if (off == 0 && val == 1) {
        if (osc_switch_pending(soc))
            soc->osc.hf_src = (soc->ddi0[DDI0_CTL0 / 4] & CTL0_SCLK_HF_SRC_SEL) ? 1 : 0;
        return;
    }
    if (off == 4) {
        /* tools/ti-simplelink-rom/hapi.c rom_unimplemented: val is its caller. */
        fprintf(stderr, "[cc2650] call into a ROM slot the stand-in ROM does "
                "not implement, from 0x%08x; the CPU is parked in a bkpt loop\n",
                val & ~1u);
        return;
    }
    fprintf(stderr, "[cc2650] HAPI port: unknown request 0x%x at +0x%x\n", val, off);
}

/* DDI: 32-bit registers.  DIR 0x000, SET 0x080, CLR 0x100, and masked
 * writes MASK4B 0x200 (byte per nibble), MASK8B 0x300 (halfword per byte),
 * MASK16B 0x400 (word per halfword): each n-bit unit of the register file
 * maps to a 2n-bit write carrying {mask, data} (inc/hw_ddi.h,
 * driverlib/ddi.c DDI16BitWrite / DDI16BitfieldWrite). */
static void ddi0_write(cc2650_soc_t *soc, uint32_t off, uint32_t val) {
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

static uint32_t ddi0_read(cc2650_soc_t *soc, uint32_t off) {
    if (off >= 0x080) return 0;          /* the write-only windows read 0 */
    uint32_t aligned = off & ~3u;
    if (aligned >= sizeof(soc->ddi0)) return 0;
    return osc_status(soc, aligned) >> ((off & 3u) * 8u);
}

/* ============================================================
 * PRCM — inc/hw_prcm.h
 *
 * CC26x0 has no oscillator interrupt registers (no OSCIMSC/OSCRIS/OSCICR,
 * no INT_OSC_COMB); the last register is RAMRETEN at 0x224.  Power domains
 * switch in a few us on silicon; the driverlib callers poll PDSTAT, so
 * reporting the new state at once is indistinguishable.
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
 * PDCTL1: CPU_ON bit1, RFC_ON bit2, VIMS_MODE bit3. */
static bool rfc_powered(cc2650_soc_t *soc) {
    return (soc->pd0 & 1u) || (soc->pd1 & 4u);
}

static uint32_t prcm_read(cc2650_soc_t *soc, cc2650_page_t *p, uint32_t off) {
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

static void prcm_write(cc2650_soc_t *soc, cc2650_page_t *p, uint32_t off, uint32_t val) {
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
    }
    REG(p, a) = merge_write(soc, REG(p, a), off, val);
}

/* ============================================================
 * AON RTC — inc/hw_aon_rtc.h, driverlib/aon_rtc.c
 *
 * The system tick on CC26x0: TimerP/ClockP run on this, not GPT0.  The
 * 64-bit counter (SEC:SUBSEC) advances 2^17 (1/32768 s) per SCLK_LF edge;
 * it is computed from the live clock, not stored.  Register layout is
 * identical to CC13x2 (same AON RTC IP block).
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
static uint64_t rtc_ticks(cc2650_soc_t *soc) {
    if (!soc->rtc.enabled) return 0;
    int64_t el = live_ns(soc) - soc->rtc.zero_ns;
    if (el < 0) el = 0;
    return ((uint64_t)el * 4096u) / 125000000u;        /* el * 32768 / 1e9 */
}

static int64_t rtc_tick_ns(cc2650_soc_t *soc, uint64_t tick) {
    return soc->rtc.zero_ns + (int64_t)((tick * 125000000u + 4095u) / 4096u);
}

/* The 16.16 view the compare channels and rtimer use (counter bits 47:16). */
static uint32_t rtc_time1616(uint64_t ticks) {
    return (uint32_t)(ticks << 1);
}

static void rtc_update_irq(cc2650_soc_t *soc) {
    uint32_t comb = (soc->rtc.ctl >> RTC_CTL_COMB_S) & 7u;
    uint32_t hit = 0;
    for (int i = 0; i < 3; i++)
        if ((comb & (1u << i)) && (soc->rtc.evflags & rtc_ch_bit[i])) hit = 1;
    if (hit) irq_raise(soc, CC2650_IRQ_AON_RTC_COMB);
}

static void rtc_arm_channel(cc2650_soc_t *soc, int ch, bool armed_now);

static void rtc_ch_cb(void *user, cpu_event_t *ev) {
    cc2650_soc_t *soc = user;
    int ch = (int)(ev - soc->rtc.ch_event);
    if (!(soc->rtc.chctl & rtc_ch_bit[ch])) return;
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
 * TODO: estimated timing — see plan "Further considerations" */
static void rtc_arm_channel(cc2650_soc_t *soc, int ch, bool armed_now) {
    arm_cpu_t *cpu = &soc->plat->cpu;
    cpu_event_t *ev = &soc->rtc.ch_event[ch];
    arm_cancel_event(cpu, ev);
    if (!soc->rtc.enabled || !(soc->rtc.chctl & rtc_ch_bit[ch])) return;
    uint64_t now = rtc_ticks(soc);
    uint32_t t1616 = rtc_time1616(now);
    uint32_t diff = soc->rtc.cmp[ch] - t1616;
    uint64_t target;
    if (diff != 0 && diff <= 0x80000000u) {
        target = now + (diff + 1u) / 2u + 1u;             /* reach, + 1 edge */
    } else if (armed_now && (uint32_t)(t1616 - soc->rtc.cmp[ch]) <= 0x10000u) {
        target = now + 1u;                                 /* as soon as possible */
    } else {
        return;
    }
    ev->callback = rtc_ch_cb;
    ev->user_data = soc;
    schedule_at(soc, ev, rtc_tick_ns(soc, target));
}

static void rtc_rearm_all(cc2650_soc_t *soc) {
    for (int i = 0; i < 3; i++) rtc_arm_channel(soc, i, false);
}

static uint32_t aon_rtc_read(cc2650_soc_t *soc, uint32_t off) {
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

static void aon_rtc_write(cc2650_soc_t *soc, uint32_t off, uint32_t val) {
    uint32_t a = off & ~3u;
    switch (a) {
    case RTC_CTL: {
        uint32_t v = merge_write(soc, soc->rtc.ctl, off, val);
        bool reset = (v & RTC_CTL_RESET) != 0;
        if (reset) {
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
        if (timebase) rtc_rearm_all(soc);
        rtc_update_irq(soc);
        return;
    }
    case RTC_EVFLAGS:
        soc->rtc.evflags &= ~merge_write(soc, 0, off, val);
        return;
    case RTC_CHCTL: {
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
    case RTC_CH2CMPINC: soc->rtc.ch2cmpinc = merge_write(soc, soc->rtc.ch2cmpinc, off, val); return;
    case RTC_CH1CAPT:   soc->rtc.ch1capt = merge_write(soc, soc->rtc.ch1capt, off, val); return;
    }
}

/* ============================================================
 * Semaphores (MCU SMPH and AUX_SMPH) — inc/hw_smph.h, inc/hw_aux_smph.h
 *
 * Reading SMPHn returns 1 and claims it if it was free, 0 if taken;
 * writing 1 releases. PEEKn (0x800+) reads the state without claiming.
 * The CC26x0 block is the same IP as CC13x2, so the semantics match.
 * ============================================================ */
static uint32_t smph_read(cc2650_page_t *p, uint32_t off, uint32_t nsem) {
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
static void smph_write(cc2650_page_t *p, uint32_t off, uint32_t val, uint32_t nsem) {
    uint32_t a = off & ~3u;
    if (a / 4 < nsem) {
        if (val & 1u) REG(p, a) = 0;
        return;
    }
    /* AUX_SMPH AUTOTAKE (0x20) and other non-semaphore offsets: plain store.
     * TODO: model the sticky autotake request + AUX_SMPH_AUTOTAKE_DONE event. */
    REG(p, a) = val;
}

/* ============================================================
 * IO dispatch
 * ============================================================ */

static int page_read_impl(void *user, uint32_t addr) {
    cc2650_page_t *p = user;
    cc2650_soc_t *soc = p->soc;
    uint32_t off = page_off(p, addr);
    uint32_t v;
    switch (p->kind) {
    case P_ROM:
        /* The stand-in ROM image, little-endian; bytes past the end read 0. */
        v = 0;
        for (int i = 0; i < 4; i++)
            if (off + (uint32_t)i < cc2650_rom_image_size)
                v |= (uint32_t)cc2650_rom_image[off + (uint32_t)i] << (8 * i);
        return (int)v;
    case P_GPRAM:
        memcpy(&v, &soc->gpram[off & ~3u], 4);
        return (int)(v >> ((off & 3u) * 8u));
    case P_RFC_RAM:
        if (off >= CC2650_RFC_RAM_SIZE) return 0;
        memcpy(&v, &soc->rfc_ram[off & ~3u], 4);
        return (int)(v >> ((off & 3u) * 8u));
    case P_FCFG1:
        return (int)(soc->fcfg1[(off & ~3u) / 4] >> ((off & 3u) * 8u));
    case P_CCFG: {
        /* CCFG is the last flash sector seen through this window:
         * CCFG_O_* run 0x1FA8..0x1FFF (inc/hw_ccfg.h), the CCFG struct the
         * image links at 0x1FFA8 (128 KB). */
        arm_cpu_t *cpu = &soc->plat->cpu;
        return (int)(arm_read32(cpu, (soc_flash_size(soc) - CC2650_CCFG_SIZE) + (off & ~3u))
                     >> ((off & 3u) * 8u));
    }
    case P_DDI0:
        return (int)ddi0_read(soc, off);
    case P_PRCM:
        return (int)prcm_read(soc, p, off);
    case P_AON_RTC:
        return (int)aon_rtc_read(soc, off);
    case P_SMPH:
        return (int)smph_read(p, off, 32);
    case P_AUX_SMPH:
        return (int)smph_read(p, off, 8);
    case P_HAPI:
    case P_STORE:
    default:
        break;
    }
    return (int)(REG(p, off & ~3u) >> ((off & 3u) * 8u));
}

static int page_read(void *user, uint32_t addr) {
    return page_read_impl(user, addr);
}

static void page_write_impl(void *user, uint32_t addr, uint32_t val) {
    cc2650_page_t *p = user;
    cc2650_soc_t *soc = p->soc;
    uint32_t off = page_off(p, addr);
    uint32_t w = width_of(soc);
    switch (p->kind) {
    case P_ROM: case P_FCFG1: case P_CCFG:
        return;                                         /* read-only */
    case P_DDI0:      ddi0_write(soc, off, val); return;
    case P_PRCM:      prcm_write(soc, p, off, val); return;
    case P_AON_RTC:   aon_rtc_write(soc, off, val); return;
    case P_SMPH:      smph_write(p, off, val, 32); return;
    case P_AUX_SMPH:  smph_write(p, off, val, 8);  return;
    case P_HAPI:      hapi_write(soc, off, val); return;
    case P_GPRAM:
        for (uint32_t i = 0; i < w && off + i < CC2650_GPRAM_SIZE; i++)
            soc->gpram[off + i] = (uint8_t)(val >> (8 * i));
        return;
    case P_RFC_RAM:
        for (uint32_t i = 0; i < w && off + i < CC2650_RFC_RAM_SIZE; i++)
            soc->rfc_ram[off + i] = (uint8_t)(val >> (8 * i));
        return;
    case P_STORE:
    default:
        break;
    }
    uint32_t a = off & ~3u;
    REG(p, a) = merge_write(soc, REG(p, a), off, val);
}

static void page_write(void *user, uint32_t addr, uint32_t val) {
    page_write_impl(user, addr, val);
}

/* ============================================================
 * Lifecycle
 * ============================================================ */

static void soc_reset_state(cc2650_soc_t *soc) {
    for (int i = 0; i < soc->num_pages; i++) {
        cc2650_page_t *p = &soc->pages[i];
        if (p->regs)
            memset(p->regs, 0, p->size);
    }
    memset(soc->gpram, 0, sizeof(soc->gpram));
    memset(soc->rfc_ram, 0, sizeof(soc->rfc_ram));
    soc->pd0 = 0;
    soc->pd1 = 0;
    soc->osc.hf_src = 0;
    soc->osc.lf_src = 0;
    soc->osc.xosc_hf_ready = false;
    soc->osc.xosc_hf_starting = false;
    soc->rtc.enabled = false;
    soc->rtc.zero_ns = 0;
    soc->rtc.ctl = 0;
    soc->rtc.evflags = 0;
    soc->rtc.chctl = 0;
    soc->rtc.cmp[0] = soc->rtc.cmp[1] = soc->rtc.cmp[2] = 0;
    soc->rtc.ch2cmpinc = 0;
    soc->rtc.ch1capt = 0;
    soc->rtc.subsec_latch = 0;
}

static void cc2650_soc_init(arm_platform_t *plat) {
    cc2650_soc_t *soc = calloc(1, sizeof(*soc));
    plat->soc = soc;
    soc->plat = plat;
    arm_cpu_t *cpu = &plat->cpu;

    soc->num_pages = NUM_PAGE_DESCS;
    soc->pages = calloc((size_t)soc->num_pages, sizeof(cc2650_page_t));
    for (int i = 0; i < soc->num_pages; i++) {
        const page_desc_t *d = &page_descs[i];
        cc2650_page_t *p = &soc->pages[i];
        p->soc = soc;
        p->base = d->base;
        p->size = d->size;
        p->name = d->name;
        p->kind = d->kind;
        if (d->kind != P_GPRAM && d->kind != P_RFC_RAM &&
            d->kind != P_FCFG1 && d->kind != P_CCFG && d->kind != P_ROM)
            p->regs = calloc(d->size / 4, sizeof(uint32_t));
        arm_register_io(cpu, d->base, d->size, page_read, page_write, p);
    }

    /* Per-node identity default: a distinct but fixed address until the
     * harness seeds one. */
    soc->mac_15_4[0] = 0x00000001u;
    soc->mac_15_4[1] = 0x00124B00u;
    fcfg1_init(soc);

    plat->host.cpu = cpu;
    soc_reset_state(soc);
}

static void cc2650_soc_reset(arm_platform_t *plat) {
    cc2650_soc_t *soc = plat->soc;
    if (!soc) return;
    soc_reset_state(soc);
}

static void cc2650_soc_destroy(arm_platform_t *plat) {
    cc2650_soc_t *soc = plat->soc;
    if (!soc) return;
    for (int i = 0; i < soc->num_pages; i++) free(soc->pages[i].regs);
    free(soc->pages);
    free(soc);
    plat->soc = NULL;
}

static void cc2650_soc_set_console(arm_platform_t *plat,
                                   arm_uart_tx_callback cb, void *user_data) {
    (void)plat; (void)cb; (void)user_data;
    /* UART0 lands in Step 6.3. */
}

const arm_soc_ops_t cc2650_soc_ops = {
    .name        = "cc2650",
    .init        = cc2650_soc_init,
    .destroy     = cc2650_soc_destroy,
    .reset       = cc2650_soc_reset,
    .set_console = cc2650_soc_set_console,
};
