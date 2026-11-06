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
    { ".sky",          "sky",             SIM_BOARD_KIND_MSP430, "MSP430/Sky",
      "Tmote Sky", "MSP430F1611", "CC2420 2.4 GHz", "TARGET=sky",
      NULL, "telosb", NULL, 0 },
    { ".z1",           "z1",              SIM_BOARD_KIND_MSP430, "MSP430/Z1",
      "Zolertia Z1", "MSP430F2617", "CC2420 2.4 GHz", "TARGET=z1",
      NULL, "z1", NULL, 0 },
    { ".msp430fr5969", "fr5969",          SIM_BOARD_KIND_MSP430, "MSP430/FR5969",
      "MSP-EXP430FR5969", "MSP430FR5969", "-", "-",
      NULL, NULL, NULL, 0 },
    /* ARM family.  (Labels are accurate per board now — the old runner
     * banner said "ARM/CC2538DK" for every ARM board.) */
    { ".cc2538dk",        "cc2538dk",        SIM_BOARD_KIND_ARM, "ARM/CC2538DK",
      "TI CC2538DK", "Cortex-M3", "802.15.4 2.4 GHz", "TARGET=cc2538dk",
      NULL, "cc2538dk", "ot-cc2538", SIM_OS_OPENTHREAD },
    { ".zoul-firefly",    "zoul-firefly",    SIM_BOARD_KIND_ARM, "ARM/Zoul-Firefly",
      "Zolertia Firefly", "Cortex-M3", "802.15.4 + CC1200 sub-GHz", "TARGET=zoul BOARD=firefly",
      NULL, "firefly", NULL, 0 },
    { ".nrf52840-dongle", "nrf52840-dongle", SIM_BOARD_KIND_ARM, "ARM/nRF52840-Dongle",
      "nRF52840 Dongle", "Cortex-M4F", "802.15.4 2.4 GHz", "TARGET=nrf52840 BOARD=dongle",
      "nrf52840dongle/nrf52840", "nrf52840dongle", NULL, 0 },
    { ".nrf52840-dk",     "nrf52840-dk",     SIM_BOARD_KIND_ARM, "ARM/nRF52840-DK",
      "nRF52840 DK", "Cortex-M4F", "802.15.4 2.4 GHz", "TARGET=nrf52840 BOARD=dk",
      "nrf52840dk/nrf52840", "nrf52840dk", "ot-nrf528xx",
      SIM_OS_ZEPHYR | SIM_OS_RIOT | SIM_OS_OPENTHREAD },
    /* .nrf52840 without board suffix: built with BOARD=dk (the default). */
    { ".nrf52840",        "nrf52840-dk",     SIM_BOARD_KIND_ARM, "ARM/nRF52840-DK",
      "nRF52840 DK", "Cortex-M4F", "802.15.4 2.4 GHz", "TARGET=nrf52840 BOARD=dk",
      "nrf52840dk/nrf52840", "nrf52840dk", "ot-nrf528xx",
      SIM_OS_ZEPHYR | SIM_OS_RIOT | SIM_OS_OPENTHREAD },
    { ".nrf54l15-dk",     "nrf54l15-dk",     SIM_BOARD_KIND_ARM, "ARM/nRF54L15-DK",
      "nRF54L15 DK", "Cortex-M33 + RV32E", "802.15.4 2.4 GHz", "TARGET=nrf BOARD=nrf54l15/dk",
      "nrf54l15dk/nrf54l15/cpuapp", NULL, NULL, 0 },
    { ".nrf54l15-xiao",   "nrf54l15-xiao",   SIM_BOARD_KIND_ARM, "ARM/nRF54L15-XIAO",
      "Seeed XIAO nRF54L15", "Cortex-M33 + RV32E", "802.15.4 2.4 GHz", "TARGET=nrf BOARD=nrf54l15/xiao",
      "xiao_nrf54l15/nrf54l15/cpuapp", NULL, NULL, 0 },
    { ".cc1352r1-launchpad", "cc1352r1-launchpad", SIM_BOARD_KIND_ARM, "ARM/CC1352R1-LaunchPad",
      "TI LAUNCHXL-CC1352R1", "Cortex-M4F", "sub-GHz + 2.4 GHz RF core", "TARGET=simplelink BOARD=launchpad/cc1352r1",
      "cc1352r1_launchxl", "cc1352-launchpad", NULL, SIM_OS_ZEPHYR | SIM_OS_RIOT },
    { ".cc1312r7-launchpad", "cc1312r7-launchpad", SIM_BOARD_KIND_ARM, "ARM/CC1312R7-LaunchPad",
      "TI LP-CC1312R7", "Cortex-M4F", "sub-GHz RF core", "TARGET=simplelink BOARD=launchpad/cc1312r7",
      "cc1312r7_lp", NULL, NULL, SIM_OS_ZEPHYR },
    /* Host-process motes */
    { ".cooja", NULL, SIM_BOARD_KIND_NATIVE, "Native/Cooja",
      "Cooja native mote", "host", "simulated", "TARGET=cooja", NULL, NULL, NULL, 0 },
    { ".js",    NULL, SIM_BOARD_KIND_JS,     "JS/QuickJS",
      "JavaScript app mote", "host", "simulated", "-", NULL, NULL, NULL, 0 },
    { ".py",    NULL, SIM_BOARD_KIND_EXTERNAL, "External/process",
      "External process", "host", "simulated", "-", NULL, NULL, NULL, 0 },
    /* Renode co-simulation device.  The path is never opened — the
     * extension only selects the kind, as ".py" does for external nodes. */
    { ".renode", NULL, SIM_BOARD_KIND_RENODE, "Renode/CoSim",
      "Renode device", "-", "-", "-", NULL, NULL, NULL, 0 },
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

