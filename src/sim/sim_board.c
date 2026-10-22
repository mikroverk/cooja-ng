/*
 * Built-in board registry — see include/sim/sim_board.h.
 *
 * One row per supported board.  The table is intentionally static
 * (refactor plan §7.1: static registry first, dlopen plugins only after
 * the API is stable).  Rows preserve the runner's historical extension
 * behavior exactly, including the ".nrf52840" → nrf52840-dk alias
 * (firmware built with the default BOARD=dk) and the Tmote Sky default
 * for unknown extensions.
 */
#include "sim_board.h"
#include "sim_registry.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const sim_board_desc_t boards[] = {
    /* MSP430 family */
    { ".sky",          "sky",             SIM_BOARD_KIND_MSP430, "MSP430/Sky",
      "Tmote Sky", "MSP430F1611", "CC2420 2.4 GHz", "TARGET=sky" },
    { ".z1",           "z1",              SIM_BOARD_KIND_MSP430, "MSP430/Z1",
      "Zolertia Z1", "MSP430F2617", "CC2420 2.4 GHz", "TARGET=z1" },
    { ".esb",          "esb",             SIM_BOARD_KIND_MSP430, "MSP430/ESB",
      "ETH ESB", "MSP430F149", "-", "TARGET=esb" },
    { ".wismote",      "wismote",         SIM_BOARD_KIND_MSP430, "MSP430/WisMote",
      "WisMote", "MSP430F5437", "-", "TARGET=wismote" },
    { ".exp5438",      "exp5438",         SIM_BOARD_KIND_MSP430, "MSP430/exp5438",
      "MSP-EXP430F5438", "MSP430F5437", "-", "TARGET=exp5438" },
    { ".msp430fr5969", "fr5969",          SIM_BOARD_KIND_MSP430, "MSP430/FR5969",
      "MSP-EXP430FR5969", "MSP430FR5969", "-", "-" },
    /* ARM family.  (Labels are accurate per board now — the old runner
     * banner said "ARM/CC2538DK" for every ARM board.) */
    { ".cc2538dk",        "cc2538dk",        SIM_BOARD_KIND_ARM, "ARM/CC2538DK",
      "TI CC2538DK", "Cortex-M3", "802.15.4 2.4 GHz", "TARGET=cc2538dk" },
    { ".zoul-firefly",    "zoul-firefly",    SIM_BOARD_KIND_ARM, "ARM/Zoul-Firefly",
      "Zolertia Firefly", "Cortex-M3", "802.15.4 + CC1200 sub-GHz", "TARGET=zoul BOARD=firefly" },
    { ".nrf52840-dongle", "nrf52840-dongle", SIM_BOARD_KIND_ARM, "ARM/nRF52840-Dongle",
      "nRF52840 Dongle", "Cortex-M4F", "802.15.4 2.4 GHz", "TARGET=nrf52840 BOARD=dongle" },
    { ".nrf52840-dk",     "nrf52840-dk",     SIM_BOARD_KIND_ARM, "ARM/nRF52840-DK",
      "nRF52840 DK", "Cortex-M4F", "802.15.4 2.4 GHz", "TARGET=nrf52840 BOARD=dk" },
    /* .nrf52840 without board suffix: built with BOARD=dk (the default). */
    { ".nrf52840",        "nrf52840-dk",     SIM_BOARD_KIND_ARM, "ARM/nRF52840-DK",
      "nRF52840 DK", "Cortex-M4F", "802.15.4 2.4 GHz", "TARGET=nrf52840 BOARD=dk" },
    { ".nrf54l15-dk",     "nrf54l15-dk",     SIM_BOARD_KIND_ARM, "ARM/nRF54L15-DK",
      "nRF54L15 DK", "Cortex-M33 + RV32E", "802.15.4 2.4 GHz", "TARGET=nrf BOARD=nrf54l15/dk" },
    { ".nrf54l15-xiao",   "nrf54l15-xiao",   SIM_BOARD_KIND_ARM, "ARM/nRF54L15-XIAO",
      "Seeed XIAO nRF54L15", "Cortex-M33 + RV32E", "802.15.4 2.4 GHz", "TARGET=nrf BOARD=nrf54l15/xiao" },
    { ".cc1352r1-launchpad", "cc1352r1-launchpad", SIM_BOARD_KIND_ARM, "ARM/CC1352R1-LaunchPad",
      "TI LAUNCHXL-CC1352R1", "Cortex-M4F", "sub-GHz + 2.4 GHz RF core", "TARGET=simplelink BOARD=launchpad/cc1352r1" },
    { ".cc1312r7-launchpad", "cc1312r7-launchpad", SIM_BOARD_KIND_ARM, "ARM/CC1312R7-LaunchPad",
      "TI LP-CC1312R7", "Cortex-M4F", "sub-GHz RF core", "TARGET=simplelink BOARD=launchpad/cc1312r7" },
    /* Host-process motes */
    { ".cooja", NULL, SIM_BOARD_KIND_NATIVE, "Native/Cooja",
      "Cooja native mote", "host", "simulated", "TARGET=cooja" },
    { ".js",    NULL, SIM_BOARD_KIND_JS,     "JS/QuickJS",
      "JavaScript app mote", "host", "simulated", "-" },
    { ".py",    NULL, SIM_BOARD_KIND_EXTERNAL, "External/process",
      "External process", "host", "simulated", "-" },
    /* Renode co-simulation device.  The path is never opened — the
     * extension only selects the kind, as ".py" does for external nodes. */
    { ".renode", NULL, SIM_BOARD_KIND_RENODE, "Renode/CoSim",
      "Renode device", "-", "-", "-" },
};

