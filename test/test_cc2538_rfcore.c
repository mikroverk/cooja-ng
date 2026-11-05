/*
 * CC2538 RF core: TX and RX timing in the main FSM (SWRU319C §23.8.2,
 * §23.9.2, Figure 23-18), the SFD signal and its MAC timer capture
 * (§23.8.11, §23.9.5.2, §12.1.10), auto-ACK timing and state, source
 * address matching and the frame-pending bit (§23.9.6, §23.9.8).
 *
 * A cc2538dk platform with the CPU parked on a `b .` in SRAM, so time
 * passes and the radio's events fire with no firmware.  The test programs
 * the registers through the bus as a driver would, feeds frames byte by
 * byte one byte period apart (each at its start, as the radio bus delivers
 * them), and records what the radio sends with the time it sends it.
 */
#include "arm_platform.h"
#include "cc2538_soc.h"
#include "cc2538_rfcore.h"
#include "ieee_802154.h"
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
        printf("  FAIL: %s: got %lld (0x%llx), want %lld (0x%llx) (%s:%d)\n", \
               msg, a_, a_, e_, e_, __FILE__, __LINE__); }               \
} while (0)

#define BYTE_NS 32000LL
#define LOOP_ADDR 0x20001000u

#define FFSM(off) (RFCORE_FFSM_BASE + (off))
#define XREG(off) (RFCORE_XREG_BASE + (off))
#define SFR(off)  (RFCORE_SFR_BASE + (off))

static arm_platform_t plat;
static arm_cpu_t *cpu;
static cc2538_rfcore_t *rf;

/* What the radio sent, and when (the CPU's time at each byte). */
static uint8_t tx_bytes[512];
static int64_t tx_ns[512];
static int tx_n;

static void record_tx(void *user, uint8_t byte) {
    (void)user;
    if (tx_n < (int)sizeof(tx_bytes)) {
        tx_bytes[tx_n] = byte;
        tx_ns[tx_n] = cpu->sim_time_ns;
        tx_n++;
    }
}

static void run_to_ns(int64_t ns) {
    int64_t target = cpu_ns_to_cycles(ns, cpu->cpu_freq_hz);
    if (target > cpu->cycles) arm_step_until(cpu, target);
}

static uint32_t rd(uint32_t addr) { return arm_read32(cpu, addr); }
static void wr(uint32_t addr, uint32_t v) { arm_write32(cpu, addr, v); }
static void strobe(uint32_t s) { wr(SFR(RFCORE_SFR_RFST), s); }

static bool plat_live;

static void setup(void) {
    if (plat_live) arm_platform_destroy(&plat);
    plat_live = true;
    arm_platform_init(&plat, arm_platform_find("cc2538dk"));
    cpu = &plat.cpu;
    rf = &arm_platform_cc2538(&plat)->rfcore;
    arm_write16(cpu, LOOP_ADDR, 0xE7FE);          /* b . */
    cpu->reg[ARM_PC] = LOOP_ADDR;
    cpu->xpsr = 1u << 24;
    cc2538_rfcore_set_tx_callback(rf, record_tx, NULL);
    tx_n = 0;
    run_to_ns(1000000);

    /* PAN 0xABCD, short address 0x0001; AUTOCRC | AUTOACK. */
    wr(FFSM(0x5C8), 0xCD); wr(FFSM(0x5CC), 0xAB);
    wr(FFSM(0x5D0), 0x01); wr(FFSM(0x5D4), 0x00);
    wr(XREG(RFCORE_XREG_FRMCTRL0), 0x60);
    /* Source table: short entry 0 = PAN 0xABCD, 0x0002, enabled with
     * auto-pending; extended entry 1 (words 8..15) = 11:22:..:88 (as on
     * the air, LSB first), enabled (SRCEXTEN bit 2), auto-pending. */
    const uint8_t e0[4] = { 0xCD, 0xAB, 0x02, 0x00 };
    for (int i = 0; i < 4; i++) wr(FFSM(0x400 + 4 * i), e0[i]);
    for (int i = 0; i < 8; i++) wr(FFSM(0x400 + 4 * (8 + i)), 0x11 * (i + 1));
    wr(XREG(0x00C), 0x01);                        /* SRCSHORTEN0 */
    wr(FFSM(0x59C), 0x01);                        /* SRCSHORTPENDEN0 */
    wr(XREG(0x018), 0x04);                        /* SRCEXTEN0: entry 1 */
    wr(FFSM(0x590), 0x04);                        /* SRCEXTPENDEN0 */
    strobe(CSP_ISRXON);
}

/* Build a frame: FCF, DSN 0x55, dest PAN 0xABCD + dest, source (PAN
 * compressed), payload; FCS appended.  Returns the PSDU length. */
static int build(uint8_t *psdu, uint16_t fcf, uint16_t dst, const uint8_t *src, int src_len,
                 const uint8_t *payload, int payload_len) {
    int n = 0;
    psdu[n++] = fcf & 0xFF; psdu[n++] = fcf >> 8;
    psdu[n++] = 0x55;
    psdu[n++] = 0xCD; psdu[n++] = 0xAB;
    psdu[n++] = dst & 0xFF; psdu[n++] = dst >> 8;
    memcpy(psdu + n, src, (size_t)src_len); n += src_len;
    memcpy(psdu + n, payload, (size_t)payload_len); n += payload_len;
    uint16_t crc = 0;
    for (int i = 0; i < n; i++) crc = ieee802154_crc_add_bitrev(crc, psdu[i]);
    psdu[n++] = ieee802154_bitrev((crc >> 8) & 0xFF);
    psdu[n++] = ieee802154_bitrev(crc & 0xFF);
    return n;
}

