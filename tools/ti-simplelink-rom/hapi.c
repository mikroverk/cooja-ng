/*
 * HAPI ("hard API") table of csim's TI SimpleLink stand-in ROM (CC13x2/CC26x2
 * and CC26x0 families).
 *
 * driverlib/rom.h declares the table at 0x10000048 (HARD_API_T) and calls
 * through it; the functions themselves are not part of driverlib, so they
 * are written here from their documented contracts (rom.h, the family TRM's
 * ROM chapters).  Entries rom.h marks reserved, and the analog-mux
 * selectors no csim firmware uses, trap (rom_unimplemented).
 *
 * HFSourceSafeSwitch is the one entry that cannot be written as plain
 * register accesses: on silicon it is the ROM's job to commit a pending
 * SCLK_HF source switch at a moment the flash is idle, and how it does so
 * is not documented.  Here it writes CSIM_HAPI_PORT, a port the emulator
 * provides and the chip does not; the emulated oscillator block commits the
 * switch that DDI_0_OSC:STAT0.PENDINGSCLKHFSWITCHING reports.
 */
#include <stdint.h>
#include "inc/hw_types.h"
#include "inc/hw_memmap.h"
#include "inc/hw_fcfg1.h"
#include "inc/hw_flash.h"
#include "inc/hw_nvic.h"

#define CSIM_HAPI_PORT            0x10040000u
#define CSIM_HAPI_HF_SAFE_SWITCH  1u
#define CSIM_HAPI_UNIMPLEMENTED   4u      /* port offset: write the caller */

/* Every ROM slot rom.h does not name lands here.  The emulator reports the
 * call site (LR) on stderr, then the CPU stays in this loop: a debugger
 * stops on the bkpt, and without one the node stays parked here. */
void rom_unimplemented(void) {
    uint32_t lr;
    __asm volatile ("mov %0, lr" : "=r" (lr));
    HWREG(CSIM_HAPI_PORT + CSIM_HAPI_UNIMPLEMENTED) = lr;
    for (;;)
        __asm volatile ("bkpt #0xc5");
}

static uint32_t hapi_crc32(uint8_t *data, uint32_t count, uint32_t repeat) {
    /* IEEE 802.3 CRC-32 (reflected 0xEDB88320, init and final XOR all ones),
     * over the buffer repeat + 1 times. */
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t r = 0; r <= repeat; r++) {
        for (uint32_t i = 0; i < count; i++) {
            crc ^= data[i];
            for (int b = 0; b < 8; b++)
                crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
        }
    }
    return ~crc;
}

static uint32_t hapi_flash_size(void) {
    uint32_t sectors = HWREG(FLASH_BASE + FLASH_O_FLASH_SIZE) &
                       FLASH_FLASH_SIZE_SECTORS_M;
    uint32_t sector_kb = (HWREG(FLASH_BASE + FLASH_O_FCFG_B0_SSIZE0) &
                          FLASH_FCFG_B0_SSIZE0_B0_SECT_SIZE_M) >>
                         FLASH_FCFG_B0_SSIZE0_B0_SECT_SIZE_S;
    return sectors * sector_kb * 1024u;
}

static uint32_t hapi_chip_id(void) {
    /* The 16-bit part identifier: ICEPICK_DEVICE_ID[27:12]. */
    return (HWREG(FCFG1_BASE + FCFG1_O_ICEPICK_DEVICE_ID) >> 12) & 0xFFFFu;
}

static void hapi_reset_device(void) {
    HWREG(NVIC_APINT) = NVIC_APINT_VECTKEY | NVIC_APINT_SYSRESETREQ;
    for (;;)
        ;
}

static uint32_t hapi_fletcher32(uint16_t *data, uint16_t words, uint16_t repeat) {
    uint32_t s1 = 0xFFFFu, s2 = 0xFFFFu;
    for (uint32_t r = 0; r <= repeat; r++) {
        for (uint32_t i = 0; i < words; i++) {
            s1 = (s1 + data[i]) % 0xFFFFu;
            s2 = (s2 + s1) % 0xFFFFu;
        }
    }
    return (s2 << 16) | s1;
}

static uint32_t hapi_min(uint32_t *buf, uint32_t n) {
    uint32_t m = buf[0];
    for (uint32_t i = 1; i < n; i++)
        if ((int32_t)buf[i] < (int32_t)m) m = buf[i];
    return m;
}

static uint32_t hapi_max(uint32_t *buf, uint32_t n) {
    uint32_t m = buf[0];
    for (uint32_t i = 1; i < n; i++)
        if ((int32_t)buf[i] > (int32_t)m) m = buf[i];
    return m;
}

static uint32_t hapi_mean(uint32_t *buf, uint32_t n) {
    int64_t sum = 0;
    for (uint32_t i = 0; i < n; i++) sum += (int32_t)buf[i];
    return (uint32_t)(int32_t)(sum / (int64_t)n);
}

static uint32_t hapi_stddev(uint32_t *buf, uint32_t n) {
    /* Integer standard deviation: sqrt(mean of squared deviations). */
    int32_t mean = (int32_t)hapi_mean(buf, n);
    uint64_t acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        int64_t d = (int64_t)(int32_t)buf[i] - mean;
        acc += (uint64_t)(d * d);
    }
    acc /= n;
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > acc) bit >>= 2;
    while (bit) {
        if (acc >= r + bit) { acc -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

static void hapi_hf_source_safe_switch(void) {
    HWREG(CSIM_HAPI_PORT) = CSIM_HAPI_HF_SAFE_SWITCH;
}

__attribute__((section(".rom_hapi"), used))
const void *const rom_hapi_table[19] = {
    (const void *)hapi_crc32,
    (const void *)hapi_flash_size,
    (const void *)hapi_chip_id,
    (const void *)rom_unimplemented,     /* ReservedLocation1 */
    (const void *)rom_unimplemented,     /* ReservedLocation2 */
    (const void *)rom_unimplemented,     /* ReservedLocation3 */
    (const void *)hapi_reset_device,
    (const void *)hapi_fletcher32,
    (const void *)hapi_min,
    (const void *)hapi_max,
    (const void *)hapi_mean,
    (const void *)hapi_stddev,
    (const void *)rom_unimplemented,     /* ReservedLocation4 */
    (const void *)rom_unimplemented,     /* ReservedLocation5 */
    (const void *)hapi_hf_source_safe_switch,
    (const void *)rom_unimplemented,     /* SelectCompAInput */
    (const void *)rom_unimplemented,     /* SelectCompARef */
    (const void *)rom_unimplemented,     /* SelectADCCompBInput */
    (const void *)rom_unimplemented,     /* SelectDACVref / SelectCompBRef */
};
