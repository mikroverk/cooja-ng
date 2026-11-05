/*
 * energest_model — datasheet defaults for the energest energy estimate.
 *
 * Every figure is a typical value from the document named next to it,
 * read off that document when this table was written, unless the board is
 * marked `verified = false` or the entry's source says "assumption".  A
 * config overrides any of them (energest.currents, docs/energest.md).
 *
 * Documents:
 *   SWRS041c  TI CC2420 datasheet, rev. c.  6.10 Power Supply (p.13-14);
 *             Table 9, output power settings and typical current @ 2.45 GHz
 *             (p.51).  Figures at the 3.0 V the Sky supplies.
 *   SLAS368G  TI MSP430F15x/16x/161x datasheet, rev. G (March 2011).
 *             MSP430F161x supply current table (p.27), VCC = 3 V, 25 C.
 *   SWRS096D  TI CC2538 datasheet, rev. D (April 2015).  5.4 Electrical
 *             Characteristics (p.10), VDD = 3 V, 25 C, EM reference design.
 *   nRF52840 PS  Nordic nRF52840 Product Specification v1.7, 5.2 Current
 *             consumption.  NOT verified: no copy of the document was at
 *             hand when this table was written; the figures are the
 *             commonly quoted ones and must be checked before use.
 *
 * Notes on what the rows mean:
 *   - Components add.  The CC2538's radio rows are "core current, CPU
 *     idle" (they include the SoC's own idle draw), so a CC2538 node with
 *     the radio on and the CPU asleep is charged the radio row plus the
 *     sleep row: an overestimate by about that sleep row.
 *   - The CC2420's "off" lane covers its VREG-off, power-down and idle
 *     states (the simulator's radio-state classes); it is charged nothing,
 *     like Contiki's Energest.  Idle would be 0.426 mA (SWRS041c 6.10).
 *   - LEDs: no datasheet gives a board's LED current (it is the board's
 *     series resistor); 0 until a config sets led.on.
 */
#include "energest_model.h"

#include <stddef.h>
#include <string.h>

/* --- Tmote Sky: MSP430F1611 + CC2420, 3.0 V ---------------------------- */
static const energest_current_t SKY_CURRENTS[] = {
    /* I(AM) = 500 uA/MHz x f(MCLK): Contiki-NG's Sky clocks MCLK from the
     * DCO at F_CPU = 3.9 MHz. */
    { "cpu.active",      1.95,    "SLAS368G p.27: I(AM) 500 uA at 1 MHz, x 3.9 MHz" },
    { "cpu.lpm0",        0.075,   "SLAS368G p.27: I(LPM0)" },
    { "cpu.lpm1",        0.075,   "assumption: LPM1 not in the table; I(LPM0)" },
    { "cpu.lpm2",        0.017,   "SLAS368G p.27: I(LPM2)" },
    { "cpu.lpm3",        0.0026,  "SLAS368G p.27: I(LPM3), 25 C" },
    { "cpu.lpm4",        0.0002,  "SLAS368G p.27: I(LPM4)" },
    { "radio.off",       0.0,     "SWRS041c 6.10: VREG off 0.02 uA (idle 426 uA uncharged, see notes)" },
    { "radio.listen",    18.8,    "SWRS041c 6.10 p.13: receive mode" },
    { "radio.rx",        18.8,    "SWRS041c 6.10 p.13: receive mode" },
    { "radio.interfered",18.8,    "SWRS041c 6.10 p.13: receive mode" },
    { "radio.tx",        17.4,    "SWRS041c Table 9 p.51: PA_LEVEL 31, 0 dBm" },
    { "led.on",          0.0,     "no datasheet figure (board resistor); set led.on" },
    { "timer.on",        0.0,     "included in the MCU mode currents (SLAS368G p.27 note 1)" },
    { NULL, 0, NULL }
};
/* SWRS041c Table 9: PA_LEVEL -> typical TX current. */
static const int    CC2420_PA_LEVEL[] = { 3,   7,   11,   15,   19,   23,   27,   31 };
static const double CC2420_PA_MA[]    = { 8.5, 9.9, 11.2, 12.5, 13.9, 15.2, 16.5, 17.4 };

