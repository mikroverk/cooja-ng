/*
 * nRF52840 RADIO: clear channel assessment (nRF52840 PS, RADIO, "IEEE
 * 802.15.4 clear channel assessment").
 *
 * An nrf52840-dk platform with the CPU parked on a `b .` in SRAM, so time
 * passes and the radio's events fire with no firmware.  The test triggers
 * tasks and reads events through the bus as a driver does, and plays the
 * channel with a fake energy source in place of the radio bus.
 */
#include "arm_platform.h"
#include "nrf52840_soc.h"
#include <stdio.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

#define ASSERT(cond, msg) do {                                          \
    if (cond) { passed++; }                                             \
    else { failed++; printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

#define ASSERT_EQ(actual, expected, msg) do {                            \
    long long a_ = (long long)(actual), e_ = (long long)(expected);      \
    if (a_ == e_) { passed++; }                                          \
    else { failed++;                                                     \
        printf("  FAIL: %s: got %lld, want %lld (%s:%d)\n",              \
               msg, a_, e_, __FILE__, __LINE__); }                       \
} while (0)

/* RADIO registers (nRF52840 PS). */
#define RADIO              0x40001000u
#define TASKS_RXEN         (RADIO + 0x004)
#define TASKS_DISABLE      (RADIO + 0x010)
#define TASKS_CCASTART     (RADIO + 0x02C)
#define TASKS_CCASTOP      (RADIO + 0x030)
#define EVENTS_CCAIDLE     (RADIO + 0x144)
#define EVENTS_CCABUSY     (RADIO + 0x148)
#define EVENTS_CCASTOPPED  (RADIO + 0x14C)
#define SHORTS             (RADIO + 0x200)
#define MODE               (RADIO + 0x510)
#define STATE              (RADIO + 0x550)
#define CCACTRL            (RADIO + 0x66C)
#define SHORT_RXREADY_CCASTART (1u << 11)

/* CCACTRL: CCAMODE [2:0], CCAEDTHRES [15:8]. */
#define CCA_ED            0u
#define CCA_CARRIER       1u
#define CCA_CARRIER_AND_ED 2u
#define CCA_CARRIER_OR_ED 3u
#define CCACTRL_VAL(mode, edthres) ((mode) | ((uint32_t)(edthres) << 8))

#define LOOP_ADDR 0x20001000u
#define US        1000LL

static arm_platform_t plat;
static bool plat_live;
static arm_cpu_t *cpu;
static nrf52840_soc_t *soc;

static int fake_dbm;
static int64_t last_span;
static int energy_calls;
static int fake_energy(void *user, int radio_idx, int64_t at_ns, int64_t span_ns) {
    (void)user; (void)radio_idx; (void)at_ns;
    last_span = span_ns;
    energy_calls++;
    return fake_dbm;
}

static int64_t now_ns(void) { return cpu_cycles_to_ns(cpu->cycles, cpu->cpu_freq_hz); }
static void run_for(int64_t ns) {
    arm_step_until(cpu, cpu->cycles + cpu_ns_to_cycles_ceil(ns, cpu->cpu_freq_hz));
}
static uint32_t rd(uint32_t a) { return arm_read32(cpu, a); }
static void wr(uint32_t a, uint32_t v) { arm_write32(cpu, a, v); }

static void setup(void) {
    if (plat_live) arm_platform_destroy(&plat);
    plat_live = true;
    arm_platform_init(&plat, arm_platform_find("nrf52840-dk"));
    cpu = &plat.cpu;
    soc = arm_platform_nrf52840(&plat);
    arm_write16(cpu, LOOP_ADDR, 0xE7FE);          /* b . */
    cpu->reg[ARM_PC] = LOOP_ADDR;
    cpu->xpsr = 1u << 24;
    soc->radio.energy_fn = fake_energy;
    soc->radio.energy_user = NULL;
    fake_dbm = -128;
    energy_calls = 0;
    run_for(1000 * US);
    wr(MODE, 15);                                 /* Ieee802154_250Kbit */
    wr(CCACTRL, CCACTRL_VAL(CCA_CARRIER_AND_ED, 0x14));  /* Contiki-NG's */
}

static void clear_cca_events(void) {
    wr(EVENTS_CCAIDLE, 0);
    wr(EVENTS_CCABUSY, 0);
    wr(EVENTS_CCASTOPPED, 0);
}

/* RXEN, then the 40 us ramp-up: RXIDLE. */
static void rx_on(void) {
    wr(TASKS_RXEN, 1);
    run_for(50 * US);
}

/* CCASTART in RXIDLE: nothing for 8 symbol periods (128 us), then the
 * verdict, from the energy over those 128 us. */
static void test_cca_takes_eight_symbols(void) {
    setup();
    rx_on();
    ASSERT_EQ(rd(STATE), NRF_RADIO_STATE_RXIDLE, "RXEN: RXIDLE after the ramp-up");
    clear_cca_events();
    wr(TASKS_CCASTART, 1);
    ASSERT_EQ(rd(EVENTS_CCAIDLE) | rd(EVENTS_CCABUSY), 0, "CCASTART: no verdict at once");
    run_for(120 * US);
    ASSERT_EQ(rd(EVENTS_CCAIDLE) | rd(EVENTS_CCABUSY), 0, "CCASTART: no verdict at 120 us");
    run_for(10 * US);
    ASSERT_EQ(rd(EVENTS_CCAIDLE), 1, "CCASTART: CCAIDLE after 128 us on a quiet channel");
    ASSERT_EQ(rd(EVENTS_CCABUSY), 0, "CCASTART: not CCABUSY");
    ASSERT_EQ(last_span, 128 * US, "CCA: the energy over the 8 symbols");
}

