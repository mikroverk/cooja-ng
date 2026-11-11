/*
 * sim_radio_bus unit tests (Phase 5 guardrail, M24).
 *
 * Drive the radio bus in isolation through mock mote_radio_ops_t
 * receivers + scripted chip TX byte streams, with a real
 * sim_runtime_t event queue + radio_medium.  No real CPU, no chips.
 *
 * Purpose (docs/design/refactor-plan.md §3.18): pin the bus's current
 * behaviour BEFORE Phase 5 moves frame-delivery policy into it, so the
 * later character-identical moves have a fast unit-level detector.
 * Covers what the bus owns today (M9.4/M9.5): frame assembler, TX
 * capture + byte clock, delivery-mode routing, re-entrant depth
 * staging, RX-stall timer, spectrum matching, FIFO sizing.  The
 * collision-window / ACK-window / backpressure tests arrive with the
 * milestones that move that policy in (M26/M27).
 *
 * These tests assert CURRENT behaviour — if one exposes a bug, document
 * it; do not fix it in this milestone.
 */
#include "sim_radio_bus.h"
#include "sim_runtime.h"
#include "radio_medium.h"
#include "sim_event_queue.h"
#include "native_node.h"
#include "mote_impl.h"         /* the native Cooja mote's real radio ops */

#include <stdio.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

#define ASSERT(cond, msg) do {                                          \
    if (cond) { passed++; }                                             \
    else { failed++; printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

#define ASSERT_EQ(actual, expected, msg) do {                            \
    long long _a = (long long)(actual), _e = (long long)(expected);      \
    if (_a == _e) { passed++; }                                          \
    else { failed++;                                                     \
        printf("  FAIL: %s — got %lld, want %lld (%s:%d)\n",            \
               msg, _a, _e, __FILE__, __LINE__); }                       \
} while (0)

/* ============================================================
 * Mock receiver — records every byte the bus delivers to it.
 * ============================================================ */

#define MOCK_CAP 1024
typedef struct {
    int      idx;
    int      recv_count;
    uint8_t  recv_byte[MOCK_CAP];
    int8_t   recv_rssi[MOCK_CAP];
    int64_t  recv_time[MOCK_CAP];   /* sim now_ns at delivery */
    int      rxfifo;                /* reported free FIFO bytes */
    bool     busy;
    int      stall_count;
    /* Optional re-entrancy: when the byte == reenter_on (>=0) is
     * delivered synchronously (SYNC mode), emit one ACK byte back
     * through the bus as `reenter_sender`. */
    int      reenter_on;
    int      reenter_sender;
    sim_radio_bus_t *bus;
    sim_runtime_t   *sim;
    int64_t  now_at_recv;           /* the pump stamps this before calling */
    int      on_air_count;          /* on_air windows announced to it */
    int64_t  on_air_end;            /* latest end announced */
    int      recv_at_count;         /* bytes via receive_byte_at */
    int64_t  recv_air_ns;           /* air time of the last one */
    int      collision_marks;       /* mark_collisions calls */
    int64_t  coll_start, coll_end;  /* window of the last one */
} mock_rx_t;

static void mock_receive_byte(void *m, uint8_t byte, int8_t rssi) {
    mock_rx_t *r = (mock_rx_t *)m;
    if (r->recv_count < MOCK_CAP) {
        r->recv_byte[r->recv_count] = byte;
        r->recv_rssi[r->recv_count] = rssi;
        r->recv_time[r->recv_count] = r->now_at_recv;
        r->recv_count++;
    }
    if (r->reenter_on >= 0 && byte == (uint8_t)r->reenter_on)
        sim_radio_bus_tx_byte(r->bus, r->sim, r->reenter_sender, 0, 0xAC);
}
static int mock_rxfifo_available(void *m) { return ((mock_rx_t *)m)->rxfifo; }
static bool mock_rx_busy(void *m) { return ((mock_rx_t *)m)->busy; }
static void mock_rx_stall(void *m) { ((mock_rx_t *)m)->stall_count++; }

static const mote_radio_ops_t mock_ops = {
    mock_receive_byte, mock_rxfifo_available, mock_rx_busy, NULL, NULL, NULL, NULL, NULL, NULL,
};
static void mock_on_air(void *m, int64_t end) {
    mock_rx_t *r = (mock_rx_t *)m;
    if (r->on_air_count == 0 || end > r->on_air_end) r->on_air_end = end;
    r->on_air_count++;
}
/* A SYNC receiver that takes each byte with its air time, as the native
 * mote does. */
static void mock_receive_byte_at(void *m, uint8_t byte, int8_t rssi,
                                 int64_t air_ns) {
    mock_rx_t *r = (mock_rx_t *)m;
    r->recv_at_count++;
    r->recv_air_ns = air_ns;
    mock_receive_byte(m, byte, rssi);
}
static const mote_radio_ops_t mock_ops_on_air = {
    .receive_byte     = mock_receive_byte,
    .receive_byte_at  = mock_receive_byte_at,
    .rxfifo_available = mock_rxfifo_available,
    .rx_busy          = mock_rx_busy,
    .on_air           = mock_on_air,
};
static int mock_mark_collisions(void *m, int64_t start, int64_t end) {
    mock_rx_t *r = (mock_rx_t *)m;
    r->collision_marks++;
    r->coll_start = start;
    r->coll_end = end;
    return 0;
}
static const mote_radio_ops_t mock_ops_collisions = {
    .receive_byte     = mock_receive_byte,
    .rxfifo_available = mock_rxfifo_available,
    .rx_busy          = mock_rx_busy,
    .mark_collisions  = mock_mark_collisions,
};
static const mote_radio_ops_t mock_ops_stall = {
    mock_receive_byte, mock_rxfifo_available, mock_rx_busy, mock_rx_stall, NULL, NULL, NULL, NULL, NULL,
};

/* ============================================================
 * Fixture + event pump.
 * ============================================================ */

typedef struct {
    sim_runtime_t   sim;
    sim_radio_bus_t bus;
    mock_rx_t       rx[8];
} fixture_t;

static void fx_init(fixture_t *f, int node_count) {
    memset(f, 0, sizeof(*f));
    sim_runtime_init(&f->sim);
    sim_eq_init(&f->sim.event_queue);
    radio_medium_init(&f->sim.radio_medium, node_count);  /* NONE by default */
    f->sim.radio_bus = &f->bus;
    for (int i = 0; i < (int)(sizeof(f->rx) / sizeof(f->rx[0])); i++) {
        f->rx[i].idx = i;
        f->rx[i].rxfifo = 128;
        f->rx[i].reenter_on = -1;
        f->rx[i].bus = &f->bus;
        f->rx[i].sim = &f->sim;
    }
}

/* Drain SIM_EV_RX_BYTE / SIM_EV_RADIO_TIMER up to horizon, dispatching
 * to the bus's registered ops exactly as the runner's RX_BYTE / timer
 * dispatch does (this is the production seam). */
static void fx_pump(fixture_t *f, int64_t horizon) {
    sim_runtime_t *sim = &f->sim;
    while (!sim_eq_empty(&sim->event_queue)) {
        int64_t t = sim_eq_peek_time(&sim->event_queue);
        if (t > horizon) break;
        sim_event_t ev = sim_eq_pop(&sim->event_queue);
        sim->now_ns = ev.time_ns;
        if (ev.kind == SIM_EV_RX_BYTE) {
            const mote_radio_ops_t *ops = f->bus.ops[ev.node_idx];
            if (ops) {
                mock_rx_t *r = (mock_rx_t *)f->bus.mote[ev.node_idx];
                r->now_at_recv = ev.time_ns;
                ops->receive_byte(r, ev.byte, ev.rssi);
            }
        } else if (ev.kind == SIM_EV_RADIO_TIMER) {
            if (sim_radio_bus_rx_stall_expired(&f->bus, sim, ev.node_idx,
                                               ev.time_ns)) {
                const mote_radio_ops_t *ops = f->bus.ops[ev.node_idx];
                if (ops && ops->rx_stall)
                    ops->rx_stall(f->bus.mote[ev.node_idx]);
            }
        }
    }
}

/* Feed a byte sequence to the assembler, return how many feeds reported
 * frame-complete (should be exactly 1 for a single well-formed frame). */
static int feed_seq(tx_frame_asm_t *a, const uint8_t *seq, int n) {
    int completes = 0;
    for (int i = 0; i < n; i++)
        if (sim_radio_bus_asm_feed(a, seq[i])) completes++;
    return completes;
}

/* Mock emulated mote: provides the sim_mote_ops the bus's delivery path
 * drives (rx_byte_sync / step_until / cycles / freq_hz). */
typedef struct {
    sim_mote_t mote;
    int      sync_count;
    int64_t  sync_times[256];
    int      step_count;
    int64_t  cyc;
    uint32_t freq;
} mock_mote_t;

static void mm_rx_byte_sync(sim_mote_t *m, int64_t t) {
    mock_mote_t *mm = (mock_mote_t *)m->impl;
    if (mm->sync_count < 256) mm->sync_times[mm->sync_count] = t;
    mm->sync_count++;
}
static void mm_step_until(sim_mote_t *m, int64_t target) {
    mock_mote_t *mm = (mock_mote_t *)m->impl;
    mm->step_count++;
    mm->cyc = target;
}
static int64_t  mm_cycles(const sim_mote_t *m) { return ((mock_mote_t *)m->impl)->cyc; }
static uint32_t mm_freq(const sim_mote_t *m)   { return ((mock_mote_t *)m->impl)->freq; }

static const sim_mote_ops_t mm_ops = {
    .rx_byte_sync = mm_rx_byte_sync,
    .step_until   = mm_step_until,
    .cycles       = mm_cycles,
    .freq_hz      = mm_freq,
};

/* Register node idx as an emulated receiver: a mock_rx for the radio
 * endpoint + a mock_mote for the sim_mote ops, with the given caps. */
static void register_emulated(fixture_t *f, mock_mote_t *mm, int idx,
                              uint32_t caps) {
    memset(mm, 0, sizeof(*mm));
    mm->freq = 1000000;          /* 1 MHz → 1 ms = 1000 cycles */
    mm->mote.impl = mm;
    mm->mote.ops = &mm_ops;
    sim_runtime_register_mote(&f->sim, idx, &mm->mote);
    sim_radio_bus_register(&f->bus, idx, &mock_ops, &f->rx[idx],
                           SIM_RADIO_DELIVERY_PER_BYTE, caps);
}

/* A short 802.15.4 frame body (preamble..payload). */
static int short_frame(uint8_t *buf, int payload) {
    int n = 0;
    buf[n++] = 0; buf[n++] = 0; buf[n++] = 0; buf[n++] = 0;
    buf[n++] = 0x7A; buf[n++] = (uint8_t)payload;
    for (int i = 0; i < payload; i++) buf[n++] = (uint8_t)(0x40 + i);
    return n;
}


/* ============================================================
 * Frame assembler.
 * ============================================================ */

static void test_asm_802154(void) {
    tx_frame_asm_t a;
    sim_radio_bus_asm_reset(&a);
    /* preamble(4×0x00) + SFD(0x7A) + len(0x0A=10) + 10 payload bytes.
     * 802.15.4 length already counts the 2 FCS the chip appends, so the
     * frame completes on the 10th payload byte. */
    uint8_t frame[] = { 0,0,0,0, 0x7A, 10, 1,2,3,4,5,6,7,8,9,10 };
    int done = feed_seq(&a, frame, (int)sizeof(frame));
    ASSERT_EQ(done, 1, "802.15.4 frame completes exactly once");
    ASSERT_EQ(a.subghz, 0, "802.15.4 frame not flagged sub-GHz");
    ASSERT_EQ(a.expected_len, 10, "802.15.4 expected_len sticky after complete");

    /* Completes on the LAST payload byte, not before. */
    sim_radio_bus_asm_reset(&a);
    for (int i = 0; i < 15; i++)
        ASSERT(!sim_radio_bus_asm_feed(&a, frame[i]), "no early completion");
    ASSERT(sim_radio_bus_asm_feed(&a, frame[15]), "completes on 10th payload byte");
}

static void test_asm_802154g(void) {
    tx_frame_asm_t a;
    sim_radio_bus_asm_reset(&a);
    /* preamble(4×0x55) + sync(0x6E,0x4E,0x90,0x4E) + PHR(1-byte, len=8) +
     * 8 payload + 2 CRC.  sub-GHz length counts payload only; the chip
     * appends 2 CRC, so completion waits for payload+2. */
    uint8_t frame[] = {
        0x55,0x55,0x55,0x55, 0x6E,0x4E,0x90,0x4E, 8,
        1,2,3,4,5,6,7,8, 0xC1,0xC2
    };
    int done = feed_seq(&a, frame, (int)sizeof(frame));
    ASSERT_EQ(done, 1, "802.15.4g frame completes exactly once");
    ASSERT_EQ(a.subghz, 1, "802.15.4g frame flagged sub-GHz");
    ASSERT_EQ(a.subghz_phr_len, 1, "802.15.4g sniffed 1-byte PHR");
    ASSERT_EQ(a.expected_len, 8, "802.15.4g expected_len sticky");

    /* Does not complete before the 2 trailing CRC bytes. */
    sim_radio_bus_asm_reset(&a);
    int n = (int)sizeof(frame);
    for (int i = 0; i < n - 1; i++)
        ASSERT(!sim_radio_bus_asm_feed(&a, frame[i]), "g: no completion before CRC2");
    ASSERT(sim_radio_bus_asm_feed(&a, frame[n - 1]), "g: completes on 2nd CRC byte");
}

