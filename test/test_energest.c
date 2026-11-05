/*
 * The energest engine on hand-made event streams — unit tests, no
 * simulation run (tools/check-energest.sh runs the plugin end to end).
 *
 *  - TX shorter than its frame on air fails the report (CHECK FAILED,
 *    return -1), unless the board's radio is a known under-model (the
 *    nRF54L15's fixed 100 us TX: flagged approximate in the summary and
 *    the frames); a cycle of stamp skew passes.
 *  - The MSP430 stamp clock (msp430_cpu_now_ns) across an MCLK change in
 *    the middle of a slice that began asleep: the change re-anchors the
 *    simulation's clock to a sim_time_ns that already counts the slice's
 *    LPM fast-forward; the stamps carry their own origin and do not jump.
 */
#include "../src/services/energest_engine.h"
#include "msp430_cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* --- a sink that keeps what the engine says -------------------------- */

static char out[65536];
static size_t out_len;
static char frames[262144];
static size_t frames_len;
static const char *board;

static void cap_log(const char *line) {
    size_t n = strlen(line);
    if (out_len + n < sizeof out) { memcpy(out + out_len, line, n + 1); out_len += n; }
}
static bool cap_frame(sim_runtime_t *sim, const char *id, const char *json) {
    (void)sim; (void)id;
    size_t n = strlen(json);
    if (frames_len + n + 1 < sizeof frames) {
        memcpy(frames + frames_len, json, n);
        frames_len += n;
        frames[frames_len++] = '\n';
        frames[frames_len] = '\0';
    }
    return true;
}
static bool cap_describe(sim_runtime_t *sim, int idx, energest_node_info_t *o) {
    (void)sim;
    o->id = idx + 1;
    o->board = board;
    o->start_ns = 0;
    return true;
}

static energest_engine_t *engine(const char *b, const char *json) {
    out_len = frames_len = 0;
    out[0] = frames[0] = '\0';
    board = b;
    energest_sink_t sink = { .log = cap_log, .push_frame = cap_frame,
                             .describe = cap_describe };
    energest_engine_t *e = energest_engine_create(sink);
    char err[256] = "";
    if (e && energest_engine_configure(e, json, err, sizeof err) != 0) {
        printf("  configure: %s\n", err);
        energest_engine_free(e);
        return NULL;
    }
    return e;
}

static void ev(energest_engine_t *e, sim_observer_event_t x) {
    energest_engine_on_event(e, NULL, &x);
}
static void radio(energest_engine_t *e, int64_t t, int state) {
    sim_observer_event_t x = { .kind = SIM_OBS_RADIO_STATE, .time_ns = t, .mote_index = 0 };
    x.u.radio_state.state = state;
    ev(e, x);
}
/* A frame of `bytes` bytes on air from t0, reported at its last byte. */
static void airtime(energest_engine_t *e, int64_t t0, int bytes) {
    sim_observer_event_t x = { .kind = SIM_OBS_RADIO_AIRTIME,
                               .time_ns = t0 + bytes * 32000LL, .mote_index = 0 };
    x.u.airtime.start_ns = t0;
    x.u.airtime.byte_ns = 32000;
    x.u.airtime.air_bytes = bytes;
    x.u.airtime.psdu_len = bytes - 6;
    ev(e, x);
}
static void stop(energest_engine_t *e, int64_t t) {
    sim_observer_event_t x = { .kind = SIM_OBS_SIM_STOP, .time_ns = t, .mote_index = -1 };
    ev(e, x);
}

/* One TX of a 20-byte frame (640 us on air) whose dwell lasts dwell_ns. */
static int one_tx(const char *b, int64_t dwell_ns) {
    energest_engine_t *e = engine(b, NULL);
    if (!e) return 99;
    const int64_t t0 = 1000000;              /* 1 ms */
    radio(e, 0, 1);                          /* listen */
    radio(e, t0, 2);                         /* tx */
    airtime(e, t0, 20);                      /* the bus times the frame (in TX) */
    radio(e, t0 + dwell_ns, 1);              /* back to listen */
    stop(e, 3000000);
    int rc = energest_engine_report(e, NULL);
    energest_engine_free(e);
    return rc;
}

