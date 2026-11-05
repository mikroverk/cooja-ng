/*
 * energest_model — the datasheet power model behind the energest plugin's
 * energy estimate: per board, a supply voltage and a current per
 * component state, each with the document and table it came from (see
 * energest_model.c and docs/energest.md "Power model").
 *
 * It is an estimate, labelled as one wherever it is printed: the
 * simulator measures time in state exactly; the current drawn in a state
 * is a datasheet typical, and a node's draw is the sum over its components
 * (the datasheets' rows are not always separable — see the notes).
 */
#ifndef ENERGEST_MODEL_H
#define ENERGEST_MODEL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One default: `key` is "<component>.<state>" (a lane name such as "cpu",
 * "radio", "led0", "uart0") or "<class>.<state>" (class = led, timer,
 * gate: every lane of that kind).  `ma` is the typical current in mA. */
typedef struct energest_current {
    const char *key;
    double      ma;
    const char *source;     /* "SWRS041c 6.10 p.13", or an assumption */
} energest_current_t;

typedef struct energest_board_model {
    const char *board;                  /* board registry name ("sky") */
    const char *title;                  /* "Tmote Sky (MSP430F1611 + CC2420)" */
    double      supply_v;
    bool        verified;               /* every figure read off the cited
                                         * document when it was written */
    const energest_current_t *currents; /* NULL-key terminated */
    /* CC2420-style TX current by PA level (NULL = none): `levels` entries
     * of (level, mA), ascending, for a radio that reports its TX power
     * indicator (0..tx_level_max). */
    const int    *tx_levels;
    const double *tx_level_ma;
    int           tx_level_count;
    int           tx_level_max;
} energest_board_model_t;

/* The default model for a board, or NULL (no datasheet model: every
 * current 0 unless the config gives one). */
const energest_board_model_t *energest_model_for_board(const char *board);

/* The default current for `key` ("lane.state", then "class.state") on
 * `m` (may be NULL); *source gets its citation.  Returns false if none. */
bool energest_model_current(const energest_board_model_t *m, const char *lane,
                            const char *cls, const char *state, double *ma,
                            const char **source);

/* TX current at PA level `level` (linear between the datasheet's points);
 * false if the model has no level table. */
bool energest_model_tx_ma(const energest_board_model_t *m, int level, double *ma);

#ifdef __cplusplus
}
#endif

#endif /* ENERGEST_MODEL_H */