static void test_asm_ti_prop(void) {
    tx_frame_asm_t a;
    sim_radio_bus_asm_reset(&a);
    /* TI CC13x2 prop mode (Contiki's prop-settings.c): 3x0x55 preamble +
     * 24-bit sync 0x55904E + 2-byte PHR (FCS=CRC-32, length 6 = 2 payload
     * + 4 CRC) + payload + CRC, nothing after the CRC. */
    uint8_t frame[] = {
        0x55,0x55,0x55, 0x55,0x90,0x4E, 0x00,0x06,
        0xA1,0xA2, 0xC1,0xC2,0xC3,0xC4
    };
    int n = (int)sizeof(frame);
    for (int i = 0; i < n - 1; i++)
        ASSERT(!sim_radio_bus_asm_feed(&a, frame[i]), "ti: no completion before CRC4");
    ASSERT(sim_radio_bus_asm_feed(&a, frame[n - 1]), "ti: completes on the last CRC byte");
    ASSERT_EQ(a.subghz, 1, "ti: flagged sub-GHz");
    ASSERT_EQ(a.ti_prop, 1, "ti: flagged TI prop layout");
    ASSERT_EQ(a.expected_len, 6, "ti: PHR length counts the CRC");
    ASSERT_EQ(a.sync_bytes, 6, "ti: 3 preamble + 3 sync bytes before the PHR");

    /* The CC1200 layout on the same assembler afterwards is unaffected. */
    uint8_t cc[] = {
        0x55,0x55,0x55,0x55, 0x6E,0x4E,0x90,0x4E, 8,
        1,2,3,4,5,6,7,8, 0xC1,0xC2
    };
    sim_radio_bus_asm_reset(&a);
    ASSERT_EQ(feed_seq(&a, cc, (int)sizeof(cc)), 1, "ti: CC1200 frame still parses");
    ASSERT_EQ(a.ti_prop, 0, "ti: CC1200 frame not flagged TI");

    uint8_t fifo[] = { 0x55,0x55,0x55, 0x55,0x90,0x4E, 0x00,0x06, 0xA1,0xA2 };
    ASSERT_EQ(frame_fifo_bytes(fifo, (int)sizeof(fifo), true), 8,
              "ti: fifo = PHR length + 2");
}

static void test_asm_invalid_length_resets(void) {
    tx_frame_asm_t a;
    sim_radio_bus_asm_reset(&a);
    /* len=0 is invalid (<3) → assembler resets, then a valid frame on
     * the same stream still parses. */
    uint8_t bad[] = { 0,0,0,0, 0x7A, 0 };
    feed_seq(&a, bad, (int)sizeof(bad));
    ASSERT_EQ(a.state, TX_ASM_PREAMBLE, "invalid length returns to PREAMBLE");

    uint8_t good[] = { 0,0,0,0, 0x7A, 5, 1,2,3,4,5 };
    int done = feed_seq(&a, good, (int)sizeof(good));
    ASSERT_EQ(done, 1, "valid frame parses after an invalid-length reset");
}

static void test_asm_reset_clears(void) {
    tx_frame_asm_t a;
    /* Garbage in, then reset clears every field. */
    memset(&a, 0xAB, sizeof(a));
    sim_radio_bus_asm_reset(&a);
    ASSERT_EQ(a.state, TX_ASM_PREAMBLE, "reset state");
    ASSERT_EQ(a.zero_count, 0, "reset zero_count");
    ASSERT_EQ(a.sync_match, 0, "reset sync_match");
    ASSERT_EQ(a.subghz, 0, "reset subghz");
    ASSERT_EQ(a.first_byte_ns, 0, "reset first_byte_ns");
}

/* ============================================================
 * Pure helpers.
 * ============================================================ */

static void test_frame_fifo_bytes(void) {
    uint8_t f24[] = { 0,0,0,0, 0x7A, 20, 0,0,0 };
    ASSERT_EQ(frame_fifo_bytes(f24, (int)sizeof(f24), false), 21,
              "2.4GHz fifo = len+1");
    ASSERT_EQ(frame_fifo_bytes(f24, 5, false), -1,
              "2.4GHz too-short buffer → -1 (unparseable)");

    uint8_t fg[] = { 0x55,0x55,0x55,0x55, 0x6E,0x4E,0x90,0x4E, 30, 0,0 };
    ASSERT_EQ(frame_fifo_bytes(fg, (int)sizeof(fg), true), 33,
              "sub-GHz fifo = phr+3");
    ASSERT_EQ(frame_fifo_bytes(fg, 8, true), -1,
              "sub-GHz too-short buffer → -1 (unparseable)");
}

/* ============================================================
 * Capture: sim_radio_frame_capture (pcap, UI packet list, [PKT]).
 * Output is [PHY length = MAC + 2][MAC frame][802.15.4 FCS, LSB first].
 * ============================================================ */

/* "123456789" is the CRC catalogue's check string; the 802.15.4 FCS is
 * CRC-16/KERMIT, whose check value is 0x2189, sent 89 21. */
static const uint8_t crc_check[9] = { '1','2','3','4','5','6','7','8','9' };

/* KERMIT's residue over a frame followed by its own FCS is zero: a second,
 * independent way to say the FCS fits whatever MAC bytes were copied. */