/* --- CC2538 (CC2538DK, OpenMote), 3.0 V --------------------------------- */
static const energest_current_t CC2538_CURRENTS[] = {
    { "cpu.active",      13.0,    "SWRS096D 5.4 p.10: 32-MHz XOSC, CPU running with flash access" },
    { "cpu.sleep",       0.6,     "assumption: PM0 (WFI) not in the table; PM1's figure, a lower bound" },
    { "cpu.pm1",         0.6,     "SWRS096D 5.4 p.10: power mode 1" },
    { "cpu.pm2",         0.0013,  "SWRS096D 5.4 p.10: power mode 2" },
    { "cpu.pm3",         0.0004,  "SWRS096D 5.4 p.10: power mode 3" },
    { "radio.off",       0.0,     "radio off: in the CPU rows" },
    { "radio.listen",    24.0,    "SWRS096D 5.4 p.10: RX at -100 dBm (waiting for signal), CPU idle" },
    { "radio.rx",        20.0,    "SWRS096D 5.4 p.10: RX at -50 dBm input, CPU idle" },
    { "radio.interfered",20.0,    "SWRS096D 5.4 p.10: RX at -50 dBm input, CPU idle" },
    { "radio.tx",        24.0,    "SWRS096D 5.4 p.10: TX at 0 dBm, CPU idle (TXPOWER not modelled)" },
    { "gate.on",         0.0,     "no figure for this unit" },
    { "gpt0.on",         0.120,   "SWRS096D 5.4 p.10: general-purpose timer running" },
    { "gpt1.on",         0.120,   "SWRS096D 5.4 p.10: general-purpose timer running" },
    { "gpt2.on",         0.120,   "SWRS096D 5.4 p.10: general-purpose timer running" },
    { "gpt3.on",         0.120,   "SWRS096D 5.4 p.10: general-purpose timer running" },
    { "ssi0.on",         0.300,   "SWRS096D 5.4 p.10: SPI" },
    { "ssi1.on",         0.300,   "SWRS096D 5.4 p.10: SPI" },
    { "uart0.on",        0.700,   "SWRS096D 5.4 p.10: UART" },
    { "uart1.on",        0.700,   "SWRS096D 5.4 p.10: UART" },
    { "i2c.on",          0.100,   "SWRS096D 5.4 p.10: I2C" },
    { "rfc.on",          0.0,     "the radio is charged by its own lane" },
    { "led.on",          0.0,     "no datasheet figure (board resistor); set led.on" },
    { NULL, 0, NULL }
};

/* --- nRF52840 (DK, Dongle), 3.0 V: UNVERIFIED ------------------------------ */
static const energest_current_t NRF52840_CURRENTS[] = {
    { "cpu.active",      3.3,     "nRF52840 PS v1.7 5.2 (unverified): CPU from flash, cache, DC/DC" },
    { "cpu.sleep",       0.0032,  "nRF52840 PS v1.7 5.2 (unverified): System ON, RTC, full RAM" },
    { "radio.off",       0.0,     "radio disabled" },
    { "radio.listen",    4.8,     "nRF52840 PS v1.7 5.2 (unverified): IEEE 802.15.4 RX, DC/DC" },
    { "radio.rx",        4.8,     "nRF52840 PS v1.7 5.2 (unverified): IEEE 802.15.4 RX, DC/DC" },
    { "radio.interfered",4.8,     "nRF52840 PS v1.7 5.2 (unverified): IEEE 802.15.4 RX, DC/DC" },
    { "radio.tx",        4.8,     "nRF52840 PS v1.7 5.2 (unverified): TX 0 dBm, DC/DC (TXPOWER not modelled)" },
    { "timer.on",        0.0,     "no figure taken" },
    { "led.on",          0.0,     "no datasheet figure (board resistor); set led.on" },
    { NULL, 0, NULL }
};

static const energest_board_model_t MODELS[] = {
    { "sky", "Tmote Sky (MSP430F1611 + CC2420)", 3.0, true, SKY_CURRENTS,
      CC2420_PA_LEVEL, CC2420_PA_MA, 8, 31 },
    { "cc2538dk", "CC2538DK (CC2538)", 3.0, true, CC2538_CURRENTS, NULL, NULL, 0, 0 },
    { "openmote", "OpenMote (CC2538)", 3.0, true, CC2538_CURRENTS, NULL, NULL, 0, 0 },
    { "nrf52840-dk", "nRF52840-DK", 3.0, false, NRF52840_CURRENTS, NULL, NULL, 0, 0 },
    { "nrf52840-dongle", "nRF52840 Dongle", 3.0, false, NRF52840_CURRENTS, NULL, NULL, 0, 0 },
};

const energest_board_model_t *energest_model_for_board(const char *board) {
    if (!board) return NULL;
    for (size_t i = 0; i < sizeof MODELS / sizeof MODELS[0]; i++)
        if (strcmp(MODELS[i].board, board) == 0) return &MODELS[i];
    return NULL;
}

static const energest_current_t *find(const energest_current_t *t,
                                      const char *comp, const char *state) {
    size_t cl = strlen(comp);
    for (; t && t->key; t++)
        if (strncmp(t->key, comp, cl) == 0 && t->key[cl] == '.' &&
            strcmp(t->key + cl + 1, state) == 0)
            return t;
    return NULL;
}

bool energest_model_current(const energest_board_model_t *m, const char *lane,
                            const char *cls, const char *state, double *ma,
                            const char **source) {
    if (!m) return false;
    const energest_current_t *c = find(m->currents, lane, state);
    if (!c && cls) c = find(m->currents, cls, state);
    if (!c) return false;
    *ma = c->ma;
    if (source) *source = c->source;
    return true;
}

bool energest_model_tx_ma(const energest_board_model_t *m, int level, double *ma) {
    if (!m || !m->tx_levels || m->tx_level_count <= 0) return false;
    const int *L = m->tx_levels;
    const double *I = m->tx_level_ma;
    int n = m->tx_level_count;
    if (level <= L[0])     { *ma = I[0];     return true; }
    if (level >= L[n - 1]) { *ma = I[n - 1]; return true; }
    for (int i = 1; i < n; i++) {
        if (level <= L[i]) {
            double f = (double)(level - L[i - 1]) / (double)(L[i] - L[i - 1]);
            *ma = I[i - 1] + f * (I[i] - I[i - 1]);
            return true;
        }
    }
    *ma = I[n - 1];
    return true;
}
