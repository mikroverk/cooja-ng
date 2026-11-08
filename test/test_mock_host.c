/*
 * mock_sim_host_t unit tests
 *
 * Sanity tests of the mock host itself, plus one end-to-end demo that
 * drives the CC2420 driver through the mock to show the pattern for
 * future per-chip unit tests (cf. docs/porting-a-device.md §"Test
 * layering").
 */
#include "mock_sim_host.h"
#include "cc2420.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static int passed = 0;
static int failed = 0;

#define ASSERT(cond, msg) do {                                          \
    if (cond) { passed++; }                                             \
    else { failed++; printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

/* ---------- self-tests for the mock itself ---------- */

static int dummy_fired;
static void dummy_event_cb(void *user_data, cpu_event_t *ev) {
    (void)user_data; (void)ev;
    dummy_fired++;
}

static void test_set_pin_recording(void) {
    mock_sim_host_t mock;
    mock_sim_host_init(&mock);

    mock.host.set_input_pin(mock.host.gpio, 1, 3, true);
    ASSERT(mock.set_input_pin_calls == 1, "set_input_pin counter");
    ASSERT(mock.last_set_pin.port == 1,   "last_set_pin.port");
    ASSERT(mock.last_set_pin.pin  == 3,   "last_set_pin.pin");
    ASSERT(mock.last_set_pin.value == true, "last_set_pin.value");
}

static void test_force_irq_recording(void) {
    mock_sim_host_t mock;
    mock_sim_host_init(&mock);

    mock.host.force_irq_edge(mock.host.gpio, 2, 5, false);
    ASSERT(mock.force_irq_edge_calls == 1, "force_irq counter");
    ASSERT(mock.last_force_irq.port == 2, "force_irq.port");
    ASSERT(mock.last_force_irq.pin  == 5, "force_irq.pin");
    ASSERT(mock.last_force_irq.rising == false, "force_irq.rising");
}

static void test_event_scheduling_and_firing(void) {
    mock_sim_host_t mock;
    mock_sim_host_init(&mock);

    cpu_event_t ev = { .callback = dummy_event_cb };
    dummy_fired = 0;

    mock.host.schedule_ns(mock.host.cpu, &ev, 1000);  /* fire at 1us */
    ASSERT(mock.schedule_calls == 1, "schedule counter");
    ASSERT(mock.queue_len == 1, "queue contains event");

    mock_sim_host_advance_ns(&mock, 500);             /* not yet */
    ASSERT(dummy_fired == 0, "event not yet fired");

    mock_sim_host_advance_ns(&mock, 600);             /* past 1us */
    ASSERT(dummy_fired == 1, "event fired exactly once");
    ASSERT(mock.queue_len == 0, "queue drained");
    ASSERT(mock.now_ns == 1100, "virtual clock advanced");
}

static void test_cancel_removes_event(void) {
    mock_sim_host_t mock;
    mock_sim_host_init(&mock);

    cpu_event_t ev = { .callback = dummy_event_cb };
    dummy_fired = 0;

    mock.host.schedule_ns(mock.host.cpu, &ev, 1000);
    mock.host.cancel(mock.host.cpu, &ev);
    ASSERT(mock.cancel_calls == 1, "cancel counter");
    ASSERT(mock.queue_len == 0, "queue empty after cancel");

    mock_sim_host_advance_ns(&mock, 5000);
    ASSERT(dummy_fired == 0, "cancelled event must not fire");
}

static void test_event_ordering(void) {
    mock_sim_host_t mock;
    mock_sim_host_init(&mock);

    cpu_event_t a = { .callback = dummy_event_cb };
    cpu_event_t b = { .callback = dummy_event_cb };
    dummy_fired = 0;

    /* Schedule out of order — mock must fire in time order. */
    mock.host.schedule_ns(mock.host.cpu, &a, 2000);
    mock.host.schedule_ns(mock.host.cpu, &b,  500);

    mock_sim_host_advance_ns(&mock, 3000);
    ASSERT(dummy_fired == 2, "both events fired");
    ASSERT(mock.queue_len == 0, "queue drained");
}

/* ---------- end-to-end: drive the CC2420 driver via mock ---------- */

/* This isn't an exhaustive CC2420 test — it just demonstrates the
 * pattern. The point is: no msp430_cpu_t, no msp430_gpio_t, no real
 * MCU. Tests for chip drivers can run in isolation. */
static void test_cc2420_vreg_off_to_on(void) {
    mock_sim_host_t mock;
    mock_sim_host_init(&mock);

    cc2420_t radio;
    cc2420_init(&radio, &mock.host);
    /* Configure the pins so the chip knows where to drive its outputs. */
    cc2420_set_pins(&radio,
                    /* fifop */ 1, 0,
                    /* fifo  */ 1, 1,
                    /* cca   */ 1, 2,
                    /* sfd   */ 4, 1);

    int prior_set_pin = mock.set_input_pin_calls;

    /* Turn VREG on — the chip should transition to POWER_DOWN, which
     * doesn't actually pulse pins yet, but it MUST NOT crash and MUST
     * NOT touch CPU state directly. */
    cc2420_set_vreg(&radio, true);
    ASSERT(mock.set_input_pin_calls >= prior_set_pin, "no negative side-effects");

    /* Turning it off again must cancel any scheduled events. */
    int prior_cancel = mock.cancel_calls;
    cc2420_set_vreg(&radio, false);
    ASSERT(mock.cancel_calls > prior_cancel, "VREG off cancels pending events");
}

/* RSSI and CCA follow the channel: a transmission window announced with
 * cc2420_channel_activity makes the chip re-read the host's channel
 * energy at its start and at its end. */
static int64_t cca_air_start, cca_air_end;
static int     cca_air_dbm;
static int64_t *cca_clock;
static int fake_channel_energy(void *user, int radio_idx, int64_t at_ns,
                               int64_t span_ns) {
    (void)user; (void)radio_idx; (void)span_ns;
    int64_t t = at_ns >= 0 ? at_ns : *cca_clock;
    return (t >= cca_air_start && t < cca_air_end) ? cca_air_dbm : -128;
}

static void cc2420_strobe(cc2420_t *r, uint8_t cmd) {
    cc2420_set_chip_select(r, true);
    cc2420_spi_exchange(r, cmd);
    cc2420_set_chip_select(r, false);
}

static void test_cc2420_cca_follows_channel(void) {
    mock_sim_host_t mock;
    mock_sim_host_init(&mock);
    mock.host.radio_channel_energy = fake_channel_energy;
    cca_clock = &mock.now_ns;
    cc2420_t radio;
    cc2420_init(&radio, &mock.host);
    cc2420_set_pins(&radio, 1, 0, 1, 1, 1, 2, 4, 1);
    cc2420_set_vreg(&radio, true);
    mock_sim_host_advance_ns(&mock, 2000000);
    cc2420_strobe(&radio, CC2420_REG_SXOSCON);
    mock_sim_host_advance_ns(&mock, 2000000);
    cc2420_strobe(&radio, CC2420_REG_SRXON);
    mock_sim_host_advance_ns(&mock, 1000000);
    ASSERT(radio.state == CC2420_RX_SFD_SEARCH, "cca: in SFD search");
    int8_t rssi = (int8_t)(radio.registers[CC2420_REG_RSSI] & 0xFF);
    ASSERT(rssi == -55, "cca: RSSI_VAL is the noise floor (-100 dBm) on a quiet channel");
    ASSERT(radio.current_cca, "cca: clear on a quiet channel");

    /* A -60 dBm frame from 6 ms to 7 ms, announced at 5.5 ms. */
    cca_air_start = 6000000; cca_air_end = 7000000; cca_air_dbm = -60;
    mock_sim_host_set_now(&mock, 5500000);
    cc2420_channel_activity(&radio, cca_air_start, cca_air_end);
    mock_sim_host_advance_ns(&mock, 400000);              /* 5.9 ms */
    ASSERT(radio.current_cca, "cca: still clear before the frame starts");
    mock_sim_host_advance_ns(&mock, 200000);              /* 6.1 ms */
    rssi = (int8_t)(radio.registers[CC2420_REG_RSSI] & 0xFF);
    ASSERT(rssi == -60 + 45, "cca: RSSI_VAL = the frame's dBm + 45 while it is on the air");
    ASSERT(!radio.current_cca, "cca: busy while the frame is on the air");
    mock_sim_host_advance_ns(&mock, 1000000);             /* 7.1 ms */
    rssi = (int8_t)(radio.registers[CC2420_REG_RSSI] & 0xFF);
    ASSERT(rssi == -55, "cca: back to the noise floor after the frame");
    ASSERT(radio.current_cca, "cca: clear again after the frame");

    /* A frame below the threshold (CCA_THR -32 = -77 dBm) is not busy. */
    cca_air_start = 8000000; cca_air_end = 9000000; cca_air_dbm = -85;
    mock_sim_host_set_now(&mock, 7500000);
    cc2420_channel_activity(&radio, cca_air_start, cca_air_end);
    mock_sim_host_advance_ns(&mock, 1000000);             /* 8.5 ms */
    rssi = (int8_t)(radio.registers[CC2420_REG_RSSI] & 0xFF);
    ASSERT(rssi == -85 + 45, "cca: a weak frame shows in RSSI_VAL");
    ASSERT(radio.current_cca, "cca: ... but is below the reset threshold, so clear");
}

/* The CCA event fires a fraction of a cycle early on a CPU clock that
 * rounds ns down to cycles: it must not re-arm for the end it was set for
 * (that looped forever at one cycle on every Sky network with traffic). */
static void test_cc2420_cca_event_early_clock(void) {
    mock_sim_host_t mock;
    mock_sim_host_init(&mock);
    mock.host.radio_channel_energy = fake_channel_energy;
    cca_clock = &mock.now_ns;
    cca_air_start = 1000000; cca_air_end = 2000000; cca_air_dbm = -60;
    cc2420_t radio;
    cc2420_init(&radio, &mock.host);
    mock_sim_host_set_now(&mock, 1000000);
    cc2420_channel_activity(&radio, cca_air_start, cca_air_end);
    mock_sim_host_advance_ns(&mock, 0);                  /* the start event */
    ASSERT(radio.cca_event_at == cca_air_end, "early clock: armed for the frame's end");
    /* Fire it with the clock 1 ns short of that end. */
    mock_sim_host_set_now(&mock, cca_air_end - 1);
    radio.cca_event.callback(radio.cca_event.user_data, &radio.cca_event);
    ASSERT(radio.cca_event_at == INT64_MAX, "early clock: not re-armed for the same end");
    ASSERT(radio.air_n == 0, "early clock: the end counts as passed");

    /* And a listening radio comes out clear, not stuck busy: its end event
     * reads the channel at the frame's end, not 1 ns before it, and a later
     * look with the same stale clock does not go back before that end. */
    cc2420_t rx;
    mock_sim_host_t m2;
    mock_sim_host_init(&m2);
    m2.host.radio_channel_energy = fake_channel_energy;
    cca_clock = &m2.now_ns;
    cc2420_init(&rx, &m2.host);
    cc2420_set_pins(&rx, 1, 0, 1, 1, 1, 2, 4, 1);
    cc2420_set_vreg(&rx, true);
    mock_sim_host_advance_ns(&m2, 2000000);
    cc2420_strobe(&rx, CC2420_REG_SXOSCON);
    mock_sim_host_advance_ns(&m2, 2000000);
    cc2420_strobe(&rx, CC2420_REG_SRXON);
    mock_sim_host_advance_ns(&m2, 1000000);              /* 5 ms, SFD search */
    cca_air_start = 6000000; cca_air_end = 7000000; cca_air_dbm = -60;
    cc2420_channel_activity(&rx, cca_air_start, cca_air_end);
    mock_sim_host_advance_ns(&m2, 1500000);              /* 6.5 ms */
    ASSERT(!rx.current_cca, "early clock: busy during the frame");
    mock_sim_host_set_now(&m2, cca_air_end - 1);
    rx.cca_event.callback(rx.cca_event.user_data, &rx.cca_event);
    ASSERT(rx.current_cca, "early clock: clear after the frame, though the clock read 1 ns early");
    ASSERT((int8_t)(rx.registers[CC2420_REG_RSSI] & 0xFF) == -55,
           "early clock: RSSI back at the noise floor");
    cc2420_strobe(&rx, CC2420_REG_SRXON);                /* a re-evaluation, stale clock */
    mock_sim_host_advance_ns(&m2, 300000);
    ASSERT(rx.current_cca, "early clock: still clear on a later look");
}

int run_mock_host_tests(int verbose) {
    (void)verbose;
    printf("=== Mock sim_host_t Tests ===\n");

    test_set_pin_recording();
    test_force_irq_recording();
    test_event_scheduling_and_firing();
    test_cancel_removes_event();
    test_event_ordering();
    test_cc2420_vreg_off_to_on();
    test_cc2420_cca_follows_channel();
    test_cc2420_cca_event_early_clock();

    printf("\n--- Results: %d passed, %d failed ---\n\n", passed, failed);
    return failed;
}