/* Deliver `zeros` preamble bytes, the SFD, length and PSDU from t0, one
 * byte period apart.  Returns the time the last byte was delivered (its
 * start on the air). */
static int64_t deliver_pre(int64_t t0, int zeros, const uint8_t *psdu, int n) {
    uint8_t air[140];
    int k = 0;
    for (int i = 0; i < zeros; i++) air[k++] = 0x00;
    air[k++] = 0x7A;
    air[k++] = (uint8_t)n;
    memcpy(air + k, psdu, (size_t)n); k += n;
    int64_t t = t0;
    for (int i = 0; i < k; i++) {
        t = t0 + i * BYTE_NS;
        run_to_ns(t);
        cc2538_rfcore_receive_byte(rf, air[i]);
    }
    return t;
}
static int64_t deliver(int64_t t0, const uint8_t *psdu, int n) {
    return deliver_pre(t0, 4, psdu, n);
}

/* The ACK the radio sent, if any: index of its first preamble byte. */
static int find_ack(int from) {
    for (int i = from; i + 10 < tx_n; i++)
        if (tx_bytes[i] == 0 && tx_bytes[i + 4] == 0x7A && tx_bytes[i + 5] == 5 &&
            (tx_bytes[i + 6] & 0x07) == 0x02)
            return i;
    return -1;
}