static bool fcs_residue_zero(const uint8_t *p, int n) {
    uint16_t crc = 0;
    for (int i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
    return crc == 0;
}

/* CC1200 air bytes: 4x0x55, sync 6E4E904E, the PHR (1 or 2 bytes), the MAC
 * frame, two placeholder bytes where a CRC would be. */
static int cc1200_air(uint8_t *buf, const uint8_t *phr, int phr_n,
                      const uint8_t *mac, int mac_n) {
    static const uint8_t hdr[8] = { 0x55,0x55,0x55,0x55, 0x6E,0x4E,0x90,0x4E };
    int n = 0;
    memcpy(buf, hdr, 8); n = 8;
    memcpy(buf + n, phr, (size_t)phr_n); n += phr_n;
    memcpy(buf + n, mac, (size_t)mac_n); n += mac_n;
    buf[n++] = 0xEE; buf[n++] = 0xEE;   /* not a CRC: the model sends none */
    return n;
}

/* TI prop-mode air bytes: `pre` x 0x55, sync 55 90 4E, 2-byte PHR (FCS
 * type bit for CRC-16, else CRC-32; length counts the CRC), MAC, CRC. */
static int ti_air(uint8_t *buf, int pre, bool crc16, const uint8_t *mac, int mac_n) {
    int n = 0, crc_n = crc16 ? 2 : 4, phr = mac_n + crc_n;
    for (int i = 0; i < pre; i++) buf[n++] = 0x55;
    buf[n++] = 0x55; buf[n++] = 0x90; buf[n++] = 0x4E;
    buf[n++] = (uint8_t)((crc16 ? 0x10 : 0x00) | ((phr >> 8) & 0x07));
    buf[n++] = (uint8_t)(phr & 0xFF);
    memcpy(buf + n, mac, (size_t)mac_n); n += mac_n;
    for (int i = 0; i < crc_n; i++) buf[n++] = 0xC0 + (uint8_t)i;  /* TI's CRC, replaced */
    return n;
}

/* Check one capture: length byte, the MAC bytes copied verbatim, a valid
 * FCS after them. */
static void check_capture(const uint8_t *out, int got, const uint8_t *mac,
                          int mac_n, const char *what) {
    char msg[128];
    snprintf(msg, sizeof msg, "%s: bytes written = MAC + 3", what);
    ASSERT_EQ(got, mac_n + 3, msg);
    if (got != mac_n + 3) return;
    snprintf(msg, sizeof msg, "%s: PHY length = MAC + FCS", what);
    ASSERT_EQ(out[0], mac_n + 2, msg);
    snprintf(msg, sizeof msg, "%s: MAC frame copied verbatim", what);
    ASSERT(memcmp(out + 1, mac, (size_t)mac_n) == 0, msg);
    snprintf(msg, sizeof msg, "%s: FCS valid over the MAC frame", what);
    ASSERT(fcs_residue_zero(out + 1, mac_n + 2), msg);
}

static void test_capture_24ghz_passthrough(void) {
    /* 2.4 GHz: the bytes from the length byte on, unchanged, even with
     * 0x7A inside the frame (an IPHC dispatch) and an FCS that is not
     * recomputed. */
    uint8_t air[] = { 0,0,0,0, 0x7A, 9, 0x41,0xD8,0x01, 0x7A,0x33, 0x3A,0x7A, 0x12,0x34 };
    uint8_t out[64];
    int got = sim_radio_frame_capture(air, (int)sizeof air, false, 0, out, (int)sizeof out);
    ASSERT_EQ(got, (int)sizeof air - 5, "cap 2.4: from the length byte on");
    ASSERT(got > 0 && memcmp(out, air + 5, (size_t)got) == 0,
           "cap 2.4: byte-for-byte, FCS untouched");
    ASSERT_EQ(sim_radio_frame_capture(air, (int)sizeof air, false, 0, out, 5), -1,
              "cap 2.4: output buffer too small -> -1");
    uint8_t nosfd[] = { 0,0,0,0, 0x00, 9, 1,2,3 };
    ASSERT_EQ(sim_radio_frame_capture(nosfd, (int)sizeof nosfd, false, 0, out, (int)sizeof out),
              -1, "cap 2.4: no SFD -> -1");
}

static void test_capture_cc1200_1byte_phr(void) {
    uint8_t air[300], out[300];
    /* The check string: the placeholder CRC is replaced by the FCS 89 21. */
    uint8_t phr = 9;
    int n = cc1200_air(air, &phr, 1, crc_check, 9);
    int got = sim_radio_frame_capture(air, n, true, 1, out, (int)sizeof out);
    check_capture(out, got, crc_check, 9, "cap cc1200");
    ASSERT(got == 12 && out[10] == 0x89 && out[11] == 0x21,
           "cap cc1200: FCS of the check string is 89 21 (CRC-16/KERMIT)");

    /* 0x7A inside the frame (IPHC 7a 33, a 0x7A source address byte) is
     * just data: the frame starts after the length byte, not at a 0x7A. */
    uint8_t mac[] = { 0x41,0xD8,0x05, 0xCD,0xAB, 0xFF,0xFF, 0x7A,0x7A,
                      0x7A,0x33, 0x3A, 0x7A, 0x00, 0x01 };
    phr = (uint8_t)sizeof mac;
    n = cc1200_air(air, &phr, 1, mac, (int)sizeof mac);
    got = sim_radio_frame_capture(air, n, true, 1, out, (int)sizeof out);
    check_capture(out, got, mac, (int)sizeof mac, "cap cc1200 embedded 0x7A");

    /* The same frame with the PHR width unknown (0, e.g. never set) reads
     * it as the 1-byte layout too. */
    got = sim_radio_frame_capture(air, n, true, 0, out, (int)sizeof out);
    check_capture(out, got, mac, (int)sizeof mac, "cap cc1200 phr_len 0");

    /* Truncated: the length byte promises more than the air holds. */
    ASSERT_EQ(sim_radio_frame_capture(air, 9 + 5, true, 1, out, (int)sizeof out), -1,
              "cap cc1200: truncated frame -> -1");
    /* A zero length is no frame. */
    phr = 0;
    n = cc1200_air(air, &phr, 1, mac, 0);
    ASSERT_EQ(sim_radio_frame_capture(air, n, true, 1, out, (int)sizeof out), -1,
              "cap cc1200: zero length -> -1");
    /* Output buffer too small for MAC + length byte + FCS. */
    phr = 9;
    n = cc1200_air(air, &phr, 1, crc_check, 9);
    ASSERT_EQ(sim_radio_frame_capture(air, n, true, 1, out, 11), -1,
              "cap cc1200: output buffer too small -> -1");
    ASSERT_EQ(sim_radio_frame_capture(air, n, true, 1, out, 12), 12,
              "cap cc1200: output buffer of exactly MAC + 3 is enough");
}

static void test_capture_cc1200_2byte_phr(void) {
    /* CC1200 in 802.15.4g mode (PKT_CFG2 FG_MODE): a 2-byte PHR whose
     * length counts the CRC, as Contiki's cc1200 driver writes it
     * (copy_header_to_tx_fifo: payload_len += 2 with the CRC-16 bit 0x10,
     * += 4 without).  The assembler decides the width; the capture must
     * follow it rather than take the first PHR byte as a length. */
    uint8_t air[300], out[300];

    /* PHR 00 32: CRC-32 type, length 50 = 46-byte MAC + 4.  The first PHR
     * byte is no 1-byte length (0 < 3), so the assembler goes 2-byte. */
    uint8_t mac46[46];
    for (int i = 0; i < 46; i++) mac46[i] = (uint8_t)(i == 9 ? 0x7A : 0x60 + i);
    uint8_t phr32[2] = { 0x00, 50 };
    int n = cc1200_air(air, phr32, 2, mac46, 46);
    tx_frame_asm_t a;
    sim_radio_bus_asm_reset(&a);
    for (int i = 0; i < n; i++) sim_radio_bus_asm_feed(&a, air[i]);
    ASSERT_EQ(a.subghz_phr_len, 2, "cap cc1200g: PHR 00 32 sniffed as 2 bytes");
    ASSERT_EQ(a.expected_len, 50, "cap cc1200g: 11-bit PHR length");
    int got = sim_radio_frame_capture(air, n, true, a.subghz_phr_len, out, (int)sizeof out);
    check_capture(out, got, mac46, 46, "cap cc1200g CRC-32");

    /* PHR 10 0B: CRC-16 type, length 11 = the 9-byte check string + 2. */
    uint8_t phr16[2] = { 0x10, 11 };
    n = cc1200_air(air, phr16, 2, crc_check, 9);
    got = sim_radio_frame_capture(air, n, true, 2, out, (int)sizeof out);
    check_capture(out, got, crc_check, 9, "cap cc1200g CRC-16");
    ASSERT(got == 12 && out[10] == 0x89 && out[11] == 0x21,
           "cap cc1200g: FCS 89 21");

    /* Length high bits: PHR 11 2C = 0x12C = 300, more than the bus
     * carries (RADIO_BUS_SUBGHZ_MAX_LEN) -> no frame. */
    uint8_t phr_big[2] = { 0x11, 0x2C };
    n = cc1200_air(air, phr_big, 2, mac46, 46);
    ASSERT_EQ(sim_radio_frame_capture(air, n, true, 2, out, (int)sizeof out), -1,
              "cap cc1200g: 300-byte PHR length -> -1");
    /* A PHR that leaves nothing after the CRC is no frame. */
    uint8_t phr_crc_only[2] = { 0x10, 2 };
    n = cc1200_air(air, phr_crc_only, 2, mac46, 0);
    ASSERT_EQ(sim_radio_frame_capture(air, n, true, 2, out, (int)sizeof out), -1,
              "cap cc1200g: CRC-only PHR -> -1");
}

static void test_capture_ti_prop(void) {
    uint8_t air[300], out[300];
    /* CRC-16 (FCS type bit set) and CRC-32 (clear, Contiki's prop-mode
     * configs): TI's CRC bytes are dropped and the 802.15.4 FCS written. */
    int n = ti_air(air, 3, true, crc_check, 9);
    int got = sim_radio_frame_capture(air, n, true, 0, out, (int)sizeof out);
    check_capture(out, got, crc_check, 9, "cap ti CRC-16");
    ASSERT(got == 12 && out[10] == 0x89 && out[11] == 0x21, "cap ti CRC-16: FCS 89 21");

    n = ti_air(air, 3, false, crc_check, 9);
    got = sim_radio_frame_capture(air, n, true, 0, out, (int)sizeof out);
    check_capture(out, got, crc_check, 9, "cap ti CRC-32");
    ASSERT(got == 12 && out[10] == 0x89 && out[11] == 0x21, "cap ti CRC-32: FCS 89 21");

    /* The PHR width argument is the CC1200's; a TI frame is known by its
     * sync and ignores it.  Preamble length does not matter either. */
    n = ti_air(air, 7, false, crc_check, 9);
    got = sim_radio_frame_capture(air, n, true, 1, out, (int)sizeof out);
    check_capture(out, got, crc_check, 9, "cap ti long preamble, phr_len 1");

    /* 0x7A in the frame is data. */
    uint8_t mac[] = { 0x41,0xD8,0x07, 0xCD,0xAB, 0xFF,0xFF, 0x7A,0x33,0x3A, 0x7A };
    n = ti_air(air, 3, false, mac, (int)sizeof mac);
    got = sim_radio_frame_capture(air, n, true, 0, out, (int)sizeof out);
    check_capture(out, got, mac, (int)sizeof mac, "cap ti embedded 0x7A");

    /* Truncated mid-frame, and a CRC-only PHR. */
    ASSERT_EQ(sim_radio_frame_capture(air, n - 6, true, 0, out, (int)sizeof out), -1,
              "cap ti: truncated frame -> -1");
    n = ti_air(air, 3, false, mac, 0);
    ASSERT_EQ(sim_radio_frame_capture(air, n, true, 0, out, (int)sizeof out), -1,
              "cap ti: CRC-only PHR -> -1");
    /* Neither sync word: no frame. */
    uint8_t junk[] = { 0x55,0x55,0x55,0x55, 0x12,0x34,0x56,0x78, 9, 1,2,3,4,5,6,7,8,9 };
    ASSERT_EQ(sim_radio_frame_capture(junk, (int)sizeof junk, true, 1, out, (int)sizeof out),
              -1, "cap sub-GHz: no sync word -> -1");
}

static void test_capture_subghz_long_frames(void) {
    /* Sub-GHz frames may be longer than 2.4 GHz's 125-byte MAC: the bus
     * carries PHR lengths up to RADIO_BUS_SUBGHZ_MAX_LEN (200), and the
     * capture keeps every frame the bus accepts. */
    uint8_t air[300], out[300], mac[RADIO_BUS_SUBGHZ_MAX_LEN];
    for (int i = 0; i < (int)sizeof mac; i++) mac[i] = (uint8_t)(i * 7 + 1);
    mac[0] = 0x41; mac[1] = 0xD8;

    /* TI CRC-32, 126-byte MAC: PHR length 130. */
    int n = ti_air(air, 3, false, mac, 126);
    tx_frame_asm_t a;
    sim_radio_bus_asm_reset(&a);
    int done = 0;
    for (int i = 0; i < n; i++) done += sim_radio_bus_asm_feed(&a, air[i]);
    ASSERT_EQ(done, 1, "cap long: assembler accepts a 130-byte TI PHR");
    int got = sim_radio_frame_capture(air, n, true, 0, out, (int)sizeof out);
    check_capture(out, got, mac, 126, "cap long ti 126");

    /* TI at the bus's limit: PHR 200 with CRC-16 = 198-byte MAC. */
    n = ti_air(air, 3, true, mac, 198);
    got = sim_radio_frame_capture(air, n, true, 0, out, (int)sizeof out);
    check_capture(out, got, mac, 198, "cap long ti 198");

    /* CC1200 1-byte PHR 150 and the 200 limit; 201 is no frame. */
    uint8_t phr = 150;
    n = cc1200_air(air, &phr, 1, mac, 150);
    got = sim_radio_frame_capture(air, n, true, 1, out, (int)sizeof out);
    check_capture(out, got, mac, 150, "cap long cc1200 150");
    phr = RADIO_BUS_SUBGHZ_MAX_LEN;
    n = cc1200_air(air, &phr, 1, mac, RADIO_BUS_SUBGHZ_MAX_LEN);
    got = sim_radio_frame_capture(air, n, true, 1, out, (int)sizeof out);
    check_capture(out, got, mac, RADIO_BUS_SUBGHZ_MAX_LEN, "cap long cc1200 200");
    uint8_t big[RADIO_BUS_SUBGHZ_MAX_LEN + 1];
    memset(big, 0x5A, sizeof big);
    phr = (uint8_t)sizeof big;
    n = cc1200_air(air, &phr, 1, big, (int)sizeof big);
    ASSERT_EQ(sim_radio_frame_capture(air, n, true, 1, out, (int)sizeof out), -1,
              "cap long cc1200: 201 bytes, past the bus limit -> -1");
}

static void test_byte_period(void) {
    ASSERT_EQ(byte_period_ns(false), 32000, "2.4GHz byte period 32µs");
    ASSERT_EQ(byte_period_ns(true), 160000, "sub-GHz byte period 160µs");
}

static void test_pick_receiver_radio(void) {
    radio_medium_t rm;
    radio_medium_init(&rm, 4);

    /* Unregistered sender slot → legacy "target receiver slot 0". */
    ASSERT_EQ(sim_radio_bus_pick_receiver_radio(&rm, 0, 0, 1), 0,
              "unregistered sender → slot 0");

    /* Registered sender 2.4GHz, receiver 2.4GHz on slot 0 → slot 0. */
    radio_medium_register_radio(&rm, 0, 0, RADIO_SPECTRUM_2_4GHZ_15_4);
    radio_medium_register_radio(&rm, 1, 0, RADIO_SPECTRUM_2_4GHZ_15_4);
    ASSERT_EQ(sim_radio_bus_pick_receiver_radio(&rm, 0, 0, 1), 0,
              "matching 2.4GHz → receiver slot 0");

    /* Receiver with sub-GHz on slot 0 and 2.4GHz on slot 1 → picks slot 1. */
    radio_medium_register_radio(&rm, 2, 0, RADIO_SPECTRUM_868MHZ_15_4G);
    radio_medium_register_radio(&rm, 2, 1, RADIO_SPECTRUM_2_4GHZ_15_4);
    ASSERT_EQ(sim_radio_bus_pick_receiver_radio(&rm, 0, 0, 2), 1,
              "2.4GHz sender → receiver's 2.4GHz slot 1");

    /* Receiver registered sub-GHz only (slot 0), no 2.4GHz → drop (-1). */
    radio_medium_register_radio(&rm, 3, 0, RADIO_SPECTRUM_868MHZ_15_4G);
    ASSERT_EQ(sim_radio_bus_pick_receiver_radio(&rm, 0, 0, 3), -1,
              "no matching spectrum on registered receiver → drop");
}

/* ============================================================
 * Delivery-mode routing + byte clock + capture.
 * ============================================================ */

/* Build a minimal valid 802.15.4 frame into buf, return length. */
static int build_802154(uint8_t *buf, int payload_len) {
    int n = 0;
    buf[n++] = 0; buf[n++] = 0; buf[n++] = 0; buf[n++] = 0;
    buf[n++] = 0x7A;
    buf[n++] = (uint8_t)payload_len;
    for (int i = 0; i < payload_len; i++) buf[n++] = (uint8_t)(0x10 + i);
    return n;
}

static void test_delivery_sync(void) {
    /* SYNC receiver gets bytes synchronously, during tx_byte. */
    fixture_t f;
    fx_init(&f, 2);
    sim_radio_bus_register(&f.bus, 0, &mock_ops, &f.rx[0], SIM_RADIO_DELIVERY_PER_BYTE, 0);
    sim_radio_bus_register(&f.bus, 1, &mock_ops, &f.rx[1], SIM_RADIO_DELIVERY_SYNC, 0);

    uint8_t frame[64];
    int n = build_802154(frame, 6);
    for (int i = 0; i < n; i++)
        sim_radio_bus_tx_byte(&f.bus, &f.sim, 0, 0, frame[i]);

    ASSERT_EQ(f.rx[1].recv_count, n, "SYNC receiver got all bytes synchronously");
    ASSERT(sim_eq_empty(&f.sim.event_queue), "SYNC schedules no RX_BYTE events");
}

static void test_delivery_per_byte(void) {
    /* PER_BYTE receiver gets nothing until the queue is pumped; events
     * land at 32µs byte-clock spacing from first_byte_ns. */
    fixture_t f;
    fx_init(&f, 2);
    sim_radio_bus_register(&f.bus, 0, &mock_ops, &f.rx[0], SIM_RADIO_DELIVERY_PER_BYTE, 0);
    sim_radio_bus_register(&f.bus, 1, &mock_ops, &f.rx[1], SIM_RADIO_DELIVERY_PER_BYTE, 0);

    f.sim.now_ns = 0;
    uint8_t frame[64];
    int n = build_802154(frame, 6);
    for (int i = 0; i < n; i++)
        sim_radio_bus_tx_byte(&f.bus, &f.sim, 0, 0, frame[i]);

    ASSERT_EQ(f.rx[1].recv_count, 0, "PER_BYTE: nothing delivered before pump");
    ASSERT(!sim_eq_empty(&f.sim.event_queue), "PER_BYTE scheduled RX_BYTE events");

    fx_pump(&f, 100 * 1000000LL);
    ASSERT_EQ(f.rx[1].recv_count, n, "PER_BYTE: all bytes after pump");

    bool spacing_ok = true;
    for (int i = 1; i < f.rx[1].recv_count; i++)
        if (f.rx[1].recv_time[i] - f.rx[1].recv_time[i - 1] != IEEE802154_BYTE_NS)
            spacing_ok = false;
    ASSERT(spacing_ok, "PER_BYTE: 32µs byte-clock spacing");
    ASSERT_EQ(f.rx[1].recv_time[0], 0, "PER_BYTE: first byte at first_byte_ns");
}

static void test_delivery_batch(void) {
    /* BATCH receiver: bytes accumulate in rf_pending during dispatch,
     * then frame_complete (M27) snapshots and delivers the whole frame
     * to the chip — rf_pending ends empty. */
    fixture_t f;
    fx_init(&f, 2);
    mock_mote_t mm;
    sim_radio_bus_register(&f.bus, 0, &mock_ops, &f.rx[0], SIM_RADIO_DELIVERY_PER_BYTE, 0);
    /* node 1 = BATCH receiver with a registered sim_mote (the snapshot
     * delivery drives its rx_byte_sync/step ops). */
    memset(&mm, 0, sizeof(mm));
    mm.freq = 1000000; mm.mote.impl = &mm; mm.mote.ops = &mm_ops;
    sim_runtime_register_mote(&f.sim, 1, &mm.mote);
    sim_radio_bus_register(&f.bus, 1, &mock_ops, &f.rx[1], SIM_RADIO_DELIVERY_BATCH, 0);
    f.bus.executing_node = -1;

    uint8_t frame[64];
    int n = build_802154(frame, 6);   /* expected_len 6 → total_air_bytes == n */
    for (int i = 0; i < n; i++)
        sim_radio_bus_tx_byte(&f.bus, &f.sim, 0, 0, frame[i]);

    ASSERT_EQ(f.bus.rf_pending[1].count, 0,
              "BATCH: staged frame consumed at frame-complete");
    ASSERT_EQ(f.rx[1].recv_count, n, "BATCH: whole frame delivered to the chip");
    ASSERT_EQ(f.bus.stats.rx_direct, 1, "BATCH: counted as a direct delivery");
}

static void test_capture_and_first_byte(void) {
    fixture_t f;
    fx_init(&f, 2);
    sim_radio_bus_register(&f.bus, 0, &mock_ops, &f.rx[0], SIM_RADIO_DELIVERY_PER_BYTE, 0);
    sim_radio_bus_register(&f.bus, 1, &mock_ops, &f.rx[1], SIM_RADIO_DELIVERY_PER_BYTE, 0);

    f.sim.now_ns = 5000;  /* first preamble byte arms first_byte_ns = now */
    uint8_t frame[64];
    int n = build_802154(frame, 4);
    for (int i = 0; i < n; i++)
        sim_radio_bus_tx_byte(&f.bus, &f.sim, 0, 0, frame[i]);

    ASSERT_EQ(f.bus.tx_cap[0].len, n, "capture length = frame length");
    ASSERT_EQ(f.bus.tx_asm[0].first_byte_ns, 5000, "first_byte_ns = now at preamble");
    bool bytes_ok = (memcmp(f.bus.tx_cap[0].bytes, frame, (size_t)n) == 0);
    ASSERT(bytes_ok, "capture bytes match the on-air frame");
}

/* ============================================================
 * Byte-clock arming: once per frame, at its first preamble byte, with the
 * frame's own byte period (F17 in kernel-radio-review-and-performance-
 * plan.md).  The clock is read back through the host's on_tx_byte hook,
 * which gets every byte's on-air time.
 * ============================================================ */

static int64_t arm_times[128];
static int     arm_count;
static void arm_on_tx_byte(void *user, int sender, uint8_t byte,
                           int64_t byte_time_ns, int depth) {
    (void)user; (void)sender; (void)byte; (void)depth;
    if (arm_count < (int)(sizeof arm_times / sizeof arm_times[0]))
        arm_times[arm_count++] = byte_time_ns;
}

/* Send seq from node 0, the sender's clock advancing `step` ns per byte
 * from `start` (a chip pushing bytes as it goes); true if every byte's
 * on-air time is start + i * period. */
static bool arm_send_and_check(fixture_t *f, const uint8_t *seq, int n,
                               int64_t start, int64_t step, int64_t period) {
    arm_count = 0;
    for (int i = 0; i < n; i++) {
        f->sim.now_ns = start + (int64_t)i * step;
        sim_radio_bus_tx_byte(&f->bus, &f->sim, 0, 0, seq[i]);
    }
    if (arm_count != n) return false;
    for (int i = 0; i < n; i++)
        if (arm_times[i] != start + (int64_t)i * period) return false;
    return true;
}

static void test_byte_clock_arming(void) {
    fixture_t f;
    fx_init(&f, 2);
    sim_radio_bus_register(&f.bus, 0, &mock_ops, &f.rx[0], SIM_RADIO_DELIVERY_PER_BYTE, 0);
    sim_radio_bus_register(&f.bus, 1, &mock_ops, &f.rx[1], SIM_RADIO_DELIVERY_PER_BYTE, 0);
    f.bus.host.on_tx_byte = arm_on_tx_byte;
    const int64_t g = byte_period_ns(true), b = byte_period_ns(false);

    /* A CC1200 frame: 4×0x55 preamble, sync word, 1-byte PHR, payload, CRC. */
    const uint8_t sub[] = { 0x55,0x55,0x55,0x55, 0x6E,0x4E,0x90,0x4E, 8,
                            1,2,3,4,5,6,7,8, 0xC1,0xC2 };
    const int nsub = (int)sizeof sub;

    /* Armed once, at the first 0x55 — not again on the next three — and
     * timed at the sub-GHz period from the first byte, sync word included.
     * The node's first sub-GHz frame used to run its first 8 bytes at the
     * 2.4 GHz period. */
    ASSERT(arm_send_and_check(&f, sub, nsub, 1000000, 7, g),
           "sub-GHz frame: armed once at its first preamble byte, 160 us apart");
    ASSERT_EQ(f.bus.tx_cap[0].len, nsub, "sub-GHz capture keeps the whole preamble");
    /* The next frame arms at its own start. */
    ASSERT(arm_send_and_check(&f, sub, nsub, 50000000, 0, g),
           "second sub-GHz frame armed at its own first byte");

    /* A 2.4 GHz frame after a sub-GHz one from the same node (dual-band
     * Firefly) runs at the 2.4 GHz period from its first byte. */
    uint8_t ieee[32];
    int nieee = build_802154(ieee, 6);
    ASSERT(arm_send_and_check(&f, ieee, nieee, 90000000, 0, b),
           "2.4 GHz frame after sub-GHz: 32 us from the first byte");

    /* A rejected length (here 0x80, above 127), with the rest of the bad
     * frame still coming: the next frame arms at its own first byte. */
    const uint8_t rejected[] = { 0,0,0,0, 0x7A, 0x80, 0x11, 0x22 };
    arm_send_and_check(&f, rejected, (int)sizeof rejected, 100000000, 0, b);
    ASSERT(arm_send_and_check(&f, ieee, nieee, 150000000, 0, b),
           "frame after a length reject armed at its own first byte");
    ASSERT_EQ(f.bus.tx_cap[0].len, nieee, "capture restarted for that frame");

    /* A sub-GHz frame longer than its PHR says: the extra bytes trail into
     * PREAMBLE, and the next frame still arms at its own start. */
    const uint8_t longer[] = { 0x55,0x55,0x55,0x55, 0x6E,0x4E,0x90,0x4E, 5,
                               1,2,3,4,5, 0xC1,0xC2, 0x99, 0x00, 0x00 };
    arm_send_and_check(&f, longer, (int)sizeof longer, 120000000, 0, g);
    ASSERT(arm_send_and_check(&f, sub, nsub, 130000000, 0, g),
           "frame after trailing bytes armed at its own first byte");

    /* The same after a rejected 802.15.4g PHR. */
    const uint8_t badphr[] = { 0x55,0x55,0x55,0x55, 0x6E,0x4E,0x90,0x4E, 0xFF, 0xFF };
    arm_send_and_check(&f, badphr, (int)sizeof badphr, 200000000, 0, g);
    ASSERT(arm_send_and_check(&f, sub, nsub, 250000000, 0, g),
           "frame after a PHR reject armed at its own first byte");
}

/* ============================================================
 * Re-entrant depth staging (auto-ACK during synchronous delivery).
 * ============================================================ */

static int reentry_frames_completed;
/* M27: the frame_complete host hook became the bus-internal
 * sim_radio_bus_frame_complete; the re-entrancy test now counts frames
 * via the frame_observed notification (fired once per completed frame). */
static void reentry_frame_observed(void *user, const sim_radio_frame_info_t *fi) {
    (void)user; (void)fi;
    reentry_frames_completed++;
}

static void test_reentrant_depth(void) {
    /* A SYNC receiver re-enters tx_byte (emits an "ACK" byte) the moment
     * it is fed a trigger byte.  The re-entrant byte must be dispatched
     * to other receivers but never fed to the sender's assembler — i.e.
     * it must not advance / corrupt the in-flight frame parse, and the
     * outer frame still completes exactly once. */
    fixture_t f;
    fx_init(&f, 3);
    reentry_frames_completed = 0;
    sim_radio_bus_host_t host = { 0 };
    host.frame_observed = reentry_frame_observed;
    sim_radio_bus_set_host(&f.bus, &host);

    /* node 0 = the sender (PER_BYTE), node 1 = a SYNC receiver that emits
     * an ACK byte the moment it is fed the SFD, node 2 = a PER_BYTE
     * bystander that should also receive the re-entrant ACK byte. */
    sim_radio_bus_register(&f.bus, 0, &mock_ops, &f.rx[0], SIM_RADIO_DELIVERY_PER_BYTE, 0);
    sim_radio_bus_register(&f.bus, 1, &mock_ops, &f.rx[1], SIM_RADIO_DELIVERY_SYNC, 0);
    sim_radio_bus_register(&f.bus, 2, &mock_ops, &f.rx[2], SIM_RADIO_DELIVERY_PER_BYTE, 0);
    f.bus.executing_node = -1;
    f.rx[1].reenter_on = 0x7A;
    f.rx[1].reenter_sender = 1;

    uint8_t frame[64];
    int n = build_802154(frame, 5);
    for (int i = 0; i < n; i++)
        sim_radio_bus_tx_byte(&f.bus, &f.sim, 0, 0, frame[i]);

    /* The outer frame completes exactly once: the re-entrant ACK byte
     * (0xAC) is dispatched to receivers but never fed to a sender's
     * assembler in a way that closes a second frame. */
    ASSERT_EQ(reentry_frames_completed, 1,
              "outer frame completes exactly once despite re-entrant ACK byte");

    /* The re-entrant 0xAC was dispatched as sender 1 → scheduled to the
     * PER_BYTE bystander; pump and confirm it arrived. */
    fx_pump(&f, 100 * 1000000LL);
    bool saw_ack = false;
    for (int i = 0; i < f.rx[2].recv_count; i++)
        if (f.rx[2].recv_byte[i] == 0xAC) saw_ack = true;
    ASSERT(saw_ack, "re-entrant ACK byte dispatched to other receivers");
}

/* ============================================================
 * RX-stall timer (M9.5).
 * ============================================================ */

static void test_rx_stall(void) {
    /* fixture_t embeds sim_runtime_t + sim_radio_bus_t (~5 MB each); this is the
     * only test that needs two live at once, and two on the stack overflow an
     * 8 MB stack (segfaults on macOS and smaller ulimits; survived the Linux CI
     * runner by luck). Keep them in static storage — fx_init re-inits each. */
    static fixture_t f;
    fx_init(&f, 2);
    sim_radio_bus_register(&f.bus, 0, &mock_ops, &f.rx[0], SIM_RADIO_DELIVERY_PER_BYTE, 0);
    /* Receiver with an rx_stall op → the bus arms the watchdog. */
    sim_radio_bus_register(&f.bus, 1, &mock_ops_stall, &f.rx[1], SIM_RADIO_DELIVERY_PER_BYTE, 0);

    f.sim.now_ns = 0;
    /* Deliver three bytes; last air time = 2 * 32µs = 64000. */
    for (int b = 0; b < 3; b++)
        sim_radio_bus_tx_byte(&f.bus, &f.sim, 0, 0, (uint8_t)(0xA0 + b));

    ASSERT(f.bus.rx_stall_pending[1], "rx_stall armed after PER_BYTE delivery");
    int64_t last_byte = 2 * IEEE802154_BYTE_NS;
    ASSERT_EQ(f.bus.rx_stall_deadline_ns[1], last_byte + SIM_RADIO_RX_STALL_NS,
              "deadline = last byte air time + 200µs");

    /* A timer that fires BEFORE the deadline is stale → re-arm, no stall. */
    bool expired_early =
        sim_radio_bus_rx_stall_expired(&f.bus, &f.sim, 1, last_byte);
    ASSERT(!expired_early, "early timer is stale (deadline extended)");
    ASSERT(f.bus.rx_stall_pending[1], "stale timer re-armed");

    /* A timer at/after the deadline truly expires. */
    bool expired =
        sim_radio_bus_rx_stall_expired(&f.bus, &f.sim, 1,
                                       last_byte + SIM_RADIO_RX_STALL_NS);
    ASSERT(expired, "timer at deadline truly expires");

    /* A receiver WITHOUT an rx_stall op never arms the watchdog. */
    static fixture_t g;          /* static: see the note on `f` above */
    fx_init(&g, 2);
    sim_radio_bus_register(&g.bus, 0, &mock_ops, &g.rx[0], SIM_RADIO_DELIVERY_PER_BYTE, 0);
    sim_radio_bus_register(&g.bus, 1, &mock_ops, &g.rx[1], SIM_RADIO_DELIVERY_PER_BYTE, 0);
    sim_radio_bus_tx_byte(&g.bus, &g.sim, 0, 0, 0x11);
    ASSERT(!g.bus.rx_stall_pending[1], "no rx_stall op → watchdog never armed");
}

/* ============================================================
 * reset_node clears per-node state.
 * ============================================================ */

static void test_reset_node(void) {
    fixture_t f;
    fx_init(&f, 2);
    sim_radio_bus_register(&f.bus, 0, &mock_ops, &f.rx[0], SIM_RADIO_DELIVERY_PER_BYTE, 0);
    sim_radio_bus_register(&f.bus, 1, &mock_ops_stall, &f.rx[1], SIM_RADIO_DELIVERY_PER_BYTE, 0);
    sim_radio_bus_tx_byte(&f.bus, &f.sim, 0, 0, 0x55);
    ASSERT(f.bus.rx_stall_pending[1], "armed before reset");
    sim_radio_bus_reset_node(&f.bus, 1);
    ASSERT(!f.bus.rx_stall_pending[1], "reset clears rx_stall_pending");
    ASSERT_EQ(f.bus.rx_stall_deadline_ns[1], 0, "reset clears rx_stall_deadline");
}

/* ============================================================
 * Emulated-RX delivery / queue / drain (M26).
 * ============================================================ */

static void test_deliver_direct(void) {
    /* Synchronous delivery: each byte is pre-synced to its air time and
     * handed to the chip; a wakeup is scheduled after the burst. */
    fixture_t f; fx_init(&f, 2);
    mock_mote_t mm;
    register_emulated(&f, &mm, 1, 0);
    f.bus.executing_node = -1;
    f.sim.now_ns = 0;

    uint8_t frame[64]; int n = short_frame(frame, 5);
    sim_radio_bus_deliver_bytes(&f.bus, &f.sim, 1, frame, n, -40, 0, false);

    ASSERT_EQ(f.rx[1].recv_count, n, "direct: all bytes reached the chip");
    ASSERT_EQ(mm.sync_count, n, "direct: one clock-sync per byte");
    bool spaced = true;
    for (int i = 1; i < mm.sync_count; i++)
        if (mm.sync_times[i] - mm.sync_times[i-1] != IEEE802154_BYTE_NS) spaced = false;
    ASSERT(spaced, "direct: per-byte sync on the 32µs air clock");
    ASSERT(!sim_eq_empty(&f.sim.event_queue), "direct: wakeup scheduled");
}

static void test_deliver_executing_node(void) {
    /* When the receiver is the executing node, skip per-byte sync; with
     * CAP_RX_TICKING_STEP the bus steps the CPU ~1 ms instead. */
    fixture_t f; fx_init(&f, 2);
    mock_mote_t mm;
    register_emulated(&f, &mm, 1, SIM_RADIO_CAP_RX_TICKING_STEP);
    f.bus.executing_node = 1;

    uint8_t frame[64]; int n = short_frame(frame, 4);
    sim_radio_bus_deliver_bytes(&f.bus, &f.sim, 1, frame, n, -40, 0, false);

    ASSERT_EQ(f.rx[1].recv_count, n, "executing: bytes still delivered");
    ASSERT_EQ(mm.sync_count, 0, "executing: no per-byte clock sync");
    ASSERT_EQ(mm.step_count, 1, "executing: CAP_RX_TICKING_STEP stepped once");
    ASSERT_EQ(mm.cyc, 1000, "executing: stepped ~1 ms (1000 cycles @ 1 MHz)");
}

static void test_queue_and_drain(void) {
    /* queue_frame stages frames FIFO; drain delivers one per call. */
    fixture_t f; fx_init(&f, 2);
    mock_mote_t mm;
    register_emulated(&f, &mm, 1, 0);
    f.bus.executing_node = -1;

    uint8_t a[64], b[64];
    int na = short_frame(a, 3), nb = short_frame(b, 4);
    a[6] = 0xA1; b[6] = 0xB1;   /* distinguishable first payload byte */
    sim_radio_bus_queue_frame(&f.bus, 1, a, na, -40, 0, 1000, false);
    sim_radio_bus_queue_frame(&f.bus, 1, b, nb, -40, 0, 1000, false);
    ASSERT_EQ(f.bus.emu_rx_queue[1].count, 2, "queue: two frames staged");
    ASSERT_EQ(f.bus.stats.rx_queued, 2, "queue: rx_queued counted");

    sim_radio_bus_drain_rx(&f.bus, &f.sim, 1);
    ASSERT_EQ(f.bus.emu_rx_queue[1].count, 1, "drain: one frame consumed");
    ASSERT_EQ(f.bus.stats.rx_drained, 1, "drain: rx_drained counted");
    ASSERT_EQ(f.rx[1].recv_byte[6], 0xA1, "drain: FIFO order (frame A first)");

    sim_radio_bus_drain_rx(&f.bus, &f.sim, 1);
    ASSERT_EQ(f.bus.emu_rx_queue[1].count, 0, "drain: queue empty");
    ASSERT_EQ(f.rx[1].recv_byte[na + 6], 0xB1, "drain: frame B second");
}

static void test_drain_collided_skip(void) {
    /* A collided frame is dropped without delivery; the next good frame
     * is delivered. */
    fixture_t f; fx_init(&f, 2);
    mock_mote_t mm;
    register_emulated(&f, &mm, 1, 0);
    f.bus.executing_node = -1;

    uint8_t a[64], b[64];
    int na = short_frame(a, 3), nb = short_frame(b, 3);
    b[6] = 0xB2;
    sim_radio_bus_queue_frame(&f.bus, 1, a, na, -40, 0, 1000, false);
    sim_radio_bus_queue_frame(&f.bus, 1, b, nb, -40, 0, 1000, false);
    /* Mark the head (frame A) collided. */
    f.bus.emu_rx_queue[1].frames[f.bus.emu_rx_queue[1].head].collided = true;

    sim_radio_bus_drain_rx(&f.bus, &f.sim, 1);
    ASSERT_EQ(f.bus.emu_rx_queue[1].count, 0, "drain: collided skipped + good drained");
    ASSERT_EQ(f.rx[1].recv_count, nb, "drain: only the good frame's bytes");
    ASSERT_EQ(f.rx[1].recv_byte[6], 0xB2, "drain: delivered frame B, not collided A");
}

static void test_drain_max_arrival(void) {
    /* A frame queued with a future arrival_ns is delivered at that time
     * (max(arrival, now)) so the ACK turnaround is preserved. */
    fixture_t f; fx_init(&f, 2);
    mock_mote_t mm;
    register_emulated(&f, &mm, 1, 0);
    f.bus.executing_node = -1;
    f.sim.now_ns = 1000;

    uint8_t a[64]; int na = short_frame(a, 2);
    int64_t future = 500000;   /* well past now */
    sim_radio_bus_queue_frame(&f.bus, 1, a, na, -40, future, 1000, false);
    sim_radio_bus_drain_rx(&f.bus, &f.sim, 1);
    ASSERT_EQ(mm.sync_times[0], future, "drain: delivered at max(arrival, now)");
}

/* A native mote's CCA reads the channel from on-air time alone: busy while
 * any transmission reaches the node, clear once the last one ends.  It used
 * to be raised on reception and lowered only when a frame was read, so a
 * collided frame, or one dropped because the radio turned off, left the
 * channel reading busy. */
static void test_native_signal_strength_follows_air(void) {
    native_node_t n;
    int in_size = 0;
    char receiving = 0;
    memset(&n, 0, sizeof(n));
    /* The receive path writes these through the firmware's symbols. */
    n.simInSize = &in_size;
    n.simReceiving = &receiving;

    ASSERT_EQ(native_radio_signal_strength(&n, 0), -100, "native: quiet channel reads clear");

    native_radio_mark_busy(&n, 5000);
    ASSERT_EQ(native_radio_signal_strength(&n, 4999), -60, "native: busy while a frame is on the air");
    ASSERT_EQ(native_radio_signal_strength(&n, 5000), -100, "native: clear once it ends, unread or not");

    native_radio_mark_busy(&n, 9000);
    native_radio_mark_busy(&n, 7000);
    ASSERT_EQ(native_radio_signal_strength(&n, 8000), -60, "native: a shorter frame does not end a longer one");
    ASSERT_EQ(native_radio_signal_strength(&n, 9000), -100, "native: clear when the last frame ends");

    /* Nothing in the receive path holds the channel busy past the air. */
    uint8_t frame[20] = {0};
    native_deliver_frame(&n, frame, (int)sizeof(frame), 10000, -1);
    native_radio_flush_rx(&n);
    ASSERT_EQ(native_radio_signal_strength(&n, 10000), -100, "native: queueing and flushing leave it alone");
}

static void tx_frame(fixture_t *f, int sender, const uint8_t *frame, int n);
static void reg_batch(fixture_t *f, mock_mote_t *mm, int idx, uint32_t caps);

/* ============================================================
 * On-air windows: the one busy window the bus announces to every mote a
 * transmission reaches (what a native mote's CCA reads).
 * ============================================================ */

/* Sender 0 at the origin on channel 26; nodes placed on the x axis.
 * UDGM with a 50 m reception range and a 100 m interference range. */
static void on_air_udgm(fixture_t *f, int nodes, const double *x,
                        const int *channel) {
    radio_medium_configure_udgm(&f->sim.radio_medium, 50.0, 100.0, 1.0, 1.0);
    for (int i = 0; i < nodes; i++) {
        radio_medium_set_position(&f->sim.radio_medium, i, x[i], 0.0);
        radio_medium_set_channel(&f->sim.radio_medium, i, channel[i]);
    }
    radio_medium_compute_neighbors(&f->sim.radio_medium);
}

static void test_on_air_emulated_sender(void) {
    /* An emulated sender's frame occupies the channel from its first
     * preamble byte, on the bus's byte clock -- not on the receiver's own
     * clock, which for a native mote is the time of its last tick. */
    fixture_t f; fx_init(&f, 2);
    mock_mote_t mm; register_emulated(&f, &mm, 0, 0);
    sim_radio_bus_register(&f.bus, 1, &mock_ops_on_air, &f.rx[1],
                           SIM_RADIO_DELIVERY_SYNC, 0);
    const int64_t t0 = 10000000;          /* 10 ms: long after any tick */
    f.sim.now_ns = t0;

    uint8_t frame[64]; int n = build_802154(frame, 20);
    sim_radio_bus_tx_byte(&f.bus, &f.sim, 0, 0, frame[0]);
    ASSERT_EQ(f.rx[1].on_air_count, 1, "on_air: announced at the first preamble byte");
    ASSERT_EQ(f.rx[1].on_air_end, t0 + 6 * IEEE802154_BYTE_NS,
              "on_air: covers the PHY header before the length is known");
    ASSERT_EQ(f.rx[1].recv_at_count, 1, "sync: the byte came with its air time");
    ASSERT_EQ(f.rx[1].recv_air_ns, t0, "sync: the first byte's air time is the bus clock");

    for (int i = 1; i < n; i++)
        sim_radio_bus_tx_byte(&f.bus, &f.sim, 0, 0, frame[i]);
    ASSERT_EQ(f.rx[1].on_air_end, t0 + (6 + 20) * IEEE802154_BYTE_NS,
              "on_air: ends with the frame's last byte");
    ASSERT_EQ(f.rx[1].on_air_count, 2,
              "on_air: announced once for the header, once for the frame -- not per byte");
    ASSERT_EQ(f.rx[1].recv_at_count, n, "sync: every byte came with its air time");
    ASSERT_EQ(f.rx[1].recv_air_ns, t0 + (n - 1) * IEEE802154_BYTE_NS,
              "sync: the last byte's air time is one byte before the window ends");
}

static void test_on_air_byte_path_radio_off(void) {
    /* A receiver the medium delivers nothing to (here: RX disabled) is
     * busy for the frame all the same: the window does not depend on a
     * byte having been accepted.  (The native mote's own radio-off state,
     * which the medium never sees, is test_native_mote_radio_off_reads_busy.) */
    fixture_t f; fx_init(&f, 2);
    const double x[2] = { 0, 30 };
    const int ch[2]   = { 26, 26 };
    on_air_udgm(&f, 2, x, ch);
    radio_medium_set_radio_rx_enabled(&f.sim.radio_medium, 1, 0, false);
    mock_mote_t mm; register_emulated(&f, &mm, 0, 0);
    sim_radio_bus_register(&f.bus, 1, &mock_ops_on_air, &f.rx[1],
                           SIM_RADIO_DELIVERY_SYNC, 0);
    f.sim.now_ns = 7000000;

    uint8_t frame[64]; int n = build_802154(frame, 10);
    tx_frame(&f, 0, frame, n);
    ASSERT_EQ(f.rx[1].recv_count + f.rx[1].recv_at_count, 0,
              "radio off: no byte delivered");
    ASSERT_EQ(f.rx[1].on_air_count, 2, "radio off: the window is still announced");
    ASSERT_EQ(f.rx[1].on_air_end, f.sim.now_ns + (6 + 10) * IEEE802154_BYTE_NS,
              "radio off: busy until the emulated sender's frame ends");
}

static void test_native_byte_frame_ends_on_bus_clock(void) {
    /* A frame assembled from an emulated sender's bytes is stamped on the
     * bus clock: reception starts at the SFD's air time and the frame ends
     * when its last byte leaves the air -- the instant the bus's on-air
     * window (this node's CCA) ends.  Before, the arrival was derived from
     * the node's last tick, so the frame was read while the channel it
     * came on still read busy. */
    native_node_t n;
    uint64_t last_ts = 0;
    char receiving = 0;
    memset(&n, 0, sizeof(n));
    n.simLastPacketTimestamp = &last_ts;
    n.simReceiving = &receiving;
    n.sim_time_ns = 1000000;              /* last tick: 1 ms */

    const int64_t t0 = 20000000;          /* first byte on the air: 20 ms */
    const int payload = 10;
    /* The PHY length counts the two FCS bytes: 6 header + 10 + 2 on the air. */
    uint8_t frame[64]; int len = build_802154(frame, payload + 2);
    int completed = 0;
    for (int i = 0; i < len; i++) {
        int64_t air = t0 + i * IEEE802154_BYTE_NS;
        if (native_rx_assembler_feed(&n, frame[i], air)) completed++;
    }
    ASSERT_EQ(completed, 1, "native byte path: one frame queued, on the last byte");
    ASSERT_EQ(receiving, 1, "native byte path: receiving from the SFD on");
    ASSERT_EQ(last_ts, (t0 + 4 * IEEE802154_BYTE_NS) / 1000, "native byte path: reception start = the SFD's air time");
    ASSERT_EQ(n.rx_queue.count, 1, "native byte path: frame queued");
    const native_pending_frame_t *q = &n.rx_queue.frames[n.rx_queue.head];
    ASSERT_EQ(q->len, payload, "native byte path: FCS stripped");
    ASSERT_EQ(q->end_ns, t0 + len * IEEE802154_BYTE_NS,
              "native byte path: the frame ends when its last byte leaves the air");
    ASSERT_EQ(q->end_ns, t0 + (6 + payload + 2) * IEEE802154_BYTE_NS,
              "native byte path: ... which is where the bus's on-air window ends");
    ASSERT_EQ(native_rx_next_end_ns(&n), q->end_ns, "native byte path: the node wakes at that end");
    ASSERT(q->end_ns > n.sim_time_ns, "native byte path: not completed on a stale tick");
}

static void test_on_air_reach_and_channel(void) {
    /* A native sender's frame: the reception and the interference
     * neighbours on the sender's channel get the same window; a neighbour
     * on another channel and a node out of range get none.  A receiver
     * that is not listening still has a busy channel. */
    fixture_t f; fx_init(&f, 5);
    const double x[5] = { 0, 30, 80, 80, 150 };
    const int ch[5]   = { 26, 26, 26, 15, 26 };
    on_air_udgm(&f, 5, x, ch);
    radio_medium_set_radio_rx_enabled(&f.sim.radio_medium, 1, 0, false);
    for (int i = 0; i < 5; i++)
        sim_radio_bus_register(&f.bus, i, &mock_ops_on_air, &f.rx[i],
                               SIM_RADIO_DELIVERY_SYNC, 0);
    f.sim.now_ns = 3000000;

    uint8_t mac[30] = {0};
    sim_radio_bus_tx_frame(&f.bus, &f.sim, 0, mac, (int)sizeof(mac));
    /* The PHY frame's air time: the header and FCS the bus carries the
     * same frame to chip receivers with, not the MAC bytes alone. */
    int64_t end = f.sim.now_ns + IEEE802154_FRAME_AIR_NS(30);
    ASSERT_EQ(end, f.sim.now_ns + (6 + 30 + 2) * IEEE802154_BYTE_NS,
              "on_air: a native frame is on the air as its PHY frame");
    ASSERT_EQ(f.rx[0].on_air_count, 0, "on_air: not announced to the sender");
    ASSERT_EQ(f.rx[1].on_air_count, 1, "on_air: reception neighbour, radio off");
    ASSERT_EQ(f.rx[1].on_air_end, end, "on_air: reception window = the frame's air time");
    ASSERT_EQ(f.rx[2].on_air_count, 1, "on_air: interference neighbour on the channel");
    ASSERT_EQ(f.rx[2].on_air_end, end, "on_air: interference window = reception window");
    ASSERT_EQ(f.rx[3].on_air_count, 0, "on_air: interference neighbour on another channel");
    ASSERT_EQ(f.rx[4].on_air_count, 0, "on_air: out of range");
}

/* ============================================================
 * Channel energy: what a chip's CCA and RSSI register read.
 * ============================================================ */

static void test_energy_window_reach_channel(void) {
    /* An emulated sender's frame: energy at the reception neighbour (its
     * RSSI) and the interference neighbour on the channel (the UDGM floor,
     * -90 dBm) for exactly [first preamble byte, last byte's end); none on
     * another channel, out of range, or at the sender itself. */
    fixture_t f; fx_init(&f, 5);
    const double x[5] = { 0, 30, 80, 80, 150 };
    const int ch[5]   = { 26, 26, 26, 15, 26 };
    on_air_udgm(&f, 5, x, ch);
    mock_mote_t mm; register_emulated(&f, &mm, 0, 0);
    for (int i = 1; i < 5; i++)
        sim_radio_bus_register(&f.bus, i, &mock_ops, &f.rx[i],
                               SIM_RADIO_DELIVERY_PER_BYTE, 0);
    const int64_t t0 = 5000000;
    f.sim.now_ns = t0;
    uint8_t frame[64]; int n = build_802154(frame, 20);
    tx_frame(&f, 0, frame, n);
    const int64_t end = t0 + (6 + 20) * IEEE802154_BYTE_NS;
    radio_medium_t *rm = &f.sim.radio_medium;
    int near = radio_medium_get_rssi(rm, 0, 1);

    ASSERT_EQ(sim_radio_bus_channel_energy(&f.bus, &f.sim, 1, 0, t0 - 1, t0 - 1),
              SIM_RADIO_NO_ENERGY, "energy: nothing before the first byte");
    ASSERT_EQ(sim_radio_bus_channel_energy(&f.bus, &f.sim, 1, 0, t0, t0),
              near, "energy: the sender's RSSI from the first byte");
    ASSERT_EQ(sim_radio_bus_channel_energy(&f.bus, &f.sim, 1, 0, end - 1, end - 1),
              near, "energy: ... until the last byte's end");
    ASSERT_EQ(sim_radio_bus_channel_energy(&f.bus, &f.sim, 1, 0, end, end),
              SIM_RADIO_NO_ENERGY, "energy: nothing once the frame has left the air");
    ASSERT_EQ(sim_radio_bus_channel_energy(&f.bus, &f.sim, 1, 0, t0 - 1000, t0),
              near, "energy: a span that reaches the first byte sees it");
    ASSERT_EQ(sim_radio_bus_channel_energy(&f.bus, &f.sim, 1, 0, end, end + 1000),
              SIM_RADIO_NO_ENERGY, "energy: a span after the frame does not");
    ASSERT_EQ(sim_radio_bus_channel_energy(&f.bus, &f.sim, 2, 0, t0, t0),
              -90, "energy: interference neighbour on the channel, at the floor");
    ASSERT_EQ(sim_radio_bus_channel_energy(&f.bus, &f.sim, 3, 0, t0, t0),
              SIM_RADIO_NO_ENERGY, "energy: not on another channel");
    ASSERT_EQ(sim_radio_bus_channel_energy(&f.bus, &f.sim, 4, 0, t0, t0),
              SIM_RADIO_NO_ENERGY, "energy: not out of range");
    ASSERT_EQ(sim_radio_bus_channel_energy(&f.bus, &f.sim, 0, 0, t0, t0),
              SIM_RADIO_NO_ENERGY, "energy: a node's own frame does not count");

    /* The receiver turned off changes nothing: energy is on the air. */
    radio_medium_set_radio_rx_enabled(rm, 1, 0, false);
    ASSERT_EQ(sim_radio_bus_channel_energy(&f.bus, &f.sim, 1, 0, t0, t0),
              near, "energy: a receiver that is off measures the same channel");

    /* A restart forgets the windows. */
    sim_radio_bus_reset_air(&f.bus);
    ASSERT_EQ(sim_radio_bus_channel_energy(&f.bus, &f.sim, 1, 0, t0, t0),
              SIM_RADIO_NO_ENERGY, "energy: reset_air forgets the frame");
}

static void test_energy_strongest_and_native(void) {
    /* Two senders on the air at once: the stronger (nearer) one is
     * reported.  A native sender's frame counts from its tx_frame for its
     * PHY air time. */
    fixture_t f; fx_init(&f, 3);
    const double x[3] = { 0, 40, 10 };
    const int ch[3]   = { 26, 26, 26 };
    on_air_udgm(&f, 3, x, ch);
    mock_mote_t mm; register_emulated(&f, &mm, 0, 0);
    sim_radio_bus_register(&f.bus, 1, &mock_ops, &f.rx[1],
                           SIM_RADIO_DELIVERY_PER_BYTE, 0);
    sim_radio_bus_register(&f.bus, 2, &mock_ops_on_air, &f.rx[2],
                           SIM_RADIO_DELIVERY_SYNC, 0);
    radio_medium_t *rm = &f.sim.radio_medium;
    f.sim.now_ns = 2000000;
    uint8_t frame[64]; int n = build_802154(frame, 30);
    tx_frame(&f, 0, frame, n);                     /* node 0: far from node 1 */
    uint8_t mac[20] = {0};
    sim_radio_bus_tx_frame(&f.bus, &f.sim, 2, mac, (int)sizeof(mac)); /* node 2 */
    int64_t t = f.sim.now_ns + 100;
    int e = sim_radio_bus_channel_energy(&f.bus, &f.sim, 1, 0, t, t);
    int r0 = radio_medium_get_rssi(rm, 0, 1), r2 = radio_medium_get_rssi(rm, 2, 1);
    ASSERT(r0 != r2, "energy: the two senders arrive at different strengths");
    ASSERT_EQ(e, r0 > r2 ? r0 : r2, "energy: the stronger of two frames on the air");
    int64_t nend = f.sim.now_ns + IEEE802154_FRAME_AIR_NS(20);
    int64_t eend = f.sim.now_ns + (6 + 30) * IEEE802154_BYTE_NS;
    ASSERT(nend < eend, "energy: the native frame is the shorter one");
    ASSERT_EQ(sim_radio_bus_channel_energy(&f.bus, &f.sim, 1, 0, nend, nend), r0,
              "energy: once the native frame has ended, only node 0's is left");
}

static void test_energy_band(void) {
    /* A sub-GHz frame is no energy on a node's 2.4 GHz radio, only on a
     * sub-GHz one. */
    fixture_t f; fx_init(&f, 2);
    const double x[2] = { 0, 20 };
    const int ch[2]   = { 26, 26 };
    on_air_udgm(&f, 2, x, ch);
    radio_medium_t *rm = &f.sim.radio_medium;
    radio_medium_register_radio(rm, 0, 1, RADIO_SPECTRUM_868MHZ_15_4G);
    radio_medium_register_radio(rm, 1, 0, RADIO_SPECTRUM_2_4GHZ_15_4);
    radio_medium_register_radio(rm, 1, 1, RADIO_SPECTRUM_868MHZ_15_4G);
    radio_medium_set_radio_channel(rm, 0, 1, 0);
    radio_medium_set_radio_channel(rm, 1, 1, 0);
    mock_mote_t mm; register_emulated(&f, &mm, 0, 0);
    sim_radio_bus_register(&f.bus, 1, &mock_ops, &f.rx[1],
                           SIM_RADIO_DELIVERY_PER_BYTE, 0);
    f.sim.now_ns = 1000000;
    /* An 802.15.4g frame on radio 1: 0x55 preamble, sync word, 1-byte PHR. */
    static const uint8_t g[] = { 0x55, 0x55, 0x55, 0x55, 0x90, 0x4E, 0x90, 0x4E,
                                 0x08, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    for (int i = 0; i < (int)sizeof(g); i++)
        sim_radio_bus_tx_byte(&f.bus, &f.sim, 0, 1, g[i]);
    int64_t t = f.sim.now_ns + 1000;
    ASSERT_EQ(sim_radio_bus_channel_energy(&f.bus, &f.sim, 1, 0, t, t),
              SIM_RADIO_NO_ENERGY, "energy: a sub-GHz frame is nothing on the 2.4 GHz radio");
    ASSERT(sim_radio_bus_channel_energy(&f.bus, &f.sim, 1, 1, t, t) > SIM_RADIO_NO_ENERGY,
           "energy: ... and energy on the sub-GHz radio");
}

static void test_energy_dual_radio(void) {
    /* A dual-radio sender (zoul-firefly: CC2538 + CC1200) keeps one window
     * per radio: its sub-GHz frame, sent while its 2.4 GHz one is on the
     * air, does not hide the 2.4 GHz one from a 2.4 GHz neighbour, nor the
     * other way round.  With one window per sender the later frame
     * replaced the earlier. */
    fixture_t f; fx_init(&f, 2);
    const double x[2] = { 0, 20 };
    const int ch[2]   = { 26, 26 };
    on_air_udgm(&f, 2, x, ch);
    radio_medium_t *rm = &f.sim.radio_medium;
    radio_medium_register_radio(rm, 0, 1, RADIO_SPECTRUM_868MHZ_15_4G);
    radio_medium_register_radio(rm, 1, 0, RADIO_SPECTRUM_2_4GHZ_15_4);
    radio_medium_register_radio(rm, 1, 1, RADIO_SPECTRUM_868MHZ_15_4G);
    radio_medium_set_radio_channel(rm, 0, 1, 0);
    radio_medium_set_radio_channel(rm, 1, 1, 0);
    mock_mote_t mm; register_emulated(&f, &mm, 0, 0);
    sim_radio_bus_register(&f.bus, 1, &mock_ops, &f.rx[1],
                           SIM_RADIO_DELIVERY_PER_BYTE, 0);
    f.sim.now_ns = 1000000;
    uint8_t frame[64]; int n = build_802154(frame, 30);
    tx_frame(&f, 0, frame, n);                     /* 2.4 GHz, radio 0 */
    int64_t t = f.sim.now_ns + 1000;
    ASSERT(sim_radio_bus_channel_energy(&f.bus, &f.sim, 1, 0, t, t) > SIM_RADIO_NO_ENERGY,
           "dual radio: the 2.4 GHz frame is on the 2.4 GHz radio's channel");
    static const uint8_t g[] = { 0x55, 0x55, 0x55, 0x55, 0x90, 0x4E, 0x90, 0x4E,
                                 0x08, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    for (int i = 0; i < (int)sizeof(g); i++)       /* sub-GHz, radio 1 */
        sim_radio_bus_tx_byte(&f.bus, &f.sim, 0, 1, g[i]);
    ASSERT(sim_radio_bus_channel_energy(&f.bus, &f.sim, 1, 1, t, t + 100000000) >
           SIM_RADIO_NO_ENERGY, "dual radio: the sub-GHz frame is on the sub-GHz radio's channel");
    ASSERT(sim_radio_bus_channel_energy(&f.bus, &f.sim, 1, 0, t, t) > SIM_RADIO_NO_ENERGY,
           "dual radio: ... and the 2.4 GHz frame is still on the 2.4 GHz one");
    int64_t e = sim_radio_bus_next_air_edge(&f.bus, &f.sim, 1, 0, t);
    ASSERT(e > t && e != INT64_MAX, "dual radio: the 2.4 GHz frame's end is still an edge to come");
}

static void test_energy_empty_window(void) {
    /* A node that never sent has an empty window, [0, 0).  A span that
     * starts before time 0 (a CCA in the first 128 us) overlapped it, so
     * every silent neighbour read as a frame on the air. */
    fixture_t f; fx_init(&f, 3);
    const double x[3] = { 0, 20, 30 };
    const int ch[3]   = { 26, 26, 26 };
    on_air_udgm(&f, 3, x, ch);
    for (int i = 0; i < 3; i++)
        sim_radio_bus_register(&f.bus, i, &mock_ops, &f.rx[i],
                               SIM_RADIO_DELIVERY_PER_BYTE, 0);
    f.sim.now_ns = 50000;
    ASSERT_EQ(sim_radio_bus_channel_energy(&f.bus, &f.sim, 1, 0, 50000 - 128000, 50000),
              SIM_RADIO_NO_ENERGY, "empty window: a span from before 0 sees no frame");
    ASSERT_EQ(sim_radio_bus_node_energy(&f.bus, &f.sim, 1, 0, 50000, 128000),
              SIM_RADIO_NO_ENERGY, "empty window: nor does an 8-symbol CCA at 50 us");
    ASSERT_EQ(sim_radio_bus_next_air_edge(&f.bus, &f.sim, 1, 0, -1000), INT64_MAX,
              "empty window: no edge to come");
    sim_radio_bus_reset_node(&f.bus, 0);
    ASSERT_EQ(sim_radio_bus_channel_energy(&f.bus, &f.sim, 1, 0, -1, 0),
              SIM_RADIO_NO_ENERGY, "empty window: a reset node's window is empty too");
}

static void test_on_air_emulated_interference(void) {
    /* An emulated sender reaches a native mote in its interference ring:
     * no bytes are delivered, but the channel is busy for the frame. */
    fixture_t f; fx_init(&f, 3);
    const double x[3] = { 0, 80, 80 };
    const int ch[3]   = { 26, 26, 15 };
    on_air_udgm(&f, 3, x, ch);
    mock_mote_t mm; register_emulated(&f, &mm, 0, 0);
    for (int i = 1; i < 3; i++)
        sim_radio_bus_register(&f.bus, i, &mock_ops_on_air, &f.rx[i],
                               SIM_RADIO_DELIVERY_SYNC, 0);
    f.sim.now_ns = 1000000;

    uint8_t frame[64]; int n = build_802154(frame, 10);
    tx_frame(&f, 0, frame, n);
    ASSERT_EQ(f.rx[1].recv_count, 0, "on_air: nothing received in the interference ring");
    ASSERT_EQ(f.rx[1].on_air_end, f.sim.now_ns + (6 + 10) * IEEE802154_BYTE_NS,
              "on_air: busy for the frame in the interference ring");
    ASSERT_EQ(f.rx[2].on_air_count, 0, "on_air: not on another channel");
}

static void test_interference_marks_same_channel(void) {
    /* A frame-level sender's frame collides with frames queued in its
     * interference ring on its own channel only: a frame on another
     * channel cannot corrupt them. */
    fixture_t f; fx_init(&f, 3);
    const double x[3] = { 0, 80, 80 };
    const int ch[3]   = { 26, 26, 15 };
    on_air_udgm(&f, 3, x, ch);
    for (int i = 0; i < 3; i++)
        sim_radio_bus_register(&f.bus, i, &mock_ops_collisions, &f.rx[i],
                               SIM_RADIO_DELIVERY_SYNC, 0);
    f.sim.now_ns = 2000000;

    uint8_t mac[20] = {0};
    sim_radio_bus_tx_frame(&f.bus, &f.sim, 0, mac, (int)sizeof(mac));
    ASSERT_EQ(f.rx[1].collision_marks, 1, "interference: marked on the sender's channel");
    ASSERT_EQ(f.rx[1].coll_start, f.sim.now_ns, "interference: over the frame's window ...");
    ASSERT_EQ(f.rx[1].coll_end, f.sim.now_ns + IEEE802154_FRAME_AIR_NS(20),
              "interference: ... the one the on-air window uses");
    ASSERT_EQ(f.rx[2].collision_marks, 0, "interference: not marked on another channel");
}

static void test_on_air_follows_live_power(void) {
    /* The neighbour lists hold the reach at the last recompute; the byte
     * and frame filters scale the range by the sender's live output
     * power.  The window follows the live range too: a listed neighbour
     * the sender no longer reaches at its current power neither gets the
     * bytes nor reads the channel busy. */
    fixture_t f; fx_init(&f, 4);
    const double x[4] = { 0, 10, 30, 80 };
    const int ch[4]   = { 26, 26, 26, 26 };
    on_air_udgm(&f, 4, x, ch);   /* full power: 10, 30 reception; 80 interference */
    mock_mote_t mm; register_emulated(&f, &mm, 0, 0);
    for (int i = 1; i < 4; i++)
        sim_radio_bus_register(&f.bus, i, &mock_ops_on_air, &f.rx[i],
                               SIM_RADIO_DELIVERY_SYNC, 0);
    /* 8/32 of full power: 12.5 m reception, 25 m interference range. */
    radio_medium_set_radio_power(&f.sim.radio_medium, 0, 0, 8, 32);
    f.sim.now_ns = 1000000;

    uint8_t frame[64]; int n = build_802154(frame, 10);
    tx_frame(&f, 0, frame, n);
    ASSERT_EQ(f.rx[1].recv_at_count, n, "live power: a node within the scaled range gets the bytes");
    ASSERT_EQ(f.rx[1].on_air_count, 2, "live power: ... and is busy");
    ASSERT_EQ(f.rx[2].recv_at_count, 0, "live power: a listed neighbour outside it gets no bytes");
    ASSERT_EQ(f.rx[2].on_air_count, 0, "live power: ... and is not busy");
    ASSERT_EQ(f.rx[3].on_air_count, 0, "live power: nor is the listed interference neighbour");

    /* Back at full power, with the same lists, every one of them is. */
    radio_medium_set_radio_power(&f.sim.radio_medium, 0, 0, 32, 32);
    f.sim.now_ns = 2000000;
    tx_frame(&f, 0, frame, n);
    ASSERT_EQ(f.rx[2].on_air_count, 2, "full power: the reception neighbour is busy");
    ASSERT_EQ(f.rx[3].on_air_count, 2, "full power: the interference neighbour is busy");
}

static void test_emulated_interference_marks_queued(void) {
    /* An emulated sender's frame corrupts what its interference-ring
     * neighbours were receiving over its own window, on its channel only:
     * a native's queue through mark_collisions, an emulated queue
     * bus-side.  It used to skip natives, ignore the channel, and use a
     * fixed 1 ms window from the frame's completion. */
    fixture_t f; fx_init(&f, 5);
    const double x[5] = { 0, 80, 80, 80, 80 };
    const int ch[5]   = { 26, 26, 15, 26, 15 };
    on_air_udgm(&f, 5, x, ch);
    mock_mote_t mm[5]; register_emulated(&f, &mm[0], 0, 0);
    for (int i = 1; i < 3; i++)
        sim_radio_bus_register(&f.bus, i, &mock_ops_collisions, &f.rx[i],
                               SIM_RADIO_DELIVERY_SYNC, 0);
    for (int i = 3; i < 5; i++) reg_batch(&f, &mm[i], i, 0);
    f.bus.executing_node = -1;
    const int64_t t0 = 5000000;
    f.sim.now_ns = t0;

    /* Frames in flight at the two emulated neighbours: started 1 ms ago,
     * still in the air when this frame starts. */
    uint8_t q[64]; int nq = short_frame(q, 3);
    sim_radio_bus_queue_frame(&f.bus, 3, q, nq, -40, t0 - 1000000, t0 + 100000, false);
    sim_radio_bus_queue_frame(&f.bus, 4, q, nq, -40, t0 - 1000000, t0 + 100000, false);

    uint8_t frame[64]; int n = build_802154(frame, 10);
    tx_frame(&f, 0, frame, n);
    int64_t end = t0 + n * IEEE802154_BYTE_NS;
    ASSERT_EQ(f.rx[1].collision_marks, 1, "emulated interference: the native on the channel is marked");
    ASSERT_EQ(f.rx[1].coll_start, t0, "emulated interference: over the frame's window ...");
    ASSERT_EQ(f.rx[1].coll_end, end, "emulated interference: ... to its last byte, not a fixed 1 ms");
    ASSERT_EQ(f.rx[2].collision_marks, 0, "emulated interference: not the native on another channel");
    ASSERT(f.bus.emu_rx_queue[3].frames[f.bus.emu_rx_queue[3].head].collided,
           "emulated interference: the emulated neighbour's frame on the channel collided");
    ASSERT(!f.bus.emu_rx_queue[4].frames[f.bus.emu_rx_queue[4].head].collided,
           "emulated interference: not the one on another channel");
}

static void test_native_mote_radio_off_reads_busy(void) {
    /* The native Cooja mote itself, through its registered radio ops.
     * With simRadioHWOn == 0 -- a state the medium never sees -- an
     * emulated sender's bytes are refused, but its frame keeps the
     * channel busy until its last byte, so a duty-cycled native waking
     * mid-frame does not transmit over it.  With the radio on, the same
     * bytes queue a frame that ends on the bus clock, the instant the
     * channel clears. */
    fixture_t f; fx_init(&f, 2);
    const double x[2] = { 0, 30 };
    const int ch[2]   = { 26, 26 };
    on_air_udgm(&f, 2, x, ch);
    mock_mote_t mm; register_emulated(&f, &mm, 0, 0);

    static mixed_node_t mn;
    memset(&mn, 0, sizeof(mn));
    mn.slot = 1;                         /* no env: nothing to wake */
    native_node_t *nat = &mn.plat.native;
    char hw_on = 0, receiving = 0;
    int in_size = 0, channel = 26;
    uint64_t last_ts = 0;
    nat->simRadioHWOn = &hw_on;
    nat->simReceiving = &receiving;
    nat->simInSize = &in_size;
    nat->simRadioChannel = &channel;
    nat->simLastPacketTimestamp = &last_ts;
    native_cooja_mote_register_radio(&mn, 1, &f.bus);

    const int64_t t0 = 7000000;
    nat->sim_time_ns = 1000000;          /* last tick: 6 ms ago */
    f.sim.now_ns = t0;
    uint8_t frame[64]; int n = build_802154(frame, 12);   /* 10 MAC + FCS */
    tx_frame(&f, 0, frame, n);
    int64_t end = t0 + n * IEEE802154_BYTE_NS;
    ASSERT_EQ(nat->rx_queue.count, 0, "native off: no frame queued");
    ASSERT_EQ(receiving, 0, "native off: not receiving");
    ASSERT_EQ(native_radio_signal_strength(nat, end - 1), -60,
              "native off: busy until the frame's last byte");
    ASSERT_EQ(native_radio_signal_strength(nat, end), -100, "native off: clear when it ends");

    hw_on = 1;
    f.sim.now_ns = t0 + 20000000;
    tx_frame(&f, 0, frame, n);
    end = f.sim.now_ns + n * IEEE802154_BYTE_NS;
    ASSERT_EQ(nat->rx_queue.count, 1, "native on: the frame is queued");
    ASSERT_EQ(receiving, 1, "native on: receiving until it ends");
    ASSERT_EQ(nat->rx_queue.frames[nat->rx_queue.head].len, 10, "native on: the FCS is stripped");
    ASSERT_EQ(nat->rx_queue.frames[nat->rx_queue.head].end_ns, end,
              "native on: it ends on the bus clock");
    ASSERT_EQ(nat->rf_busy_until_ns, end, "native on: the channel clears at that same instant");
}

static void test_deliver_native_noop(void) {
    /* A receiver whose sim_mote has no rx_byte_sync (native/JS) gets no
     * byte delivery — only the on_rx timeline notification. */
    fixture_t f; fx_init(&f, 2);
    sim_mote_t bare = {0};
    static const sim_mote_ops_t bare_ops = { .kind = "X" };  /* rx_byte_sync NULL */
    bare.ops = &bare_ops;
    sim_runtime_register_mote(&f.sim, 1, &bare);
    sim_radio_bus_register(&f.bus, 1, &mock_ops, &f.rx[1],
                           SIM_RADIO_DELIVERY_SYNC, 0);

    uint8_t frame[64]; int n = short_frame(frame, 4);
    sim_radio_bus_deliver_bytes(&f.bus, &f.sim, 1, frame, n, -40, 0, false);
    ASSERT_EQ(f.rx[1].recv_count, 0, "native: no byte delivery (rx_byte_sync NULL)");
}

/* ============================================================
 * Frame-complete delivery policy (M27): collision windows, the dual
 * 192 µs auto-ACK windows, and RXFIFO backpressure — driven end to end
 * through sim_radio_bus_tx_byte (which invokes the bus-internal
 * sim_radio_bus_frame_complete) and observed via the frame_observed /
 * on_rx_frame / on_ack hooks.
 * ============================================================ */

static struct {
    int     frame_observed;
    int     outcome[8];       /* last on_rx_frame outcome per receiver */
    int     ack_count;
    int     ack_receiver;
    int64_t ack_start;
} frec;

static void frec_observed(void *u, const sim_radio_frame_info_t *fi) {
    (void)u; (void)fi; frec.frame_observed++;
}
static void frec_rx(void *u, const sim_radio_frame_info_t *fi, int receiver,
                    int outcome, const uint8_t *data, int len,
                    int64_t coll_start, int64_t prev_end) {
    (void)u; (void)fi; (void)data; (void)len; (void)coll_start; (void)prev_end;
    if (receiver >= 0 && receiver < 8) frec.outcome[receiver] = outcome;
}
static void frec_ack(void *u, const sim_radio_frame_info_t *fi, int data_recv,
                     int64_t ack_start, int ack_bytes) {
    (void)u; (void)fi; (void)ack_bytes;
    frec.ack_count++;
    frec.ack_receiver = data_recv;
    frec.ack_start = ack_start;
}
static void frec_set_host(fixture_t *f) {
    memset(&frec, 0, sizeof(frec));
    for (int i = 0; i < 8; i++) frec.outcome[i] = -1;
    static sim_radio_bus_host_t host;
    memset(&host, 0, sizeof(host));
    host.frame_observed = frec_observed;
    host.on_rx_frame = frec_rx;
    host.on_ack = frec_ack;
    sim_radio_bus_set_host(&f->bus, &host);
}

/* Register node idx as a BATCH emulated receiver (snapshot-delivered by
 * frame_complete) with a sim_mote backing the CPU ops. */
static void reg_batch(fixture_t *f, mock_mote_t *mm, int idx, uint32_t caps) {
    memset(mm, 0, sizeof(*mm));
    mm->freq = 1000000;
    mm->mote.impl = mm;
    mm->mote.ops = &mm_ops;
    sim_runtime_register_mote(&f->sim, idx, &mm->mote);
    sim_radio_bus_register(&f->bus, idx, &mock_ops, &f->rx[idx],
                           SIM_RADIO_DELIVERY_BATCH, caps);
}

static void tx_frame(fixture_t *f, int sender, const uint8_t *frame, int n) {
    for (int i = 0; i < n; i++)
        sim_radio_bus_tx_byte(&f->bus, &f->sim, sender, 0, frame[i]);
}

static void test_frame_collision_window(void) {
    /* A frame whose air window opens before the receiver's previous RX
     * ended (emu_rx_end_ns) is dropped as a collision — no delivery. */
    fixture_t f; fx_init(&f, 2);
    frec_set_host(&f);
    mock_mote_t mm; reg_batch(&f, &mm, 1, 0);
    f.bus.executing_node = -1;
    f.sim.now_ns = 1000;                  /* frame air-start == now (M27) */
    f.bus.emu_rx_end_ns[1] = 5000;        /* receiver busy until 5 µs     */

    uint8_t frame[64]; int n = build_802154(frame, 5);
    tx_frame(&f, 0, frame, n);

    ASSERT_EQ(frec.frame_observed, 1, "collision: frame observed once");
    ASSERT_EQ(frec.outcome[1], SIM_RADIO_RX_COLLIDED, "collision: receiver outcome COLLIDED");
    ASSERT_EQ(f.bus.stats.rx_collided, 1, "collision: rx_collided counted");
    ASSERT_EQ(f.rx[1].recv_count, 0, "collision: no bytes delivered");
    ASSERT_EQ(f.bus.stats.rx_direct, 0, "collision: not delivered directly");
}

static void test_frame_no_collision_when_clear(void) {
    /* Same frame with a clear channel delivers directly. */
    fixture_t f; fx_init(&f, 2);
    frec_set_host(&f);
    mock_mote_t mm; reg_batch(&f, &mm, 1, 0);
    f.bus.executing_node = -1;
    f.sim.now_ns = 1000;
    f.bus.emu_rx_end_ns[1] = 0;           /* channel clear */

    uint8_t frame[64]; int n = build_802154(frame, 5);
    tx_frame(&f, 0, frame, n);

    ASSERT_EQ(frec.outcome[1], SIM_RADIO_RX_DIRECT, "clear: outcome DIRECT");
    ASSERT_EQ(f.bus.stats.rx_direct, 1, "clear: delivered directly");
    ASSERT_EQ(f.rx[1].recv_count, n, "clear: all bytes delivered");
}

static void test_frame_backpressure_queue(void) {
    /* RXFIFO too small: frame_complete mini-steps the receiver once, and
     * (still full) queues the frame instead of delivering. */
    fixture_t f; fx_init(&f, 2);
    frec_set_host(&f);
    mock_mote_t mm; reg_batch(&f, &mm, 1, 0);
    f.bus.executing_node = -1;
    f.rx[1].rxfifo = 2;                   /* < frame_fifo_bytes (len+1) */

    uint8_t frame[64]; int n = build_802154(frame, 5);
    tx_frame(&f, 0, frame, n);

    ASSERT_EQ(mm.step_count, 1, "backpressure: mini-stepped once to drain RXFIFO");
    ASSERT_EQ(frec.outcome[1], SIM_RADIO_RX_QUEUED, "backpressure: outcome QUEUED");
    ASSERT_EQ(f.bus.stats.rx_queued, 1, "backpressure: rx_queued counted");
    ASSERT_EQ(f.bus.stats.rx_direct, 0, "backpressure: not delivered directly");
    ASSERT_EQ(f.bus.emu_rx_queue[1].count, 1, "backpressure: frame deferred to RX queue");
}

static void test_frame_ack_window_sender(void) {
    /* Scripted auto-ACK: the receiver emits an ACK byte (re-entering the
     * bus as its own sender) the moment it is fed the SFD during
     * synchronous delivery.  The ACK is staged in the data sender's
     * rf_pending and, at the ACK flush, queued for the sender at
     * now + 192 µs (the sender turnaround window — not tx_end + 192 µs). */
    fixture_t f; fx_init(&f, 2);
    frec_set_host(&f);
    mock_mote_t mm0, mm1;
    reg_batch(&f, &mm0, 0, 0);   /* data sender — also receives the ACK */
    reg_batch(&f, &mm1, 1, 0);   /* data receiver — auto-ACKs           */
    f.bus.executing_node = -1;
    f.sim.now_ns = 0;
    f.rx[1].reenter_on = 0x7A;   /* on the SFD, emit one ACK byte... */
    f.rx[1].reenter_sender = 1;  /* ...as a transmission from node 1 */

    uint8_t frame[64]; int n = build_802154(frame, 5);
    tx_frame(&f, 0, frame, n);

    ASSERT_EQ(frec.ack_count, 1, "ack: one ACK flush notification");
    ASSERT_EQ(frec.ack_receiver, 1, "ack: keyed on the data receiver");
    /* Sender's ACK is queued (not delivered) for the sender turnaround. */
    ASSERT_EQ(f.bus.emu_rx_queue[0].count, 1, "ack: sender ACK deferred to its RX queue");
    ASSERT_EQ(f.bus.emu_rx_queue[0].frames[f.bus.emu_rx_queue[0].head].arrival_ns,
              192000, "ack: sender ACK at now + 192 µs turnaround");
}

/* The bus hands the frame hooks the PHR width its assembler settled on, so
 * the runner's capture (test_mixed_multinode.c) reads a 2-byte CC1200 PHR
 * as one.  Driven through sim_radio_bus_tx_byte like a chip's TX, with the
 * sender reporting its PHR width (mote_radio_ops_t.tx_phr_len) as the
 * Firefly's CC1200 does from PKT_CFG2.FG_MODE. */
static struct { int frames; int phr_len; int cap_len; uint8_t cap[256]; } fcap;
static void fcap_observed(void *u, const sim_radio_frame_info_t *fi) {
    (void)u;
    fcap.frames++;
    fcap.phr_len = fi->subghz_phr_len;
    fcap.cap_len = sim_radio_frame_capture(fi->capture, fi->capture_len,
                                           fi->subghz, fi->subghz_phr_len,
                                           fcap.cap, (int)sizeof fcap.cap);
}
static int mock_phr_width;   /* what mock_ops_phr reports */
static int mock_phr_radio;   /* the radio slot it was asked about */
static int mock_tx_phr_len(void *m, int radio) {
    (void)m;
    mock_phr_radio = radio;
    return mock_phr_width;
}
static const mote_radio_ops_t mock_ops_phr = {
    .receive_byte     = mock_receive_byte,
    .rxfifo_available = mock_rxfifo_available,
    .rx_busy          = mock_rx_busy,
    .tx_phr_len       = mock_tx_phr_len,
};

/* Send one CC1200 frame from node 0 through the bus and return whether it
 * completed.  pad bytes of the sender's next preamble follow it. */
static int fcap_send(const mote_radio_ops_t *ops, int width,
                     const uint8_t *phr, int phr_n, const uint8_t *mac,
                     int mac_n, int pad) {
    static fixture_t f;   /* ~6 MB: not on the stack of every inlined call */
    fx_init(&f, 2);
    static sim_radio_bus_host_t host;
    memset(&host, 0, sizeof(host));
    host.frame_observed = fcap_observed;
    sim_radio_bus_set_host(&f.bus, &host);
    sim_radio_bus_register(&f.bus, 0, ops, &f.rx[0], SIM_RADIO_DELIVERY_PER_BYTE, 0);
    sim_radio_bus_register(&f.bus, 1, &mock_ops, &f.rx[1], SIM_RADIO_DELIVERY_PER_BYTE, 0);
    memset(&fcap, 0, sizeof fcap);
    mock_phr_width = width;
    mock_phr_radio = -1;
    uint8_t air[300];
    int n = cc1200_air(air, phr, phr_n, mac, mac_n);
    for (int i = 0; i < pad; i++) air[n++] = 0x55;
    for (int i = 0; i < n; i++)
        sim_radio_bus_tx_byte(&f.bus, &f.sim, 0, 1, air[i]);
    return fcap.frames;
}

static void test_capture_frame_info_phr_len(void) {
    uint8_t mac[20];
    for (int i = 0; i < 20; i++) mac[i] = (uint8_t)(0x30 + i);
    mac[3] = 0x10;   /* bytes that look like a PHR or an SFD are data */
    mac[7] = 0x7A;

    /* 802.15.4g PHRs as Contiki's cc1200 driver writes them for a 20-byte
     * MAC frame: CRC-16 (10 16 = 20 + 2, its default), whitening with
     * CRC-32 (08 18), whitening with CRC-16 (18 16), CRC-32 (00 18).  The
     * first byte of the first three is a plausible 1-byte length, so only
     * the sender's word decides the width.
     *
     * pad: the assembler waits for the PHR length + 2 bytes after a CC1200
     * PHR, as for the 1-byte layout, though a 2-byte PHR's length already
     * counts the CRC, and the model sends 2 placeholder CRC bytes; so the
     * frame only completes on bytes of the sender's next transmission (its
     * preamble), as it does in a run.  The padding stands in for them; it
     * goes when that wait is fixed (a separate issue). */
    static const struct { uint8_t phr[2]; int crc; const char *what; } g[] = {
        { { 0x10, 22 }, 2, "cap fi 10 16 (CRC-16)" },
        { { 0x08, 24 }, 4, "cap fi 08 18 (whitening, CRC-32)" },
        { { 0x18, 22 }, 2, "cap fi 18 16 (whitening, CRC-16)" },
        { { 0x00, 24 }, 4, "cap fi 00 18 (CRC-32)" },
    };
    char msg[128];
    for (int k = 0; k < (int)(sizeof g / sizeof g[0]); k++) {
        int pad = (g[k].phr[1] + 2) - (20 + 2);
        snprintf(msg, sizeof msg, "%s: one frame observed", g[k].what);
        ASSERT_EQ(fcap_send(&mock_ops_phr, 2, g[k].phr, 2, mac, 20, pad), 1, msg);
        snprintf(msg, sizeof msg, "%s: frame info carries the 2-byte PHR width", g[k].what);
        ASSERT_EQ(fcap.phr_len, 2, msg);
        check_capture(fcap.cap, fcap.cap_len, mac, 20, g[k].what);
        /* One byte short of that wait: not complete yet. */
        snprintf(msg, sizeof msg, "%s: no completion before PHR length + 2", g[k].what);
        ASSERT_EQ(fcap_send(&mock_ops_phr, 2, g[k].phr, 2, mac, 20, pad - 1), 0, msg);
    }
    ASSERT_EQ(mock_phr_radio, 1, "cap fi: the sender is asked about the radio slot it sent on");

    /* A sender that cannot say (no op, or 0) leaves the assembler to guess:
     * 00 18 is no 1-byte length, so it is still read as 2 bytes ... */
    ASSERT_EQ(fcap_send(&mock_ops, 0, g[3].phr, 2, mac, 20, 4), 1, "cap guess 00 18: observed");
    ASSERT_EQ(fcap.phr_len, 2, "cap guess 00 18: 2-byte PHR");
    check_capture(fcap.cap, fcap.cap_len, mac, 20, "cap guess 00 18");
    ASSERT_EQ(fcap_send(&mock_ops_phr, 0, g[3].phr, 2, mac, 20, 4), 1, "cap guess 0: observed");
    ASSERT_EQ(fcap.phr_len, 2, "cap guess 0: 00 18 as 2 bytes");
    /* ... but 10 16 reads as a 1-byte length of 16, the misframing the
     * sender's word avoids. */
    fcap_send(&mock_ops, 0, g[0].phr, 2, mac, 20, 2);
    ASSERT_EQ(fcap.phr_len, 1, "cap guess 10 16: taken for a 1-byte length");

    /* Standard mode (width 1): the length byte counts the MAC frame. */
    uint8_t len1 = 20;
    ASSERT_EQ(fcap_send(&mock_ops_phr, 1, &len1, 1, mac, 20, 0), 1, "cap fi 1-byte: observed");
    ASSERT_EQ(fcap.phr_len, 1, "cap fi 1-byte: width 1");
    check_capture(fcap.cap, fcap.cap_len, mac, 20, "cap fi 1-byte PHR");
    /* A length byte the guess would take for a 2-byte PHR's upper byte is
     * no frame when the sender says it sends a length byte. */
    uint8_t len_bad = 2;
    ASSERT_EQ(fcap_send(&mock_ops_phr, 1, &len_bad, 1, mac, 20, 4), 0,
              "cap fi 1-byte: length 2 is no frame");
}

/* ============================================================
 * Entry point.
 * ============================================================ */

int run_radio_bus_tests(int verbose);
int run_radio_bus_tests(int verbose) {
    (void)verbose;
    passed = failed = 0;
    printf("=== sim_radio_bus unit tests (Phase 5 guardrail) ===\n");

    test_asm_802154();
    test_asm_802154g();
    test_asm_ti_prop();
    test_asm_invalid_length_resets();
    test_asm_reset_clears();
    test_frame_fifo_bytes();
    test_capture_24ghz_passthrough();
    test_capture_cc1200_1byte_phr();
    test_capture_cc1200_2byte_phr();
    test_capture_ti_prop();
    test_capture_subghz_long_frames();
    test_byte_period();
    test_pick_receiver_radio();
    test_delivery_sync();
    test_delivery_per_byte();
    test_delivery_batch();
    test_capture_and_first_byte();
    test_byte_clock_arming();
    test_reentrant_depth();
    test_rx_stall();
    test_reset_node();
    test_deliver_direct();
    test_deliver_executing_node();
    test_queue_and_drain();
    test_drain_collided_skip();
    test_drain_max_arrival();
    test_deliver_native_noop();
    test_native_signal_strength_follows_air();
    test_on_air_emulated_sender();
    test_on_air_byte_path_radio_off();
    test_native_byte_frame_ends_on_bus_clock();
    test_on_air_reach_and_channel();
    test_on_air_emulated_interference();
    test_energy_window_reach_channel();
    test_energy_strongest_and_native();
    test_energy_band();
    test_energy_dual_radio();
    test_energy_empty_window();
    test_interference_marks_same_channel();
    test_on_air_follows_live_power();
    test_emulated_interference_marks_queued();
    test_native_mote_radio_off_reads_busy();
    test_frame_collision_window();
    test_frame_no_collision_when_clear();
    test_frame_backpressure_queue();
    test_frame_ack_window_sender();
    test_capture_frame_info_phr_len();

    printf("--- Results: %d passed, %d failed ---\n", passed, failed);
    return failed;
}