/* The removed board an extension names, or NULL. */
static const char *removed_board(const char *dot) {
    for (size_t i = 0; dot && i < sizeof(removed_boards) / sizeof(removed_boards[0]); i++)
        if (strcmp(dot, removed_boards[i].extension) == 0)
            return removed_boards[i].board;
    return NULL;
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

const sim_board_desc_t *sim_board_resolve(const char *path, const char *board,
                                          char *err, size_t errlen) {
    const sim_board_desc_t *by_ext = sim_board_for_path(path);
    if (board && board[0]) {
        const sim_board_desc_t *named = sim_board_find(board);
        if (!named) {
            sim_board_unknown_error(board, err, errlen);
            return NULL;
        }
        /* A generic extension (".nrf52840", a SoC rather than a board)
         * names a default board; `board` may pick another board with that
         * SoC name, e.g. nrf52840-dongle.  Anything else contradicts it. */
        const char *ext = path ? path_extension(path) : NULL;
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
        const char *dot = path ? path_extension(path) : NULL;
        if (removed_board(dot)) {
            sim_board_path_error(path, err, errlen);
            return NULL;
        }
        static const char hint[] =
            "; name the board with 'board: <name>' on its mote type, or "
            "--board <name> for an image on the command line (cngsim boards "
            "lists them)";
        if (dot)
            snprintf(err, errlen, "firmware '%s': unknown extension '%s'%s",
                     path, dot, hint);
        else
            snprintf(err, errlen, "firmware '%s': no extension%s",
                     path ? path : "(none)", hint);
        return NULL;
    }
    return by_ext;
}

const sim_board_desc_t *sim_board_at(int i) {
    return (i >= 0 && i < BOARD_COUNT) ? &boards[i] : NULL;
}

void sim_board_path_error(const char *path, char *out, size_t outsz) {
    if (!out || outsz == 0) return;
    if (!path) path = "(none)";
    const char *dot = path_extension(path);
    const char *gone = removed_board(dot);
    if (gone) {
        snprintf(out, outsz,
                 "firmware '%s': the %s board (%s) was removed after "
                 "Cooja-NG 0.3.0; it had no radio model and no "
                 "Contiki-NG port", path, gone, dot);
        return;
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

/* Other spellings of a board's name, beyond the rows' zephyr/riot fields:
 * the bare Zephyr board name (what CONFIG_BOARD holds in a build's .config;
 * west takes it too where the board has one CPU), the target without its
 * CPU cluster, and the names Zephyr used before its hardware model v2
 * (3.7).  Only names of the same hardware: RIOT's cc1312-launchpad is the
 * CC1312R1, not the R7; Zephyr's nrf52840dk/nrf52811 runs the DK as an
 * nRF52811, nrf54l15dk/nrf54l15/cpuflpr is an image
 * for the RISC-V core and .../cpuapp/ns a Non-secure one that needs TF-M,
 * so neither is an alias.  OpenThread's port names are not either: one
 * port (ot-nrf528xx) builds for the DK and the dongle alike. */
static const sim_board_alias_t aliases[] = {
    { "nrf52840dk_nrf52840",        "nrf52840-dk",        "Zephyr (before 3.7)" },
    { "nrf52840dongle_nrf52840",    "nrf52840-dongle",    "Zephyr (before 3.7)" },
    { "nrf54l15dk",                 "nrf54l15-dk",        "Zephyr" },
    { "nrf54l15dk/nrf54l15",        "nrf54l15-dk",        "Zephyr" },
    { "xiao_nrf54l15",              "nrf54l15-xiao",      "Zephyr" },
    { "xiao_nrf54l15/nrf54l15",     "nrf54l15-xiao",      "Zephyr" },
    { "cc1352r1_launchxl/cc1352r",  "cc1352r1-launchpad", "Zephyr" },
    /* Not a typo for cc1312r7: this repository's board
     * (tools/zephyr-boards/boards/ti/cc1312r7_lp/board.yml) is built on
     * the cc1352r7 SoC, so that is its CONFIG_BOARD_TARGET, the name
     * zephyr-hello-world.cc1312r7-launchpad prints (CI checks it). */
    { "cc1312r7_lp/cc1352r7",       "cc1312r7-launchpad", "Zephyr" },
};

#define ALIAS_COUNT ((int)(sizeof(aliases) / sizeof(aliases[0])))

const sim_board_alias_t *sim_board_alias_at(int i) {
    return (i >= 0 && i < ALIAS_COUNT) ? &aliases[i] : NULL;
}

static const sim_board_desc_t *find_name(const char *name) {
    for (int i = 0; i < BOARD_COUNT; i++)
        if (boards[i].name && strcmp(name, boards[i].name) == 0)
            return &boards[i];
    return NULL;
}

const sim_board_desc_t *sim_board_find(const char *name) {
    if (!name) return NULL;
    const sim_board_desc_t *b = find_name(name);
    if (b) return b;
    for (int i = 0; i < BOARD_COUNT; i++) {
        if (!boards[i].name) continue;
        if ((boards[i].zephyr && strcmp(name, boards[i].zephyr) == 0) ||
            (boards[i].riot && strcmp(name, boards[i].riot) == 0))
            return find_name(boards[i].name);      /* the first row of that name */
    }
    for (int i = 0; i < ALIAS_COUNT; i++)
        if (strcmp(name, aliases[i].alias) == 0)
            return find_name(aliases[i].board);
    return NULL;
}

/* Lower case, letters and digits only: "nRF52840_DK" -> "nrf52840dk". */
static void squash(const char *s, char *out, size_t n) {
    size_t k = 0;
    for (; *s && k + 1 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[k++] = (char)c;
    }
    out[k] = '\0';
}

/* Levenshtein distance, capped: anything past `cap` is cap + 1. */
static int distance(const char *a, const char *b, int cap) {
    size_t la = strlen(a), lb = strlen(b);
    if (la > 63 || lb > 63) return cap + 1;
    if ((la > lb ? la - lb : lb - la) > (size_t)cap) return cap + 1;
    int row[64];
    for (size_t j = 0; j <= lb; j++) row[j] = (int)j;
    for (size_t i = 1; i <= la; i++) {
        int diag = row[0], best;
        row[0] = (int)i;
        best = row[0];
        for (size_t j = 1; j <= lb; j++) {
            int up = row[j];
            int v = diag + (a[i - 1] != b[j - 1]);
            if (row[j] + 1 < v) v = row[j] + 1;
            if (row[j - 1] + 1 < v) v = row[j - 1] + 1;
            row[j] = v;
            diag = up;
            if (v < best) best = v;
        }
        if (best > cap) return cap + 1;
    }
    return row[lb];
}

/* Is the name `cand` close to what was typed (already squashed)? */
static int close_to(const char *typed, const char *cand) {
    char c[96];
    squash(cand, c, sizeof(c));
    if (!typed[0] || !c[0]) return 0;
    if (strcmp(typed, c) == 0) return 1;
    /* a prefix of the name, or the name with something added ("nrf52840"
     * for either nRF52840 board; "sky-mote") -- from four characters, so
     * "cc" does not name every TI board */
    if (strlen(typed) >= 4 && (strstr(c, typed) == c || strstr(typed, c) == typed))
        return 1;
    return distance(typed, c, strlen(typed) < 6 ? 1 : 2) <= (strlen(typed) < 6 ? 1 : 2);
}

static void add_name(char *out, size_t outsz, const char *name) {
    size_t len = strlen(out), nl = strlen(name);
    /* already listed? (names are whole words between ", ") */
    for (const char *p = out; (p = strstr(p, name)) != NULL; p += nl)
        if ((p == out || p[-1] == ' ') && (p[nl] == ',' || p[nl] == '\0')) return;
    snprintf(out + len, outsz - len, "%s%s", len ? ", " : "", name);
}

void sim_board_suggest(const char *name, char *out, size_t outsz) {
    if (!out || outsz == 0) return;
    out[0] = '\0';
    if (!name) return;
    char typed[96];
    squash(name, typed, sizeof(typed));
    for (int i = 0; i < BOARD_COUNT; i++) {
        const sim_board_desc_t *b = &boards[i];
        if (!b->name) continue;
        if (close_to(typed, b->name) || (b->zephyr && close_to(typed, b->zephyr)) ||
            (b->riot && close_to(typed, b->riot)))
            add_name(out, outsz, b->name);
    }
    for (int i = 0; i < ALIAS_COUNT; i++)
        if (close_to(typed, aliases[i].alias)) add_name(out, outsz, aliases[i].board);
}

void sim_board_unknown_error(const char *name, char *out, size_t outsz) {
    if (!out || outsz == 0) return;
    char near[256];
    sim_board_suggest(name, near, sizeof(near));
    if (near[0])
        snprintf(out, outsz, "unknown board '%s'; did you mean %s? (cngsim boards "
                 "lists them)", name ? name : "", near);
    else
        snprintf(out, outsz, "unknown board '%s' (cngsim boards lists them, and "
                 "cngsim boards --os the Zephyr, RIOT and OpenThread names)",
                 name ? name : "");
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