static const uint8_t SRC_SHORT_2[2] = { 0x02, 0x00 };
static const uint8_t SRC_SHORT_3[2] = { 0x03, 0x00 };
static const uint8_t SRC_EXT[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
static const uint8_t DATAREQ[1] = { 0x04 };
static const uint8_t DATA[3] = { 0xDE, 0xAD, 0x01 };

/* FCF: AR | PAN ID compression | dest short; source mode; frame type. */
#define FCF(type, sam) ((uint16_t)((type) | 0x0020 | 0x0040 | (2 << 10) | ((sam) << 14)))

static void test_ack_timing_and_state(void) {
    setup();
    uint8_t psdu[64];
    int n = build(psdu, FCF(3, 2), 0x0001, SRC_SHORT_2, 2, DATAREQ, 1);
    int64_t last = deliver(2000000, psdu, n);
    int64_t frame_end = last + BYTE_NS;

    /* Turnaround: ACK calibration, TX_ACTIVE, not receiving. */
    ASSERT_EQ(rd(XREG(RFCORE_XREG_FSMSTAT0)), RF_STATE_ACK_CALIBR, "FSMSTAT0 in the turnaround");
    ASSERT((rd(XREG(RFCORE_XREG_FSMSTAT1)) & 3) == 2,
           "FSMSTAT1: TX_ACTIVE, not RX_ACTIVE, in the turnaround");
    ASSERT_EQ(find_ack(0), -1, "no ACK before the turnaround");

    run_to_ns(frame_end + 191000);
    ASSERT_EQ(find_ack(0), -1, "no ACK 191 us after the frame");
    run_to_ns(frame_end + 193000);
    int a = find_ack(0);
    ASSERT(a >= 0, "ACK sent by 193 us after the frame");
    if (a < 0) return;
    int64_t gap = tx_ns[a] - frame_end;
    ASSERT(gap >= 192000 && gap < 192100, "ACK starts 192 us after the frame's last byte ends");
    if (!(gap >= 192000 && gap < 192100)) printf("    gap = %lld ns\n", (long long)gap);
    ASSERT_EQ(tx_bytes[a + 6], 0x12,
              "ACK FCF: frame pending (Data Request from an auto-pending entry)");
    ASSERT_EQ(tx_bytes[a + 8], 0x55, "ACK DSN");

    /* On the air: ACK states, TX_ACTIVE until the ACK's 352 us are over. */
    ASSERT_EQ(rd(XREG(RFCORE_XREG_FSMSTAT0)), RF_STATE_ACK, "FSMSTAT0 while the ACK is on the air");
    ASSERT(rd(XREG(RFCORE_XREG_FSMSTAT1)) & 2, "TX_ACTIVE while the ACK is on the air");
    ASSERT(!(rd(SFR(RFCORE_SFR_RFIRQF1)) & RFIRQF1_TXACKDONE),
           "no TXACKDONE before the ACK has been sent");
    run_to_ns(tx_ns[a] + 11 * BYTE_NS + 1000);
    ASSERT(rd(SFR(RFCORE_SFR_RFIRQF1)) & RFIRQF1_TXACKDONE, "TXACKDONE after the ACK");
    /* Then the TX/RX transit back to SFD wait, 192 us (Figure 23-18). */
    ASSERT_EQ(rd(XREG(RFCORE_XREG_FSMSTAT0)), RF_STATE_TX_RX_TRANSIT, "TX/RX transit after the ACK");
    ASSERT(rd(XREG(RFCORE_XREG_FSMSTAT1)) & 2, "TX_ACTIVE in the TX/RX transit");
    run_to_ns(tx_ns[a] + 11 * BYTE_NS + 191000);
    ASSERT_EQ(rd(XREG(RFCORE_XREG_FSMSTAT0)), RF_STATE_TX_RX_TRANSIT, "still in transit 191 us after the ACK");
    run_to_ns(tx_ns[a] + 11 * BYTE_NS + 193000);
    ASSERT(!(rd(XREG(RFCORE_XREG_FSMSTAT1)) & 2), "TX_ACTIVE clear 192 us after the ACK");
    ASSERT_EQ(rd(XREG(RFCORE_XREG_FSMSTAT0)), RF_STATE_SFD_WAIT, "back to SFD wait 192 us after the ACK");

    /* Source matching results. */
    uint32_t f0 = rd(SFR(RFCORE_SFR_RFIRQF0));
    ASSERT(f0 & RFIRQF0_SRC_MATCH_FOUND, "SRC_MATCH_FOUND");
    ASSERT(f0 & RFIRQF0_SRC_MATCH_DONE, "SRC_MATCH_DONE");
    ASSERT_EQ(rd(FFSM(0x58C)), 0x40, "SRCRESINDEX: short entry 0, AUTOPEND");
    ASSERT_EQ(rd(FFSM(0x580)), 0x01, "SRCRESMASK0");
}

/* Run one frame on a fresh radio; returns the ACK index or -1. */
static int one_frame(uint16_t fcf, uint16_t dst, const uint8_t *src, int src_len,
                     const uint8_t *pl, int pl_len, bool corrupt) {
    setup();
    uint8_t psdu[64];
    int n = build(psdu, fcf, dst, src, src_len, pl, pl_len);
    if (corrupt) psdu[n - 1] ^= 0xFF;
    int64_t last = deliver(2000000, psdu, n);
    run_to_ns(last + 1000000);
    return find_ack(0);
}

static void test_source_matching(void) {
    /* A data frame from the same entry: found, but PEND_DATAREQ_ONLY. */
    int a = one_frame(FCF(1, 2), 0x0001, SRC_SHORT_2, 2, DATA, 3, false);
    ASSERT(a >= 0 && tx_bytes[a + 6] == 0x02,
           "data frame from a pending entry: ACK without frame pending");
    ASSERT(rd(SFR(RFCORE_SFR_RFIRQF0)) & RFIRQF0_SRC_MATCH_FOUND, "data frame: SRC_MATCH_FOUND");
    ASSERT_EQ(rd(FFSM(0x58C)), 0x00, "data frame: SRCRESINDEX entry 0, no AUTOPEND");

    /* A source not in the table. */
    a = one_frame(FCF(3, 2), 0x0001, SRC_SHORT_3, 2, DATAREQ, 1, false);
    ASSERT(a >= 0 && tx_bytes[a + 6] == 0x02, "unknown source: ACK without frame pending");
    uint32_t f0 = rd(SFR(RFCORE_SFR_RFIRQF0));
    ASSERT(!(f0 & RFIRQF0_SRC_MATCH_FOUND), "unknown source: no SRC_MATCH_FOUND");
    ASSERT(f0 & RFIRQF0_SRC_MATCH_DONE, "unknown source: SRC_MATCH_DONE");
    ASSERT_EQ(rd(FFSM(0x58C)), 0x3F, "unknown source: SRCRESINDEX 0x3F");
    ASSERT_EQ(rd(FFSM(0x580)), 0x00, "unknown source: SRCRESMASK0");

    /* An extended source address, entry 1. */
    a = one_frame(FCF(3, 3), 0x0001, SRC_EXT, 8, DATAREQ, 1, false);
    ASSERT(a >= 0 && tx_bytes[a + 6] == 0x12, "extended entry: ACK with frame pending");
    ASSERT_EQ(rd(FFSM(0x58C)), 0x61, "extended entry: SRCRESINDEX 1 | ext | AUTOPEND");
    ASSERT_EQ(rd(FFSM(0x580)), 0x0C, "extended entry: SRCRESMASK0 bits 2 and 3");

    /* Not for us: frame filtering rejects it, so no matching and no ACK. */
    a = one_frame(FCF(3, 2), 0x0009, SRC_SHORT_2, 2, DATAREQ, 1, false);
    ASSERT_EQ(a, -1, "frame for another node: no ACK");
    f0 = rd(SFR(RFCORE_SFR_RFIRQF0));
    ASSERT(!(f0 & (RFIRQF0_SRC_MATCH_FOUND | RFIRQF0_SRC_MATCH_DONE)),
           "frame for another node: no source matching");

    /* Matching runs before the FCS is known: a bad-CRC frame from a table
     * entry sets the flags (SWRU319C Figure 23-12), but is not ACKed. */
    a = one_frame(FCF(3, 2), 0x0001, SRC_SHORT_2, 2, DATAREQ, 1, true);
    ASSERT_EQ(a, -1, "bad CRC: no ACK");
    ASSERT(rd(SFR(RFCORE_SFR_RFIRQF0)) & RFIRQF0_SRC_MATCH_DONE, "bad CRC: SRC_MATCH_DONE");

    /* FRMCTRL1.PENDING_OR sets the bit for every ACK. */
    setup();
    wr(XREG(RFCORE_XREG_FRMCTRL1), rd(XREG(RFCORE_XREG_FRMCTRL1)) | 0x04);
    uint8_t psdu[64];
    int n = build(psdu, FCF(1, 2), 0x0001, SRC_SHORT_3, 2, DATA, 3);
    int64_t last = deliver(2000000, psdu, n);
    run_to_ns(last + 1000000);
    a = find_ack(0);
    ASSERT(a >= 0 && tx_bytes[a + 6] == 0x12, "PENDING_OR: ACK with frame pending");

    /* SRCMATCH = 0 (Contiki-NG): no matching, no flags. */
    setup();
    wr(XREG(RFCORE_XREG_SRCMATCH), 0);
    n = build(psdu, FCF(3, 2), 0x0001, SRC_SHORT_2, 2, DATAREQ, 1);
    last = deliver(2000000, psdu, n);
    run_to_ns(last + 1000000);
    a = find_ack(0);
    ASSERT(a >= 0 && tx_bytes[a + 6] == 0x02, "SRCMATCH=0: ACK without frame pending");
    ASSERT(!(rd(SFR(RFCORE_SFR_RFIRQF0)) & (RFIRQF0_SRC_MATCH_FOUND | RFIRQF0_SRC_MATCH_DONE)),
           "SRCMATCH=0: no source-match flags");

    /* SRCEXTEN: bits 2n+1 always read 0 (SWRU319C §23.9.6.3). */
    setup();
    wr(XREG(0x018), 0xFF);
    ASSERT_EQ(rd(XREG(0x018)), 0x55, "SRCEXTEN0 reads back bits 2n only");
}

/* The strobes that abort a transmission abort an ACK in its turnaround. */
static void test_ack_abort(void) {
    static const uint32_t strobes[3] = { CSP_ISRFOFF, CSP_ISRXON, CSP_ISTXON };
    static const char *names[3] = { "ISRFOFF", "ISRXON", "ISTXON" };
    for (int s = 0; s < 3; s++) {
        setup();
        uint8_t psdu[64];
        int n = build(psdu, FCF(1, 2), 0x0001, SRC_SHORT_3, 2, DATA, 3);
        int64_t last = deliver(2000000, psdu, n);
        run_to_ns(last + 100000);                     /* in the turnaround */
        strobe(strobes[s]);
        run_to_ns(last + 2000000);
        char msg[96];
        snprintf(msg, sizeof msg, "%s in the turnaround: no ACK", names[s]);
        ASSERT_EQ(find_ack(0), -1, msg);
        snprintf(msg, sizeof msg, "%s in the turnaround: TX_ACTIVE clear afterwards", names[s]);
        ASSERT(!(rd(XREG(RFCORE_XREG_FSMSTAT1)) & 2), msg);
    }
}

/* While it sends its ACK the radio hears nothing. */
static void test_deaf_during_ack(void) {
    setup();
    uint8_t psdu[64];
    int n = build(psdu, FCF(1, 2), 0x0001, SRC_SHORT_3, 2, DATA, 3);
    int64_t last = deliver(2000000, psdu, n);
    int fifo = rf->rxfifo_len;
    uint8_t other[64];
    int m = build(other, FCF(1, 2) & ~0x0020, 0x0001, SRC_SHORT_2, 2, DATA, 3);
    deliver(last + 2 * BYTE_NS, other, m);            /* lands in the turnaround */
    ASSERT_EQ(rf->rxfifo_len, fifo, "a frame arriving during the turnaround is not received");
    run_to_ns(last + 2000000);
    ASSERT(find_ack(0) >= 0, "the first frame is still ACKed");
}

/* A frame whose SFD arrives while the ACK is on the air is not received
 * either; one that starts after the ACK is. */
static void test_deaf_while_ack_on_air(void) {
    setup();
    uint8_t psdu[64];
    int n = build(psdu, FCF(1, 2), 0x0001, SRC_SHORT_3, 2, DATA, 3);
    int64_t last = deliver(2000000, psdu, n);
    int64_t ack_start = last + BYTE_NS + 192000;
    int fifo = rf->rxfifo_len;
    uint8_t other[64];
    int m = build(other, FCF(1, 2) & ~0x0020, 0x0001, SRC_SHORT_2, 2, DATA, 3);
    /* Preamble from 2 bytes into the ACK: its SFD lands at byte 6. */
    deliver(ack_start + 2 * BYTE_NS, other, m);
    ASSERT_EQ(rf->rxfifo_len, fifo, "a frame arriving while the ACK is on the air is not received");
    int a = find_ack(0);
    ASSERT(a >= 0 && tx_ns[a] == ack_start, "the ACK went out as scheduled");

    /* After the ACK (11 bytes), the TX/RX transit (192 us), then SFD wait:
     * a frame whose SFD comes before the transit ends is not heard; one
     * whose last preamble byte comes after it is. */
    setup();
    n = build(psdu, FCF(1, 2), 0x0001, SRC_SHORT_3, 2, DATA, 3);
    last = deliver(2000000, psdu, n);
    ack_start = last + BYTE_NS + 192000;
    int64_t ready = ack_start + 11 * BYTE_NS + 192000;
    fifo = rf->rxfifo_len;
    deliver(ready - 5 * BYTE_NS, other, m);           /* SFD 1 byte before the end */
    ASSERT_EQ(rf->rxfifo_len, fifo, "a frame whose SFD arrives in the TX/RX transit is not received");
    run_to_ns(ready + 1000);
    ASSERT_EQ(rd(XREG(RFCORE_XREG_FSMSTAT0)), RF_STATE_SFD_WAIT, "SFD wait after the transit");
    deliver(ready + 2000000, other, m);
    ASSERT_EQ(rf->rxfifo_len, fifo + 1 + m, "a frame after the transit is received");
}

/* No ACK from a radio the firmware has turned off (ISRFOFF).  The model
 * still puts the frame in the RXFIFO (its "perfect reception while off",
 * finding F6 in docs/design/kernel-radio-review-and-performance-plan.md);
 * this test covers only the ACK, which silicon, not having heard the
 * frame, never sends. */
static void test_no_ack_while_off(void) {
    setup();
    strobe(CSP_ISRFOFF);
    uint8_t psdu[64];
    int n = build(psdu, FCF(3, 2), 0x0001, SRC_SHORT_2, 2, DATAREQ, 1);
    int64_t last = deliver(2000000, psdu, n);
    ASSERT_EQ(rd(XREG(RFCORE_XREG_FSMSTAT0)), RF_STATE_IDLE, "radio off: FSMSTAT0 idle");
    ASSERT(!(rd(XREG(RFCORE_XREG_FSMSTAT1)) & 2), "radio off: no TX_ACTIVE after the frame");
    run_to_ns(last + 1000000);
    ASSERT_EQ(find_ack(0), -1, "radio off: no ACK");
    ASSERT(!(rd(SFR(RFCORE_SFR_RFIRQF1)) & RFIRQF1_TXACKDONE), "radio off: no TXACKDONE");

    /* Back on (ISRXON): the same frame is ACKed. */
    strobe(CSP_ISRXON);
    last = deliver(last + 2000000, psdu, n);
    run_to_ns(last + 1000000);
    ASSERT(find_ack(0) >= 0, "radio on again: ACK");
}

/* AUTOACK only works with frame filtering on (SWRU319C §23.9.8.3); with
 * FRAME_FILTER_EN clear there is no ACK and no source matching.  Nor is a
 * beacon ACKed, even with its ACK-request bit set. */
static void test_no_ack_without_filtering(void) {
    setup();
    wr(XREG(RFCORE_XREG_FRMFILT0), rd(XREG(RFCORE_XREG_FRMFILT0)) & ~1u);
    uint8_t psdu[64];
    int n = build(psdu, FCF(3, 2), 0x0001, SRC_SHORT_2, 2, DATAREQ, 1);
    int64_t last = deliver(2000000, psdu, n);
    ASSERT(!(rd(XREG(RFCORE_XREG_FSMSTAT1)) & 2), "filtering off: no TX_ACTIVE after the frame");
    run_to_ns(last + 1000000);
    ASSERT_EQ(find_ack(0), -1, "filtering off: no ACK");
    ASSERT(!(rd(SFR(RFCORE_SFR_RFIRQF0)) & (RFIRQF0_SRC_MATCH_FOUND | RFIRQF0_SRC_MATCH_DONE)),
           "filtering off: no source matching");
    ASSERT(rd(SFR(RFCORE_SFR_RFIRQF0)) & RFIRQF0_RXPKTDONE, "filtering off: the frame is received");

    ASSERT_EQ(one_frame(FCF(0, 2), 0x0001, SRC_SHORT_2, 2, DATA, 3, false), -1,
              "beacon with ACK request: no ACK");
}

/* --- TX and RX timing in the main FSM --- */

#define FSM0() rd(XREG(RFCORE_XREG_FSMSTAT0))
#define FSM1() rd(XREG(RFCORE_XREG_FSMSTAT1))
#define FSM1_RX_ACTIVE 0x01u
#define FSM1_TX_ACTIVE 0x02u
#define FSM1_SFD       0x20u

/* The SFD capture (MTMSEL = 1, MTMOVFSEL = 1) as a 40-bit MAC timer count. */
static int64_t sfd_capture(void) {
    wr(SFR(RFCORE_SFR_MTMSEL), 0x11);
    int64_t v = rd(SFR(RFCORE_SFR_MTM0)) | (rd(SFR(RFCORE_SFR_MTM1)) << 8);
    v |= (int64_t)rd(SFR(RFCORE_SFR_MTMOVF0)) << 16;
    v |= (int64_t)rd(SFR(RFCORE_SFR_MTMOVF1)) << 24;
    v |= (int64_t)rd(SFR(RFCORE_SFR_MTMOVF2)) << 32;
    wr(SFR(RFCORE_SFR_MTMSEL), 0);
    return v;
}
/* MAC timer ticks (32 MHz) at a cycle of the CPU. */
static int64_t mt_at_cycle(int64_t cycle) {
    return cpu_cycles_to_ns(cycle, cpu->cpu_freq_hz) * 32 / 1000;
}

/* A frame as Contiki-NG writes it: length (MPDU + FCS), then the MPDU
 * without its FCS (AUTOCRC adds it).  11 bytes: 14 on the air after the SHR. */
static const uint8_t TXPSDU[11] = { 0x41, 0x88, 0x07, 0xCD, 0xAB, 0xFF, 0xFF, 0x02, 0x00, 'h', 'i' };
#define TX_AFTER_SHR (1 + 11 + 2)
static void load_tx(void) {
    strobe(CSP_ISFLUSHTX);
    wr(SFR(RFCORE_SFR_RFDATA), sizeof TXPSDU + 2);
    for (size_t i = 0; i < sizeof TXPSDU; i++) wr(SFR(RFCORE_SFR_RFDATA), TXPSDU[i]);
}

/* STXON: 192 us of TX calibration, then the preamble; the SFD signal, its
 * interrupt and the MAC timer capture once the SFD is out; TX_FRM_DONE at
 * the frame's end; TX final, the TX/RX transit, and SFD wait 192 us after
 * the frame, TX_ACTIVE set until then (SWRU319C §23.8.2, §23.8.11,
 * §12.1.10, Figure 23-18, Table 23-4). */
static void test_tx_timing(void) {
    setup();
    run_to_ns(2000000);
    load_tx();
    wr(SFR(RFCORE_SFR_RFIRQF0), 0);
    wr(SFR(RFCORE_SFR_RFIRQF1), 0);
    tx_n = 0;
    int64_t c0 = cpu->cycles;
    int64_t t0 = cpu_cycles_to_ns(c0, cpu->cpu_freq_hz);
    strobe(CSP_ISTXON);
    ASSERT_EQ(FSM0(), RF_STATE_TX_CALIBR, "STXON: TX calibration");
    ASSERT_EQ(FSM1() & 3, FSM1_TX_ACTIVE, "TX calibration: TX_ACTIVE, not RX_ACTIVE");
    run_to_ns(t0 + 191000);
    ASSERT_EQ(tx_n, 0, "nothing on the air 191 us after STXON");
    run_to_ns(t0 + 193000);
    ASSERT_EQ(tx_n, 5 + TX_AFTER_SHR, "the frame on the air 193 us after STXON");
    if (tx_n > 0) {
        int64_t lat = tx_ns[0] - t0;
        ASSERT(lat >= 192000 && lat < 192100, "the preamble starts 192 us after STXON");
        if (!(lat >= 192000 && lat < 192100)) printf("    latency = %lld ns\n", (long long)lat);
        ASSERT(tx_bytes[0] == 0 && tx_bytes[3] == 0 && tx_bytes[4] == 0x7A &&
               tx_bytes[5] == sizeof TXPSDU + 2, "preamble, SFD, length byte");
    }
    ASSERT_EQ(FSM0(), RF_STATE_TX, "FSMSTAT0 TX with the frame on the air");
    run_to_ns(t0 + 351000);
    ASSERT(!(FSM1() & FSM1_SFD), "no SFD signal before the SFD is out");
    ASSERT(!(rd(SFR(RFCORE_SFR_RFIRQF0)) & RFIRQF0_SFD), "no SFD interrupt before the SFD is out");
    run_to_ns(t0 + 353000);
    ASSERT(FSM1() & FSM1_SFD, "SFD signal once the SFD is out (352 us after STXON)");
    ASSERT(rd(SFR(RFCORE_SFR_RFIRQF0)) & RFIRQF0_SFD, "SFD interrupt once the SFD is out");
    int64_t want = mt_at_cycle(c0 + cpu_ns_to_cycles(352000, cpu->cpu_freq_hz));
    ASSERT_EQ(sfd_capture(), want, "MAC timer captures the SFD's end");
    int64_t end = t0 + 352000 + TX_AFTER_SHR * BYTE_NS;
    run_to_ns(end - 1000);
    ASSERT(!(rd(SFR(RFCORE_SFR_RFIRQF1)) & RFIRQF1_TXDONE), "no TXDONE before the frame's end");
    run_to_ns(end + 1000);
    ASSERT(rd(SFR(RFCORE_SFR_RFIRQF1)) & RFIRQF1_TXDONE, "TXDONE at the frame's end");
    ASSERT(!(FSM1() & FSM1_SFD), "SFD signal low after the frame");
    ASSERT_EQ(FSM0(), RF_STATE_TX_FINAL, "TX final for 2 us after the frame");
    run_to_ns(end + 3000);
    ASSERT_EQ(FSM0(), RF_STATE_TX_RX_TRANSIT, "then the TX/RX transit");
    ASSERT_EQ(FSM1() & 3, FSM1_TX_ACTIVE, "TX/RX transit: TX_ACTIVE");
    run_to_ns(end + 191000);
    ASSERT(FSM1() & FSM1_TX_ACTIVE, "TX_ACTIVE 191 us after the frame");
    run_to_ns(end + 193000);
    ASSERT_EQ(FSM0(), RF_STATE_SFD_WAIT, "SFD wait 192 us after the frame");
    ASSERT_EQ(FSM1() & 3, FSM1_RX_ACTIVE, "SFD wait: RX_ACTIVE, TX_ACTIVE clear");
}

/* STXON sets RXENABLE[6] when FRMCTRL1.SET_RXENMASK_ON_TX is set (reset
 * value), so a radio sent from IDLE returns to RX; without it, to IDLE 2 us
 * after the frame (§23.9.1, §23.8.2, FRMCTRL1). */
static void test_tx_from_idle(void) {
    setup();
    strobe(CSP_ISRFOFF);
    ASSERT_EQ(rd(XREG(RFCORE_XREG_RXENABLE)), 0, "SRFOFF clears RXENABLE");
    load_tx();
    int64_t t0 = cpu_cycles_to_ns(cpu->cycles, cpu->cpu_freq_hz);
    strobe(CSP_ISTXON);
    ASSERT(rd(XREG(RFCORE_XREG_RXENABLE)) & 0x40, "STXON sets RXENABLE[6] (SET_RXENMASK_ON_TX)");
    int64_t end = t0 + 352000 + TX_AFTER_SHR * BYTE_NS;
    run_to_ns(end + 193000);
    ASSERT_EQ(FSM0(), RF_STATE_SFD_WAIT, "from IDLE, SFD wait after the frame (RXENABLE[6])");

    setup();
    wr(XREG(RFCORE_XREG_FRMCTRL1), 0);
    strobe(CSP_ISRFOFF);
    load_tx();
    t0 = cpu_cycles_to_ns(cpu->cycles, cpu->cpu_freq_hz);
    strobe(CSP_ISTXON);
    ASSERT_EQ(rd(XREG(RFCORE_XREG_RXENABLE)), 0, "SET_RXENMASK_ON_TX clear: RXENABLE stays 0");
    end = t0 + 352000 + TX_AFTER_SHR * BYTE_NS;
    run_to_ns(end + 1000);
    ASSERT_EQ(FSM0(), RF_STATE_TX_FINAL, "RX disabled: TX final after the frame");
    run_to_ns(end + 3000);
    ASSERT_EQ(FSM0(), RF_STATE_IDLE, "RX disabled: IDLE 2 us after the frame");
    ASSERT(!(FSM1() & FSM1_TX_ACTIVE), "RX disabled: TX_ACTIVE clear in IDLE");
}

/* SRXON: RX calibration for 192 us, RX_ACTIVE set, deaf; then SFD wait.
 * The SFD search needs one zero byte ahead of the SFD (MDMCTRL0
 * DEM_NUM_ZEROS = 2 symbols), so a frame whose preamble began during the
 * calibration is still heard if a zero byte comes after it (§23.9.2,
 * Figure 23-18, MDMCTRL0). */
static void test_rx_calibration(void) {
    setup();
    uint8_t psdu[64];
    int n = build(psdu, FCF(1, 2) & ~0x0020, 0x0001, SRC_SHORT_2, 2, DATA, 3);
    int64_t t1 = 2000000;
    run_to_ns(t1);
    strobe(CSP_ISRXON);
    ASSERT_EQ(FSM0(), RF_STATE_RX_CALIBR, "SRXON: RX calibration");
    ASSERT_EQ(FSM1() & 3, FSM1_RX_ACTIVE, "RX calibration: RX_ACTIVE");
    deliver(t1, psdu, n);                             /* SFD at +128 us */
    ASSERT_EQ(rf->rxfifo_len, 0, "a frame whose SFD comes in the RX calibration is not received");
    run_to_ns(t1 + 191000);
    /* (the frame above is still arriving; the calibration ends regardless) */
    ASSERT(FSM0() == RF_STATE_RX_CALIBR || FSM0() == RF_STATE_SFD_WAIT, "calibrating at 191 us");

    int64_t t2 = 4000000;
    run_to_ns(t2);
    strobe(CSP_ISRXON);
    run_to_ns(t2 + 191000);
    ASSERT_EQ(FSM0(), RF_STATE_RX_CALIBR, "still calibrating 191 us after SRXON");
    run_to_ns(t2 + 193000);
    ASSERT_EQ(FSM0(), RF_STATE_SFD_WAIT, "SFD wait 192 us after SRXON");

    /* Preamble from 100 us: three zero bytes lost, the fourth heard. */
    int64_t t3 = 6000000;
    run_to_ns(t3);
    strobe(CSP_ISRXON);
    deliver(t3 + 100000, psdu, n);
    ASSERT_EQ(rf->rxfifo_len, 1 + n, "one zero byte after the calibration is enough for the SFD");

    /* Preamble from 72 us: all four zero bytes lost, the SFD first. */
    strobe(CSP_ISFLUSHRX);
    int64_t t4 = 8000000;
    run_to_ns(t4);
    strobe(CSP_ISRXON);
    deliver(t4 + 72000, psdu, n);
    ASSERT_EQ(rf->rxfifo_len, 0, "no zero byte before the SFD: not received");
}

/* The SFD signal goes high, its interrupt is raised and the MAC timer
 * captures when the SFD has been completely received (§23.9.5.2, Figure
 * 23-9, §12.1.10): as the length byte starts, one byte period after the
 * SFD byte reached the radio. */
static void test_rx_sfd_at_its_end(void) {
    setup();
    wr(SFR(RFCORE_SFR_RFIRQF0), 0);
    int64_t t = 2000000;
    for (int i = 0; i < 4; i++) { run_to_ns(t + i * BYTE_NS); cc2538_rfcore_receive_byte(rf, 0x00); }
    run_to_ns(t + 4 * BYTE_NS);
    cc2538_rfcore_receive_byte(rf, 0x7A);
    ASSERT(!(FSM1() & FSM1_SFD), "the SFD byte arriving: no SFD signal yet");
    ASSERT(!(rd(SFR(RFCORE_SFR_RFIRQF0)) & RFIRQF0_SFD), "the SFD byte arriving: no SFD interrupt yet");
    ASSERT_EQ(FSM0(), RF_STATE_SFD_WAIT, "the SFD byte arriving: still SFD wait");
    uint8_t psdu[64];
    int n = build(psdu, FCF(1, 2) & ~0x0020, 0x0001, SRC_SHORT_2, 2, DATA, 3);
    run_to_ns(t + 5 * BYTE_NS);
    int64_t c = cpu->cycles;
    cc2538_rfcore_receive_byte(rf, (uint8_t)n);
    ASSERT(FSM1() & FSM1_SFD, "SFD signal as the length byte starts");
    ASSERT(rd(SFR(RFCORE_SFR_RFIRQF0)) & RFIRQF0_SFD, "SFD interrupt as the length byte starts");
    ASSERT_EQ(FSM0(), RF_STATE_RX, "FSMSTAT0 RX (7) from the SFD's end");
    ASSERT_EQ(sfd_capture(), mt_at_cycle(c), "MAC timer captures the SFD's end");
    for (int i = 0; i < n; i++) {
        run_to_ns(t + (6 + i) * BYTE_NS);
        cc2538_rfcore_receive_byte(rf, psdu[i]);
    }
    ASSERT(!(FSM1() & FSM1_SFD), "SFD signal low once the frame is in");
    ASSERT(FSM1() & 0x40, "FIFOP once the frame is in");
}

/* SFLUSHRX in an RX state forces an RX calibration (Figure 23-18). */
static void test_flushrx_calibrates(void) {
    setup();
    int64_t t = 2000000;
    run_to_ns(t);
    strobe(CSP_ISFLUSHRX);
    ASSERT_EQ(FSM0(), RF_STATE_RX_CALIBR, "SFLUSHRX in SFD wait: RX calibration");
    uint8_t psdu[64];
    int n = build(psdu, FCF(1, 2) & ~0x0020, 0x0001, SRC_SHORT_2, 2, DATA, 3);
    deliver(t, psdu, n);
    ASSERT_EQ(rf->rxfifo_len, 0, "SFLUSHRX: deaf for the calibration");
    run_to_ns(t + 1000000);
    ASSERT_EQ(FSM0(), RF_STATE_SFD_WAIT, "SFLUSHRX: SFD wait after the calibration");
    strobe(CSP_ISRFOFF);
    strobe(CSP_ISFLUSHRX);
    ASSERT_EQ(rf->state, RF_STATE_IDLE, "SFLUSHRX in IDLE only flushes");
}

/* A strobe the core executes inside a slice ends its batch of
 * instructions, so a core that ran past its slice's end stops there and
 * waits for the kernel; one written from outside a slice (here, the test
 * harness, or the shell) leaves the core free to run. */
static void test_strobe_ends_batch(void) {
    setup();
    const uint32_t prog = 0x20001100u;
    arm_write16(cpu, prog + 0, 0x21E9);           /* movs r1, #0xE9 (ISTXON) */
    arm_write16(cpu, prog + 2, 0x6001);           /* str  r1, [r0]  (RFST)   */
    arm_write16(cpu, prog + 4, 0xE7FE);           /* b .                      */
    cpu->reg[0] = SFR(RFCORE_SFR_RFST);
    cpu->reg[ARM_PC] = prog;
    int64_t c0 = cpu->cycles;
    cpu->cycle_limit = c0 + 1;                    /* a slice already past its end */
    arm_step(cpu, 1000);
    int64_t used = cpu->cycles - c0;
    bool stopped = cpu->stopping;
    cpu->cycle_limit = INT64_MAX;
    cpu->stopping = false;
    ASSERT(stopped, "a strobe from the core stops its batch");
    ASSERT(used < 20, "the batch ends at the strobe, not 1000 instructions on");
    ASSERT_EQ(FSM0(), RF_STATE_TX_CALIBR, "the strobe took effect");
    strobe(CSP_ISRXON);                           /* from outside a slice */
    ASSERT(!cpu->stopping, "a strobe from outside a slice leaves the core free");
    int64_t c1 = cpu->cycles;
    run_to_ns(cpu_cycles_to_ns(c1, cpu->cpu_freq_hz) + 100000);
    ASSERT(cpu->cycles > c1, "the core runs after a strobe from outside");
}

int run_cc2538_rfcore_tests(int verbose) {
    (void)verbose;
    printf("=== CC2538 RF core: TX/RX timing, auto-ACK and source matching ===\n");
    passed = failed = 0;
    test_tx_timing();
    test_tx_from_idle();
    test_rx_calibration();
    test_rx_sfd_at_its_end();
    test_flushrx_calibrates();
    test_strobe_ends_batch();
    test_ack_timing_and_state();
    test_source_matching();
    test_ack_abort();
    test_deaf_during_ack();
    test_deaf_while_ack_on_air();
    test_no_ack_while_off();
    test_no_ack_without_filtering();
    if (plat_live) arm_platform_destroy(&plat);
    plat_live = false;
    printf("  %d passed, %d failed\n", passed, failed);
    return failed;
}