static void test_tx_short(void) {
    /* A radio model that leaves TX 100 us into a 640 us frame. */
    int rc = one_tx("cc2538dk", 100000);
    CHECK(rc != 0, "a TX 540 us shorter than its frame passes the report (rc %d)", rc);
    CHECK(strstr(out, "CHECK FAILED: a tx dwell 540.000 us shorter than its frame's air time"),
          "no CHECK FAILED for the short TX:\n%s", out);
    CHECK(!strstr(frames, "\"approx\""), "a cc2538dk radio lane flagged approximate");

    /* A stamp a cycle early (the CC2538's -0.063 us) is not a short TX. */
    rc = one_tx("cc2538dk", 640000 - 63);
    CHECK(rc == 0 && !strstr(out, "CHECK FAILED"),
          "a TX 63 ns short of its frame fails the report (rc %d):\n%s", rc, out);

    /* The nRF54L15's model ends every TX 100 us after it starts: known. */
    rc = one_tx("nrf54l15-dk", 100000);
    CHECK(rc == 0 && !strstr(out, "CHECK FAILED"),
          "the nRF54L15's fixed-length TX fails the report (rc %d):\n%s", rc, out);
    CHECK(strstr(out, "radio APPROXIMATE") &&
          strstr(out, "note     radio approximate: tx is the nRF54L15 model's fixed 100 us"),
          "the nRF54L15 radio is not flagged approximate in the summary:\n%s", out);
    CHECK(strstr(frames, "{\"c\":\"radio\",\"base\":\"listen\",\"approx\":true"),
          "the nRF54L15 radio lane is not flagged approximate in the frames:\n%s", frames);
}

extern const msp430_config_t msp430f1611_config;

static void test_msp430_stamp_freq_change(void) {
    msp430_cpu_t cpu;
    msp430_cpu_init(&cpu, &msp430f1611_config);
    msp430_cpu_set_frequency(&cpu, 4000000);          /* 250 ns a cycle */
    /* A slice starts at 10 ms (the runner's pin, msp430_elf_mote.c). */
    const int64_t S = 10000000;
    cpu.sim_time_ns = cpu.anchor_sim_time_ns = S;
    cpu.anchor_cycles = cpu.cycles;
    cpu.anchor_lpm_ns = cpu.lpm_ns;
#ifndef ENERGEST_TEST_NO_PIN_STAMP
    msp430_cpu_pin_stamp(&cpu);
#endif
    /* The core slept across the gap before the slice: the LPM fast-forward
     * adds 1 ms of cycles and of lpm_ns (msp430_cpu.c), then the wake at S
     * runs 40 cycles (10 us). */
    cpu.cycles += 4000;
    cpu.lpm_ns += 1000000;
    cpu.cycles += 40;
    int64_t t = msp430_cpu_now_ns(&cpu);
    CHECK(t == S + 10000, "before the MCLK change: %lld ns, want %lld", (long long)t,
          (long long)(S + 10000));
    /* The firmware writes DCOCTL/BCSCTL: MCLK to 8 MHz (msp430_clock.c). */
    msp430_cpu_set_frequency(&cpu, 8000000);
    t = msp430_cpu_now_ns(&cpu);
    CHECK(t == S + 10000, "at the MCLK change: %lld ns, want %lld (the slice's 1 ms "
          "of sleep counted again?)", (long long)t, (long long)(S + 10000));
    cpu.cycles += 80;                                  /* 10 us at 8 MHz */
    t = msp430_cpu_now_ns(&cpu);
    CHECK(t == S + 20000, "10 us after the MCLK change: %lld ns, want %lld",
          (long long)t, (long long)(S + 20000));
    /* The simulation's own clock is what it was: re-anchored to its
     * sim_time_ns, the 1 ms fast-forward included, until the next pin. */
    CHECK(cpu.anchor_sim_time_ns == S + 1010000, "the simulation's anchor moved: %lld",
          (long long)cpu.anchor_sim_time_ns);
    msp430_cpu_destroy(&cpu);
}

int run_energest_tests(int verbose) {
    (void)verbose;
    printf("=== energest engine ===\n");
    checks = failures = 0;
    test_tx_short();
    test_msp430_stamp_freq_change();
    printf("  %d checks passed, %d failed\n", checks - failures, failures);
    return failures;
}
