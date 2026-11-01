/*
 * CC2538 RF core: auto-ACK timing and state, source address matching and
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

/* Deliver preamble, SFD, length and PSDU from t0, one byte period apart.
 * Returns the time the last byte was delivered (its start on the air). */
static int64_t deliver(int64_t t0, const uint8_t *psdu, int n) {
    uint8_t air[140];
    int k = 0;
    for (int i = 0; i < 4; i++) air[k++] = 0x00;
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
    ASSERT((rd(XREG(RFCORE_XREG_FSMSTAT1)) & 3) == 2, "FSMSTAT1: TX_ACTIVE, not RX_ACTIVE, in the turnaround");
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
    ASSERT_EQ(tx_bytes[a + 6], 0x12, "ACK FCF: frame pending (Data Request from an auto-pending entry)");
    ASSERT_EQ(tx_bytes[a + 8], 0x55, "ACK DSN");

    /* On the air: ACK states, TX_ACTIVE until the ACK's 352 us are over. */
    ASSERT_EQ(rd(XREG(RFCORE_XREG_FSMSTAT0)), RF_STATE_ACK, "FSMSTAT0 while the ACK is on the air");
    ASSERT(rd(XREG(RFCORE_XREG_FSMSTAT1)) & 2, "TX_ACTIVE while the ACK is on the air");
    ASSERT(!(rd(SFR(RFCORE_SFR_RFIRQF1)) & RFIRQF1_TXACKDONE), "no TXACKDONE before the ACK has been sent");
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
    ASSERT(a >= 0 && tx_bytes[a + 6] == 0x02, "data frame from a pending entry: ACK without frame pending");
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

int run_cc2538_rfcore_tests(int verbose) {
    (void)verbose;
    printf("=== CC2538 RF core: auto-ACK and source matching ===\n");
    passed = failed = 0;
    test_ack_timing_and_state();
    test_source_matching();
    test_ack_abort();
    test_deaf_during_ack();
    if (plat_live) arm_platform_destroy(&plat);
    plat_live = false;
    printf("  %d passed, %d failed\n", passed, failed);
    return failed;
}