/* A CCASTART during the ramp-up waits for RXREADY, then takes its 128 us. */
static void test_cca_waits_for_rampup(void) {
    setup();
    wr(TASKS_RXEN, 1);
    ASSERT_EQ(rd(STATE), NRF_RADIO_STATE_RXRU, "RXEN: ramping up");
    clear_cca_events();
    int64_t t0 = now_ns();
    wr(TASKS_CCASTART, 1);
    run_for(150 * US);                            /* 40 + 128 = 168 us */
    ASSERT_EQ(rd(EVENTS_CCAIDLE) | rd(EVENTS_CCABUSY), 0,
              "CCASTART in RXRU: no verdict 150 us later");
    run_for(30 * US);
    ASSERT_EQ(rd(EVENTS_CCAIDLE), 1, "CCASTART in RXRU: CCAIDLE once ramp-up + 128 us passed");
    ASSERT(now_ns() - t0 >= 168 * US, "CCASTART in RXRU: not before 168 us");

    /* The RXREADY_CCASTART short does the same without firmware. */
    setup();
    wr(SHORTS, SHORT_RXREADY_CCASTART);
    clear_cca_events();
    wr(TASKS_RXEN, 1);
    run_for(160 * US);
    ASSERT_EQ(rd(EVENTS_CCAIDLE) | rd(EVENTS_CCABUSY), 0, "RXREADY_CCASTART: no verdict at 160 us");
    run_for(20 * US);
    ASSERT_EQ(rd(EVENTS_CCAIDLE), 1, "RXREADY_CCASTART: CCAIDLE at 168 us");
}

/* The verdict per CCAMODE and CCAEDTHRES (ED level = dBm + 92). */
static bool cca_busy_with(uint32_t ccactrl, int dbm) {
    setup();
    wr(CCACTRL, ccactrl);
    fake_dbm = dbm;
    rx_on();
    clear_cca_events();
    wr(TASKS_CCASTART, 1);
    run_for(140 * US);
    ASSERT_EQ(rd(EVENTS_CCAIDLE) + rd(EVENTS_CCABUSY), 1, "exactly one verdict");
    return rd(EVENTS_CCABUSY) != 0;
}

static void test_cca_modes(void) {
    /* ED mode, threshold 20 (-72 dBm). */
    ASSERT(cca_busy_with(CCACTRL_VAL(CCA_ED, 20), -60), "ED: -60 dBm (level 32) busy");
    ASSERT(!cca_busy_with(CCACTRL_VAL(CCA_ED, 20), -80), "ED: -80 dBm (level 12) idle");
    ASSERT(!cca_busy_with(CCACTRL_VAL(CCA_ED, 20), -128), "ED: quiet channel idle");
    /* Carrier: any 802.15.4 signal. */
    ASSERT(cca_busy_with(CCACTRL_VAL(CCA_CARRIER, 20), -80), "Carrier: a weak frame busy");
    ASSERT(!cca_busy_with(CCACTRL_VAL(CCA_CARRIER, 20), -128), "Carrier: quiet channel idle");
    /* Carrier and ED (Contiki-NG's default). */
    ASSERT(cca_busy_with(CCACTRL_VAL(CCA_CARRIER_AND_ED, 20), -60), "Carrier&ED: strong frame busy");
    ASSERT(!cca_busy_with(CCACTRL_VAL(CCA_CARRIER_AND_ED, 20), -80), "Carrier&ED: weak frame idle");
    /* Carrier or ED. */
    ASSERT(cca_busy_with(CCACTRL_VAL(CCA_CARRIER_OR_ED, 20), -80), "Carrier|ED: weak frame busy");
    ASSERT(!cca_busy_with(CCACTRL_VAL(CCA_CARRIER_OR_ED, 20), -128), "Carrier|ED: quiet idle");
}

/* CCASTOP ends an assessment with CCASTOPPED and no verdict; DISABLE
 * ends one with nothing; outside RX, CCASTART does nothing. */
static void test_cca_stop_and_outside_rx(void) {
    setup();
    rx_on();
    clear_cca_events();
    wr(TASKS_CCASTART, 1);
    run_for(50 * US);
    wr(TASKS_CCASTOP, 1);
    ASSERT_EQ(rd(EVENTS_CCASTOPPED), 1, "CCASTOP: CCASTOPPED");
    run_for(200 * US);
    ASSERT_EQ(rd(EVENTS_CCAIDLE) | rd(EVENTS_CCABUSY), 0, "CCASTOP: no verdict afterwards");

    setup();
    rx_on();
    clear_cca_events();
    wr(TASKS_CCASTART, 1);
    run_for(50 * US);
    wr(TASKS_DISABLE, 1);
    run_for(200 * US);
    ASSERT_EQ(rd(EVENTS_CCAIDLE) | rd(EVENTS_CCABUSY), 0, "DISABLE: the assessment is gone");

    setup();
    clear_cca_events();
    ASSERT_EQ(rd(STATE), NRF_RADIO_STATE_DISABLED, "reset: DISABLED");
    wr(TASKS_CCASTART, 1);
    run_for(300 * US);
    ASSERT_EQ(rd(EVENTS_CCAIDLE) | rd(EVENTS_CCABUSY), 0, "DISABLED: CCASTART does nothing");
    ASSERT_EQ(energy_calls, 0, "DISABLED: the channel was not even measured");
}

int run_nrf52840_radio_tests(int verbose) {
    (void)verbose;
    printf("=== nRF52840 RADIO: clear channel assessment ===\n");
    passed = failed = 0;
    test_cca_takes_eight_symbols();
    test_cca_waits_for_rampup();
    test_cca_modes();
    test_cca_stop_and_outside_rx();
    if (plat_live) arm_platform_destroy(&plat);
    plat_live = false;
    printf("  %d passed, %d failed\n", passed, failed);
    return failed;
}
