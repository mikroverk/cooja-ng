/*
 * sim_board — the board/platform registry (Phase 3 of the refactor,
 * docs/design/refactor-plan.md §9 Phase 3).
 *
 * Owns the firmware-extension → board mapping that previously lived as
 * four separate strcmp ladders in the runner (detect_node_type, the
 * MSP430/ARM platform-name derivations, and the init banner label).
 * The filename extension stays a compatibility input — config/CLI
 * platform overrides can layer on sim_board_find() later.
 *
 * Phase 4 (§3.17, M23): `kind` binds to a `sim_mote_kind_t` registry
 * row in src/motes/mote_kinds.c — boot policy, radio registration,
 * and (for native/JS) the sim_mote_ops_t table hang off the row.  The
 * MSP430/ARM ops tables are runner-injected until their execute/serial
 * adapters follow their dependencies (radio bus Phase 5, GDB service
 * Phase 6).  Phase 8 folds both registries into sim_registry.
 */
#ifndef SIM_BOARD_H
#define SIM_BOARD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum sim_board_kind {
    SIM_BOARD_KIND_MSP430 = 0,
    SIM_BOARD_KIND_ARM,
    SIM_BOARD_KIND_NATIVE,
    SIM_BOARD_KIND_JS,
    SIM_BOARD_KIND_EXTERNAL,
    SIM_BOARD_KIND_RENODE,
} sim_board_kind_t;

typedef struct sim_board_desc {
    const char *extension;     /* ".sky" — filename compatibility input  */
    const char *name;          /* registry key, also the arch platform
                                * lookup name (msp430_platform_find /
                                * arm_platform_find); NULL for native/JS */
    sim_board_kind_t kind;     /* which mote adapter family runs it      */
    const char *label;         /* "MSP430/Sky" — init banner             */
    /* For people (cngsim boards): what the board is and how to build for it. */
    const char *title;         /* "Tmote Sky"                            */
    const char *cpu;           /* "MSP430F1611"                          */
    const char *radio;         /* "CC2420 2.4 GHz"                       */
    const char *build;         /* Contiki-NG make arguments, or "-"      */
    /* The board's name in the other systems that build for it (NULL: none
     * builds for it, or none that is known to run here).  `board:` and
     * --board accept them; `cngsim boards --os` lists them. */
    const char *zephyr;        /* west build -b, "nrf52840dk/nrf52840"   */
    const char *riot;          /* RIOT BOARD=, "nrf52840dk"              */
    const char *openthread;    /* the OpenThread port repo, "ot-cc2538"  */
    unsigned runs;             /* SIM_OS_* bits: a bundled image of that
                                * system runs on this board here          */
} sim_board_desc_t;

#define SIM_OS_ZEPHYR     1u
#define SIM_OS_RIOT       2u
#define SIM_OS_OPENTHREAD 4u

/* Board for a firmware path, by the extension of its basename, or NULL
 * when the extension is missing or unknown (there is no default board;
 * sim_board_path_error() says why). */
const sim_board_desc_t *sim_board_for_path(const char *path);

/* The error for a path sim_board_for_path() rejects, naming the file: a
 * removed board's extension says so; otherwise it lists the known
 * extensions.  Always NUL-terminates `out` (truncating if needed). */
void sim_board_path_error(const char *path, char *out, size_t outsz);

/* The board a node runs: the named `board` (a mote type's "board:" key)
 * when given, else the path's extension.  NULL with a message in `err`
 * for an unknown name, an unknown extension without a name, or a name
 * that contradicts a known extension. */
const sim_board_desc_t *sim_board_resolve(const char *path, const char *board,
                                          char *err, size_t errlen);

/* Registry row i (0-based), or NULL past the end — for listing boards. */
const sim_board_desc_t *sim_board_at(int i);

/* Board by registry name (e.g. "zoul-firefly"), or by a name another
 * system gives it (Zephyr's "nrf52840dk/nrf52840", RIOT's "firefly"; see
 * sim_board_alias_at), or NULL.  For explicit platform overrides from
 * config/CLI; the row returned always carries the registry name. */
const sim_board_desc_t *sim_board_find(const char *name);

/* Alias i (0-based): a name another system gives a board, the registry
 * name it stands for, and whose name it is ("Zephyr").  NULL past the end.
 * The rows' zephyr/riot fields are aliases too; these are the rest (short
 * and older spellings). */
typedef struct sim_board_alias {
    const char *alias, *board, *system;
} sim_board_alias_t;
const sim_board_alias_t *sim_board_alias_at(int i);

/* For an unknown board name: the registry names it most likely meant
 * (a different case or punctuation, a prefix, a typo of one or two
 * letters), comma-separated in `out`, or "" when nothing is close.
 * Always NUL-terminates `out`. */
void sim_board_suggest(const char *name, char *out, size_t outsz);

/* The message for an unknown board name, with sim_board_suggest's names
 * when there are any: "unknown board 'x'; did you mean a, b? (cngsim
 * boards lists them)". */
void sim_board_unknown_error(const char *name, char *out, size_t outsz);

#ifdef __cplusplus
}
#endif

#endif /* SIM_BOARD_H */
