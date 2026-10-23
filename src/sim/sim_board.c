/*
 * Built-in board registry — see include/sim/sim_board.h.
 *
 * One row per supported board.  The table is intentionally static
 * (refactor plan §7.1: static registry first, dlopen plugins only after
 * the API is stable).  Rows preserve the runner's historical extension
 * behavior exactly, including the ".nrf52840" → nrf52840-dk alias
 * (firmware built with the default BOARD=dk).  An unknown or missing
 * extension has no board: it used to run as a Tmote Sky, which turned a
 * typo or a removed board into a confusing failure inside the emulator.
 */
#include "sim_board.h"
#include "sim_registry.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const sim_board_desc_t boards[] = {
    /* MSP430 family */
    { ".sky",          "sky",             SIM_BOARD_KIND_MSP430, "MSP430/Sky"      },
    { ".z1",           "z1",              SIM_BOARD_KIND_MSP430, "MSP430/Z1"       },
    { ".msp430fr5969", "fr5969",          SIM_BOARD_KIND_MSP430, "MSP430/FR5969"   },
    /* ARM family.  (Labels are accurate per board now — the old runner
     * banner said "ARM/CC2538DK" for every ARM board.) */
    { ".cc2538dk",        "cc2538dk",        SIM_BOARD_KIND_ARM, "ARM/CC2538DK"        },
    { ".zoul-firefly",    "zoul-firefly",    SIM_BOARD_KIND_ARM, "ARM/Zoul-Firefly"    },
    { ".nrf52840-dongle", "nrf52840-dongle", SIM_BOARD_KIND_ARM, "ARM/nRF52840-Dongle" },
    { ".nrf52840-dk",     "nrf52840-dk",     SIM_BOARD_KIND_ARM, "ARM/nRF52840-DK"     },
    /* .nrf52840 without board suffix: built with BOARD=dk (the default). */
    { ".nrf52840",        "nrf52840-dk",     SIM_BOARD_KIND_ARM, "ARM/nRF52840-DK"     },
    { ".nrf54l15-dk",     "nrf54l15-dk",     SIM_BOARD_KIND_ARM, "ARM/nRF54L15-DK"     },
    { ".nrf54l15-xiao",   "nrf54l15-xiao",   SIM_BOARD_KIND_ARM, "ARM/nRF54L15-XIAO"   },
    { ".cc1352r1-launchpad", "cc1352r1-launchpad", SIM_BOARD_KIND_ARM, "ARM/CC1352R1-LaunchPad" },
    { ".cc1312r7-launchpad", "cc1312r7-launchpad", SIM_BOARD_KIND_ARM, "ARM/CC1312R7-LaunchPad" },
    /* Host-process motes */
    { ".cooja", NULL, SIM_BOARD_KIND_NATIVE, "Native/Cooja" },
    { ".js",    NULL, SIM_BOARD_KIND_JS,     "JS/QuickJS"   },
    { ".py",    NULL, SIM_BOARD_KIND_EXTERNAL, "External/process" },
    /* Renode co-simulation device.  The path is never opened — the
     * extension only selects the kind, as ".py" does for external nodes. */
    { ".renode", NULL, SIM_BOARD_KIND_RENODE, "Renode/CoSim" },
};

#define BOARD_COUNT ((int)(sizeof(boards) / sizeof(boards[0])))

/* Boards that have been removed, so their extension says so instead of
 * "unknown".  None had a radio model or a Contiki-NG port. */
static const struct { const char *extension, *board; } removed_boards[] = {
    { ".esb",     "ETH ESB"     },
    { ".wismote", "WisMote"     },
    { ".exp5438", "MSP-EXP5438" },
};

/* The extension of the path's basename, or NULL.  Keyed on the basename,
 * not the last dot in the whole path: a dotted directory (e.g.
 * build.out/firmware) is not an extension. */
static const char *path_extension(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return strrchr(base, '.');
}

const sim_board_desc_t *sim_board_for_path(const char *path) {
    if (!path) return NULL;
    const char *dot = path_extension(path);
    if (!dot) return NULL;
    for (int i = 0; i < BOARD_COUNT; i++) {
        if (strcmp(dot, boards[i].extension) == 0)
            return &boards[i];
    }
    return NULL;
}

void sim_board_path_error(const char *path, char *out, size_t outsz) {
    if (!out || outsz == 0) return;
    if (!path) path = "(none)";
    const char *dot = path_extension(path);
    for (size_t i = 0; dot && i < sizeof(removed_boards) / sizeof(removed_boards[0]); i++) {
        if (strcmp(dot, removed_boards[i].extension) == 0) {
            snprintf(out, outsz,
                     "firmware '%s': the %s board (%s) was removed after "
                     "Cooja-NG 0.3.0; it had no radio model and no "
                     "Contiki-NG port", path, removed_boards[i].board, dot);
            return;
        }
    }
    int n = dot ? snprintf(out, outsz, "firmware '%s': unknown extension '%s'", path, dot)
                : snprintf(out, outsz, "firmware '%s': no extension", path);
    if (n < 0 || (size_t)n >= outsz) return;
    n += snprintf(out + n, outsz - (size_t)n,
                  " (the extension selects the board; known:");
    for (int i = 0; i < BOARD_COUNT && n > 0 && (size_t)n < outsz; i++)
        n += snprintf(out + n, outsz - (size_t)n, " %s", boards[i].extension);
    if (n > 0 && (size_t)n < outsz)
        snprintf(out + n, outsz - (size_t)n, ")");
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