#define BOARD_COUNT ((int)(sizeof(boards) / sizeof(boards[0])))

/* The board a firmware path's extension names, or NULL.  An unknown or
 * missing extension used to fall back to the Tmote Sky, so an ARM image
 * named .elf booted silently as an MSP430 and did nothing; now it is the
 * caller's error to report (sim_board_resolve says what to do). */
const sim_board_desc_t *sim_board_for_path(const char *path) {
    if (!path) return NULL;
    /* Key on the basename's extension, not the last dot in the whole path —
     * a dotted directory (e.g. build.out/firmware) otherwise resolved to a
     * bogus "extension". */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    if (!dot) return NULL;
    for (int i = 0; i < BOARD_COUNT; i++) {
        if (strcmp(dot, boards[i].extension) == 0)
            return &boards[i];
    }
    return NULL;
}

const sim_board_desc_t *sim_board_resolve(const char *path, const char *board,
                                          char *err, size_t errlen) {
    const sim_board_desc_t *by_ext = sim_board_for_path(path);
    if (board && board[0]) {
        const sim_board_desc_t *named = sim_board_find(board);
        if (!named) {
            snprintf(err, errlen, "unknown board '%s' (cngsim boards lists them)",
                     board);
            return NULL;
        }
        /* A generic extension (".nrf52840", a SoC rather than a board)
         * names a default board; `board` may pick another board with that
         * SoC name, e.g. nrf52840-dongle.  Anything else contradicts it. */
        const char *ext = path ? strrchr(path, '.') : NULL;
        size_t el = ext ? strlen(ext + 1) : 0;
        int variant = by_ext && by_ext->name && named->name && el > 0 &&
                      strcmp(ext, by_ext->extension) == 0 &&
                      strcmp(ext + 1, by_ext->name) != 0 &&
                      strncmp(named->name, ext + 1, el) == 0 &&
                      named->name[el] == '-';
        if (by_ext && !variant && by_ext->name != named->name &&
            (!by_ext->name || strcmp(by_ext->name, named->name) != 0)) {
            snprintf(err, errlen, "%s: the file name says %s, but board is '%s'",
                     path, by_ext->name ? by_ext->name : by_ext->label, board);
            return NULL;
        }
        return named;
    }
    if (!by_ext) {
        const char *base = path ? strrchr(path, '/') : NULL;
        base = base ? base + 1 : (path ? path : "");
        const char *dot = strrchr(base, '.');
        snprintf(err, errlen,
                 "%s: no board for the extension '%s'; name the board with "
                 "'board: <name>' on its mote type, or --board <name> for an "
                 "image on the command line (cngsim boards lists them)",
                 path ? path : "(none)", dot ? dot : "(none)");
        return NULL;
    }
    return by_ext;
}

const sim_board_desc_t *sim_board_at(int i) {
    return (i >= 0 && i < BOARD_COUNT) ? &boards[i] : NULL;
}

const sim_board_desc_t *sim_board_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < BOARD_COUNT; i++) {
        if (boards[i].name && strcmp(name, boards[i].name) == 0)
            return &boards[i];
    }
    return NULL;
}

/* --- Registry glue (Phase 8 M45).  The registry references this table; the
 * accessors forward to the bodies above. --- */

void csim_register_builtin_platforms(sim_registry_t *r) {
    if (!r) return;
    r->boards = boards;
    r->board_count = BOARD_COUNT;
}

const sim_board_desc_t *sim_registry_board_for_path(const sim_registry_t *r,
                                                    const char *path) {
    (void)r;
    return sim_board_for_path(path);
}

const sim_board_desc_t *sim_registry_board_find(const sim_registry_t *r,
                                                const char *name) {
    (void)r;
    return sim_board_find(name);
}
