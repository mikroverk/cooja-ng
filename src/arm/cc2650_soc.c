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
 * HAPI port
 * ============================================================ */

/* The stand-in ROM's HAPI table (tools/ti-simplelink-rom/hapi.c) reaches the
 * emulator through this csim-only port.  HFSourceSafeSwitch (offset 0, value
 * 1) commits a pending SCLK_HF source switch; on silicon the ROM does it at a
 * flash-idle moment.  The oscillator model that reports the pending switch
 * lands in Step 6.3, so for now the commit is a no-op.  Offset 4 is
 * rom_unimplemented(): the value is the caller's LR, reported so a call into
 * a ROM slot the stand-in does not supply is visible. */
static void hapi_write(cc2650_soc_t *soc, uint32_t off, uint32_t val) {
    (void)soc;
    if (off == 0 && val == 1) {
        /* HFSourceSafeSwitch: commit pending SCLK_HF switch (Step 6.3). */
        return;
    }
    if (off == 4) {
        fprintf(stderr, "[cc2650] call into a ROM slot the stand-in ROM does "
                "not implement, from 0x%08x; the CPU is parked in a bkpt loop\n",
                val & ~1u);
        return;
    }
    fprintf(stderr, "[cc2650] HAPI port: unknown request 0x%x at +0x%x\n", val, off);
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
