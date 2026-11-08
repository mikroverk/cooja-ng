/*
 * CC2538 RF core: the receiver on and off (ISRXON's calibration, RSSI
 * valid, ISRFOFF), auto-ACK timing and state, source address matching and
 * the frame-pending bit (SWRU319C §23.9.6, §23.9.8).
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

/* The platform and the driver's register setup, receiver still off. */
static void setup_off(void) {
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
}

static void setup(void) {
    setup_off();
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

/* On-air bytes of a frame: preamble, SFD, length, PSDU. */
#define AIR_BYTES(n) (6 + (n))

/* Deliver on-air bytes [from, to) of the frame that starts at t0, one byte
 * period apart.  Returns the time the last one was delivered. */
static int64_t deliver_range(int64_t t0, const uint8_t *psdu, int n, int from, int to) {
    uint8_t air[140];
    int k = 0;
    for (int i = 0; i < 4; i++) air[k++] = 0x00;
    air[k++] = 0x7A;
    air[k++] = (uint8_t)n;
    memcpy(air + k, psdu, (size_t)n); k += n;
    if (to > k) to = k;
    int64_t t = t0;
    for (int i = from; i < to; i++) {
        t = t0 + i * BYTE_NS;
        run_to_ns(t);
        cc2538_rfcore_receive_byte(rf, air[i]);
    }
    return t;
}

/* Deliver preamble, SFD, length and PSDU from t0, one byte period apart.
 * Returns the time the last byte was delivered (its start on the air). */
static int64_t deliver(int64_t t0, const uint8_t *psdu, int n) {
    return deliver_range(t0, psdu, n, 0, AIR_BYTES(n));
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
    ASSERT(!(rd(XREG(RFCORE_XREG_FSMSTAT1)) & 2), "TX_ACTIVE clear after the ACK");
    ASSERT_EQ(rd(XREG(RFCORE_XREG_FSMSTAT0)), RF_STATE_SFD_WAIT, "back to SFD wait after the ACK");
    ASSERT(rd(SFR(RFCORE_SFR_RFIRQF1)) & RFIRQF1_TXACKDONE, "TXACKDONE after the ACK");

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

    /* After the ACK (11 bytes), back in SFD wait: the next frame is heard. */
    run_to_ns(ack_start + 11 * BYTE_NS + 1000);
    ASSERT_EQ(rd(XREG(RFCORE_XREG_FSMSTAT0)), RF_STATE_SFD_WAIT, "SFD wait after the ACK");
    fifo = rf->rxfifo_len;
    deliver(ack_start + 12 * BYTE_NS, other, m);
    ASSERT_EQ(rf->rxfifo_len, fifo + 1 + m, "a frame after the ACK is received");
}

/* No ACK from a radio the firmware has turned off (ISRFOFF): it does not
 * hear the frame at all. */
static void test_no_ack_while_off(void) {
    setup();
    strobe(CSP_ISRFOFF);
    uint8_t psdu[64];
    int n = build(psdu, FCF(3, 2), 0x0001, SRC_SHORT_2, 2, DATAREQ, 1);
    int64_t last = deliver(2000000, psdu, n);
    ASSERT_EQ(rf->rxfifo_len, 0, "radio off: the frame is not received");
    ASSERT(!(rd(SFR(RFCORE_SFR_RFIRQF0)) & RFIRQF0_RXPKTDONE), "radio off: no RXPKTDONE");
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

/* --- The receiver on and off --- */

/* A frame with no ACK request, so no auto-ACK gets in the way. */
static int plain_frame(uint8_t *psdu, uint8_t dsn) {
    int n = build(psdu, FCF(1, 2) & ~0x0020, 0x0001, SRC_SHORT_2, 2, DATA, 3);
    psdu[2] = dsn;
    uint16_t crc = 0;
    for (int i = 0; i < n - 2; i++) crc = ieee802154_crc_add_bitrev(crc, psdu[i]);
    psdu[n - 2] = ieee802154_bitrev((crc >> 8) & 0xFF);
    psdu[n - 1] = ieee802154_bitrev(crc & 0xFF);
    return n;
}

static bool got_frame(void) { return (rd(SFR(RFCORE_SFR_RFIRQF0)) & RFIRQF0_RXPKTDONE) != 0; }
static uint32_t fsm_state(void) { return rd(XREG(RFCORE_XREG_FSMSTAT0)) & 0x3F; }
static uint32_t rssi_valid(void) { return rd(XREG(RFCORE_XREG_RSSISTAT)) & 1; }
static uint32_t cca_bit(void) { return (rd(XREG(RFCORE_XREG_FSMSTAT1)) >> 4) & 1; }

#define T_ON 2000000LL   /* when these tests strobe ISRXON */

/* The receiver is off until the firmware turns it on: nothing is heard. */
static void test_off_until_rxon(void) {
    setup_off();
    uint8_t psdu[64];
    int n = plain_frame(psdu, 1);
    deliver(T_ON, psdu, n);
    ASSERT(!got_frame(), "never turned on: no frame");
    ASSERT_EQ(rf->rxfifo_len, 0, "never turned on: RXFIFO empty");
    ASSERT_EQ(rf->rx_bytes_dropped, AIR_BYTES(n), "never turned on: every byte dropped");
    ASSERT_EQ(fsm_state(), RF_STATE_IDLE, "never turned on: FSMSTAT0 idle");
}

/* ISRXON calibrates for 192 us: a frame whose SFD arrives in that window
 * is missed, one that starts after it is received. */
static void test_rx_calibration(void) {
    setup_off();
    run_to_ns(T_ON);
    strobe(CSP_ISRXON);
    ASSERT_EQ(fsm_state(), RF_STATE_RX_CALIBR, "ISRXON: calibrating");
    ASSERT_EQ(rssi_valid(), 0, "calibrating: RSSI not valid");
    ASSERT_EQ(cca_bit(), 0, "calibrating: CCA not asserted");
    uint8_t psdu[64];
    int n = plain_frame(psdu, 2);
    deliver(T_ON, psdu, n);                           /* SFD at +128 us */
    ASSERT(!got_frame(), "SFD during calibration: frame missed");
    ASSERT_EQ(fsm_state(), RF_STATE_SFD_WAIT, "after 192 us: SFD search");

    setup_off();
    run_to_ns(T_ON);
    strobe(CSP_ISRXON);
    n = plain_frame(psdu, 3);
    deliver(T_ON + 200000, psdu, n);
    ASSERT(got_frame(), "frame after calibration: received");
    ASSERT_EQ(rf->rxfifo_len, 1 + n, "frame after calibration: PHR + PSDU in RXFIFO");
    ASSERT_EQ(rf->rxfifo[3], 3, "frame after calibration: the DSN that was sent");
    ASSERT_EQ(rf->rxfifo[n] & 0x80, 0x80, "frame after calibration: CRC_OK");
    ASSERT_EQ(rf->rx_bytes_dropped, 0, "frame after calibration: nothing dropped");
}

/* RSSI_VALID, and the CCA bit with it, rises 8 symbol periods after SFD
 * search starts, and drops when the receiver is turned off. */
static void test_rssi_valid(void) {
    setup_off();
    run_to_ns(T_ON);
    strobe(CSP_ISRXON);
    run_to_ns(T_ON + 192000 + 100000);
    ASSERT_EQ(fsm_state(), RF_STATE_SFD_WAIT, "+292 us: SFD search");
    ASSERT_EQ(rssi_valid(), 0, "+292 us: RSSI not valid yet");
    run_to_ns(T_ON + 192000 + 130000);
    ASSERT_EQ(rssi_valid(), 1, "+322 us: RSSI valid");
    ASSERT_EQ(cca_bit(), 1, "RSSI valid: CCA reads clear");
    strobe(CSP_ISRFOFF);
    ASSERT_EQ(rssi_valid(), 0, "ISRFOFF: RSSI not valid");
    ASSERT_EQ(cca_bit(), 0, "ISRFOFF: CCA not asserted");
}

/* ISRFOFF stops reception, between frames and in the middle of one. */
static void test_rfoff_stops_reception(void) {
    setup();
    run_to_ns(T_ON);
    strobe(CSP_ISRFOFF);
    ASSERT_EQ(fsm_state(), RF_STATE_IDLE, "ISRFOFF: FSMSTAT0 idle");
    ASSERT_EQ(rd(XREG(RFCORE_XREG_FSMSTAT1)) & 3, 0, "ISRFOFF: RX_ACTIVE and TX_ACTIVE clear");
    uint8_t psdu[64];
    int n = plain_frame(psdu, 4);
    deliver(T_ON + 1000000, psdu, n);
    ASSERT(!got_frame(), "after ISRFOFF: frame not received");
    ASSERT_EQ(rf->rxfifo_len, 0, "after ISRFOFF: RXFIFO empty");

    /* Mid-frame: through the PHR and FCF, then the driver's off(). */
    setup();
    n = plain_frame(psdu, 5);
    int64_t t = deliver_range(T_ON, psdu, n, 0, 8);
    ASSERT_EQ(fsm_state(), RF_STATE_RX, "mid-frame: receiving");
    strobe(CSP_ISFLUSHRX);
    strobe(CSP_ISRFOFF);
    deliver_range(T_ON, psdu, n, 8, AIR_BYTES(n));
    ASSERT(!got_frame(), "ISRFOFF mid-frame: frame abandoned");
    /* The parser starts clean on the next ISRXON. */
    run_to_ns(t + 1000000);
    strobe(CSP_ISRXON);
    n = plain_frame(psdu, 6);
    deliver(t + 1000000 + 200000, psdu, n);
    ASSERT(got_frame(), "next ISRXON: next frame received");
    ASSERT_EQ(rf->rxfifo[3], 6, "next ISRXON: the new frame, not a splice");
}

/* ISRFOFF during calibration: the calibration's end does not turn the
 * receiver on after all.  A second ISRXON recalibrates. */
static void test_rfoff_during_calibration(void) {
    setup_off();
    run_to_ns(T_ON);
    strobe(CSP_ISRXON);
    run_to_ns(T_ON + 50000);
    strobe(CSP_ISRFOFF);
    run_to_ns(T_ON + 400000);
    ASSERT_EQ(fsm_state(), RF_STATE_IDLE, "ISRFOFF in calibration: stays idle");
    uint8_t psdu[64];
    int n = plain_frame(psdu, 7);
    deliver(T_ON + 400000, psdu, n);
    ASSERT(!got_frame(), "ISRFOFF in calibration: nothing received");

    setup();
    run_to_ns(T_ON);
    strobe(CSP_ISRXON);                               /* e.g. a channel change */
    ASSERT_EQ(fsm_state(), RF_STATE_RX_CALIBR, "second ISRXON: recalibrating");
    ASSERT_EQ(rssi_valid(), 0, "second ISRXON: RSSI not valid again");
}

/* --- RSSI and CCA from the channel's energy --- */

static int fake_dbm = -128;              /* what the channel holds */
static int fake_energy(void *user, int radio_idx, int64_t at_ns, int64_t span_ns) {
    (void)user; (void)radio_idx; (void)at_ns; (void)span_ns;
    return fake_dbm;
}

/* The receiver on, RSSI valid, and the channel's energy from fake_dbm. */
static void setup_listening(void) {
    setup_off();
    rf->energy_fn = fake_energy;
    fake_dbm = -128;
    run_to_ns(T_ON);
    strobe(CSP_ISRXON);
    run_to_ns(T_ON + 400000);
}

static int8_t rssi_reg(void) { return (int8_t)(rd(XREG(RFCORE_XREG_RSSI)) & 0xFF); }

/* RSSI = dBm + 73; CCA (mode 3, reset) is clear while RSSI is below
 * CCA_THR (Contiki-NG's -81 dBm is the reset 0xF8 = -8). */
static void test_cca_energy_threshold(void) {
    setup_listening();
    ASSERT_EQ(rssi_reg(), -100 + 73, "quiet channel: RSSI is the noise floor");
    ASSERT_EQ(cca_bit(), 1, "quiet channel: CCA clear");
    fake_dbm = -70;
    ASSERT_EQ(rssi_reg(), -70 + 73, "a -70 dBm signal: RSSI follows it");
    ASSERT_EQ(cca_bit(), 0, "a -70 dBm signal: above -81 dBm, CCA busy");
    fake_dbm = -90;
    ASSERT_EQ(cca_bit(), 1, "a -90 dBm signal: below -81 dBm, CCA clear");
    wr(XREG(RFCORE_XREG_CCACTRL0), (uint32_t)(uint8_t)(-95 + 73));
    ASSERT_EQ(cca_bit(), 0, "threshold lowered to -95 dBm: the -90 dBm signal is busy");

    /* Before RSSI is valid the RSSI register reads -128 and CCA is not clear. */
    setup_off();
    rf->energy_fn = fake_energy;
    fake_dbm = -128;
    run_to_ns(T_ON);
    strobe(CSP_ISRXON);
    ASSERT_EQ(rssi_reg(), -128, "calibrating: RSSI reads -128");
    ASSERT_EQ(cca_bit(), 0, "calibrating: CCA not clear even on a quiet channel");
}

/* CCACTRL1.CCA_MODE: 0 always clear; 1 energy only; 2 only "not receiving
 * a frame"; 3 both. */
static void test_cca_modes(void) {
    setup_listening();
    fake_dbm = -60;
    wr(XREG(RFCORE_XREG_CCACTRL1), 0x02);              /* mode 0 */
    ASSERT_EQ(cca_bit(), 1, "mode 0: clear despite the energy");
    wr(XREG(RFCORE_XREG_CCACTRL1), 0x0A);              /* mode 1 */
    ASSERT_EQ(cca_bit(), 0, "mode 1: busy on energy");
    wr(XREG(RFCORE_XREG_CCACTRL1), 0x12);              /* mode 2 */
    ASSERT_EQ(cca_bit(), 1, "mode 2: clear on energy alone");
    /* Receiving a frame (past its SFD) is busy in modes 2 and 3. */
    fake_dbm = -128;
    uint8_t psdu[64];
    int n = plain_frame(psdu, 9);
    deliver_range(T_ON + 500000, psdu, n, 0, 8);
    ASSERT_EQ(fsm_state(), RF_STATE_RX, "mid-frame: receiving");
    ASSERT_EQ(cca_bit(), 0, "mode 2: busy while receiving a frame");
    wr(XREG(RFCORE_XREG_CCACTRL1), 0x0A);              /* mode 1 */
    ASSERT_EQ(cca_bit(), 1, "mode 1: clear while receiving if the energy is low");
    wr(XREG(RFCORE_XREG_CCACTRL1), 0x1A);              /* mode 3 */
    ASSERT_EQ(cca_bit(), 0, "mode 3: busy while receiving");
}

/* ISTXONCCA transmits only on a clear channel, and latches SAMPLED_CCA. */
static void test_istxoncca(void) {
    setup_listening();
    fake_dbm = -60;
    int before = tx_n;
    strobe(CSP_ISTXONCCA);
    ASSERT_EQ(tx_n, before, "busy channel: ISTXONCCA sends nothing");
    ASSERT_EQ((rd(XREG(RFCORE_XREG_FSMSTAT1)) >> 3) & 1, 0, "busy channel: SAMPLED_CCA 0");
    ASSERT_EQ(rd(XREG(RFCORE_XREG_FSMSTAT1)) & 2, 0, "busy channel: no TX_ACTIVE");
    fake_dbm = -128;
    strobe(CSP_ISTXONCCA);
    ASSERT(tx_n > before, "clear channel: ISTXONCCA transmits");
    ASSERT_EQ((rd(XREG(RFCORE_XREG_FSMSTAT1)) >> 3) & 1, 1, "clear channel: SAMPLED_CCA 1");

    /* A receiver that is off: no CCA, no transmission. */
    setup_off();
    rf->energy_fn = fake_energy;
    fake_dbm = -128;
    before = tx_n;
    strobe(CSP_ISTXONCCA);
    ASSERT_EQ(tx_n, before, "receiver off: ISTXONCCA sends nothing");
}

int run_cc2538_rfcore_tests(int verbose) {
    (void)verbose;
    printf("=== CC2538 RF core: receiver on/off, auto-ACK and source matching ===\n");
    passed = failed = 0;
    test_off_until_rxon();
    test_rx_calibration();
    test_rssi_valid();
    test_rfoff_stops_reception();
    test_rfoff_during_calibration();
    test_cca_energy_threshold();
    test_cca_modes();
    test_istxoncca();
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
