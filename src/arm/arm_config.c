/*
 * ARM SoC configuration definitions
 */
#include "arm_config.h"

const arm_config_t cc2538_config = {
    .name            = "CC2538",
    .rom_size        = 128 * 1024,
    .flash_size      = 512 * 1024,
    .flash_base      = 0x00200000,
    .sram_size       = 32 * 1024,
    .sram_base       = 0x20000000,
    .default_cpu_freq = 32000000,   /* 32 MHz system clock */
    .num_irqs        = 179,
};

/* Nordic nRF52840.
 *
 * Per nRF52840 Product Specification v1.7 (Nordic doc 4397_734 v1.7):
 *   - Cortex-M4F @ 64 MHz (HFXO 32 MHz × 2 PLL).
 *   - 1 MiB flash @ 0x00000000.
 *   - 256 KiB SRAM @ 0x20000000.
 *   - 48 IRQ lines in the vector table (NVIC ISER[0] + ISER[1] cover IDs 0–47).
 *
 * No `rom_size` — the chip's "ROM" (FICR/UICR factory data) lives in the
 * flash region's high pages and is read through the regular memory map,
 * not a separate ROM window.
 *
 * The current ARM interpreter is Cortex-M3. The Thumb-2 ISA M4 uses is a
 * superset; networking firmware (Contiki RPL/UDP/TSCH) does not exercise
 * the FPU or DSP/SIMD extensions, so the M3 interpreter is expected to
 * cover the common path. M4-only opcodes are added on demand once
 * firmware traps on them.
 */
/* Nordic nRF54L15 (Cortex-M33).
 *
 * Per nRF54L15 Product Specification (Nordic) and the Contiki-NG
 * port at `arch/cpu/nrf/nrf54l15/`:
 *   - Cortex-M33 @ 128 MHz (HFXO 32 MHz × 4 PLL).
 *   - 1.5 MiB flash @ 0x00000000.
 *   - 192 KiB SRAM @ 0x20000000 (256 KiB on some variants — confirm
 *     against PS / linker script per board).
 *   - Peripheral region at 0x50000000 (NOT 0x40000000 — different
 *     from nRF52). NVIC + system peripherals in 0xE0000000 common
 *     Cortex-M space.
 *   - ~50 IRQs in the application-core vector table (count TBD;
 *     confirm against nrfx mdk header nrf54l15_application.h).
 *
 * No `rom_size` — like nRF52840, the chip's factory data (FICR/UICR)
 * lives in flash/info pages and is read through the regular memory
 * map.
 *
 * ARMv8-M Mainline ISA. The existing arm_cpu.c Thumb-2 interpreter
 * (and arm_vfp.c FPv4-SP-D16) covers the common path; ARMv8-M-only
 * additions (MSPLIM/PSPLIM, BTI, TrustZone-M) are added on demand
 * when firmware traps. Contiki-NG's nrf54l15 build is soft-float
 * ABI and non-secure-only by default, so the L0–L6 path is expected
 * to stay within the M3/M4-compatible subset.
 */
const arm_config_t nrf54l15_config = {
    .name             = "nRF54L15",
    .rom_size         = 0,
    .flash_size       = 1536 * 1024,
    .flash_base       = 0x00000000,
    /* 256 KB RAM (0x20000000..0x20040000). Must span the FLPR partition:
     * the RV32E coprocessor's exec memory (0x20028000), data/stack
     * (0x20030000..0x2003f000) and the M33<->FLPR shared counter
     * (0x2003f000) all live in the top 96 KB. See riscv-vpr-plan.md. */
    .sram_size        = 256 * 1024,
    .sram_base        = 0x20000000,
    .default_cpu_freq = 128000000,    /* 128 MHz */
    .num_irqs         = 64,           /* upper bound; tighten once PS is in hand */
    .vtor_default     = 0,            /* vector table at flash base */
    .has_trustzone    = true,         /* ARMv8-M security extension (M33 + SPU) */
    .periph_ns_alias  = true,         /* 0x4xxx_xxxx = Non-secure alias of 0x5xxx_xxxx */
    .cpuid            = 0x410FD214,   /* Cortex-M33 r0p4 */
};

/* TI CC13x2 / CC26x2 (CC1352R1 LaunchPad).
 *
 * Per the CC13x2/CC26x2 TRM (SWCU185) and driverlib inc/hw_memmap.h:
 *   - Cortex-M4F @ 48 MHz (SCLK_HF, from the 48 MHz XOSC_HF or RCOSC_HF).
 *   - 352 KB flash @ 0x00000000 (44 sectors of 8 KB; CCFG in the last one).
 *   - 80 KB SRAM @ 0x20000000.
 *   - 38 external interrupts (inc/hw_ints.h NUM_INTERRUPTS = 54).
 *
 * The 256 KB ROM sits at 0x10000000, not at 0, so `rom_size` stays 0: the
 * SoC maps its stand-in ROM as an IO window (cc13x2_soc.c), so every
 * halfword fetched from it goes through the IO lookup and the page's read
 * handler. ROM routines are cold paths (power and clock setup), so they do
 * not need the core's direct-mapped fetch; a firmware that idled in a ROM
 * routine would.
 */
const arm_config_t cc13x2_config = {
    .name             = "CC13x2",
    .rom_size         = 0,
    .flash_size       = 352 * 1024,
    .flash_base       = 0x00000000,
    .sram_size        = 80 * 1024,
    .sram_base        = 0x20000000,
    .default_cpu_freq = 48000000,
    .num_irqs         = 38,
    .vtor_default     = 0,           /* vector table at flash base */
    .cpuid            = 0x410FC241,  /* Cortex-M4 r0p1 */
};

/* TI CC13x2x7 / CC26x2x7 (CC1312R7 LaunchPad): the CC13x2/CC26x2 core and
 * peripherals with 704 KB flash (88 sectors of 8 KB; CCFG in the last one)
 * and 144 KB SRAM at the same addresses (the CC1312R7 datasheet).  Same
 * interrupt map and ROM placement as cc13x2_config. */
const arm_config_t cc13x2x7_config = {
    .name             = "CC13x2x7",
    .rom_size         = 0,
    .flash_size       = 704 * 1024,
    .flash_base       = 0x00000000,
    .sram_size        = 144 * 1024,
    .sram_base        = 0x20000000,
    .default_cpu_freq = 48000000,
    .num_irqs         = 38,
    .vtor_default     = 0,
    .cpuid            = 0x410FC241,  /* Cortex-M4 r0p1 */
};

const arm_config_t nrf52840_config = {
    .name            = "nRF52840",
    .rom_size        = 0,
    .flash_size      = 1024 * 1024,
    .flash_base      = 0x00000000,
    .sram_size       = 256 * 1024,
    .sram_base       = 0x20000000,
    .default_cpu_freq = 64000000,   /* 64 MHz HFCLK */
    .num_irqs        = 48,
    /* VTOR is board-specific (DK = 0x0, Dongle = 0x1000 because of
     * the Open Bootloader). Set per-platform via
     * `arm_platform_config_t::vtor_override`. */
    .vtor_default    = 0,
};
