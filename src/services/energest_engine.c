/*
 * energest_engine — see energest_engine.h and docs/energest.md.
 *
 * Accounting: every lane (one component of one node) is in exactly one
 * state at a time.  A transition at t closes the open dwell [since, t) and
 * credits it to the state it was in — the time to total_ns[state], its
 * charge (mA x ns) to q[state] at the current the power model gives that
 * state.  Nothing is sampled, so the totals are exact to the stamps the
 * models give, and a lane's states sum to the time it existed.
 *
 * Timelines: the same dwells are also cut into fixed bins (bin_ns, default
 * 10 ms of simulated time) for the UI.  A frame covers frame_ns (default
 * 1 s) of bins and is emitted once simulated time is a full frame past its
 * end, so a node whose clock lags the others (each runs its own slice)
 * still lands in the right bin; a stamp older than that is folded into the
 * oldest bin still open and counted (late_bins).
 */
#include "energest_engine.h"
#include "energest_model.h"
#include "sim_event_queue.h" /* SIM_EQ_MAX_NODES: the kernel's slot count */
#include "cJSON.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NODES      SIM_EQ_MAX_NODES
#define MAX_LANES      28
#define MAX_STATES     10
#define MAX_OVERRIDES  128
/* A TX dwell may end this much before its frame's last byte (the stamps'
 * granularity: a CPU cycle or two) before the TX check fails. */
#define TX_SHORT_NS    1000

/* Lane kinds: the two built-in lanes, then SIM_COMP_* for peripherals. */
enum { LK_CPU = 100, LK_RADIO = 101 };

static const char *const CPU_STATES[SIM_CPU_MODE_COUNT] = {
    "active", "lpm0", "lpm1", "lpm2", "lpm3", "lpm4", "sleep", "pm1", "pm2", "pm3"
};
/* Indexed by sim_radio_state_t (ui/sim_state.h): OFF ON TX RX INTF. */
static const char *const RADIO_STATES[5] = { "off", "listen", "tx", "rx", "interfered" };
enum { R_OFF = 0, R_LISTEN = 1, R_TX = 2 };
static const char *const ONOFF_STATES[2] = { "off", "on" };

typedef struct lane {
    int      kind, index;
    char     name[24];
    const char *cls;               /* "cpu", "radio", "led", "timer", "gate" */
    int      nstates;
    const char *const *snames;
    int      cur;
    int64_t  since;                /* open dwell start (totals)            */
    int64_t  bin_since;            /* binned up to here                     */
    int64_t  entered;              /* the current state began               */
    int64_t  total_ns[MAX_STATES];
    double   q[MAX_STATES];        /* charge, mA x ns                       */
    /* The same, split at the end of the frame emitted next (win_t0 +
     * frame_ns): a frame is emitted a frame late, so by then total_ns and q
     * hold time past its end.  edge_* is the total to that end, next_* the
     * time after it (in the ring's second frame); a frame's totals are
     * edge_* plus the open dwell up to its end, then next_* moves over. */
    int64_t  edge_ns[MAX_STATES], next_ns[MAX_STATES];
    double   edge_q[MAX_STATES], next_q[MAX_STATES];
    double   ma[MAX_STATES];       /* current per state (model / config)    */
    const char *src[MAX_STATES];   /* where each current came from          */
    int64_t  alive_before;         /* the node's alive time before this lane
                                    * existed (lanes open lazily)           */
    int32_t *bins;                 /* [nstates][2 * bins_per_frame] ns      */
} lane_t;

typedef struct node {
    bool     seen;
    int      id;
    char     board[32];
    const energest_board_model_t *model;
    const char *radio_approx;      /* why its radio lane is approximate, or NULL */
    double   supply_v;
    bool     alive;
    int64_t  life_t0;              /* current life began                    */
    int64_t  alive_ns;             /* closed lives' total                   */
    int64_t  gone_ns;              /* removed at (when !alive)              */
    int      tx_level;             /* PA level (-1 = unknown: the 0 dBm row) */
    int      nlanes;
    lane_t   lanes[MAX_LANES];
    /* cross-checks */
    int      frames, acks;
    int64_t  air_ns, air_bytes;
    int64_t  air_bytes_ns;         /* sum of air bytes x the byte period    */
    /* TX dwell vs the air time of the frame sent in it, per frame class
     * (0 = data, 1 = an ACK-sized frame of 5 bytes): the radio's own
     * overhead around the bytes on air (calibration, turnaround). */
    int64_t  pend_air_ns;          /* air time of the frame in the open TX dwell */
    int      pend_class;           /* -1 = none */
    int      matched[2];
    int64_t  over_min[2], over_max[2], over_sum[2];
    int      tx_unmatched, air_unmatched;
    int      wakes;                /* CPU transitions into active */
    int64_t  core_lpm_at_event;    /* the event being handled's core_lpm_ns */
    /* The CPU's low-power time between its first and last wake, from the
     * stream and from the core's own accounting (core_lpm_ns). */
    bool     wake0_seen;
    int      wakes_cmp;
    int64_t  lp_at_wake0, core_at_wake0, lp_at_wake, core_at_wake;
    /* The same over the lives before the current one (each life is a new
     * core, whose lpm_ns starts again from 0). */
    int      wakes_cmp_prev;
    int64_t  lp_prev, core_prev;
    bool     have_core;
    int64_t  core_lpm_ns, core_active_ns;
} node_t;

typedef struct { char board[32]; char key[48]; double v; bool used; } override_t;

struct energest_engine {
    energest_sink_t sink;
    /* options */
    int64_t  bin_ns, frame_ns;
    int      nb;                   /* bins per frame                         */
    char     csv_path[512], frames_path[512], trace_path[512];
    double   battery_mah;
    override_t ov[MAX_OVERRIDES];
    int      nov;
    FILE    *frames_f;
    FILE    *csv_f;                /* created at configure, written at the report */
    FILE    *trace_f;              /* every state change, CSV (option trace) */
    /* state */
    node_t   n[MAX_NODES];
    int64_t  last_ns;
    int64_t  end_ns;               /* the run's end, once the report closed it */
    int64_t  win_t0;               /* first bin of the ring                  */
    long     frames_emitted;
    long     late_stamps, late_bins;
    int      lane_overflow;
    long     dropped_events;       /* of slots past MAX_NODES (none today)  */
};

/* ---------------------------------------------------------------- utils */

typedef struct { char *s; size_t len, cap; bool oom; } sb_t;
static void sb_printf(sb_t *b, const char *fmt, ...) {
    if (b->oom) return;
    for (;;) {
        size_t room = b->cap - b->len;
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(b->s ? b->s + b->len : NULL, b->s ? room : 0, fmt, ap);
        va_end(ap);
        if (n < 0) { b->oom = true; return; }
        if (b->s && (size_t)n < room) { b->len += (size_t)n; return; }
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap - b->len <= (size_t)n) cap *= 2;
        char *p = realloc(b->s, cap);
        if (!p) { b->oom = true; return; }
        b->s = p; b->cap = cap;
    }
}

static void emit_line(energest_engine_t *e, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (e->sink.log) e->sink.log(buf);
    else             fputs(buf, stdout);
}

/* ------------------------------------------------------------- options */

static void set_defaults(energest_engine_t *e) {
    e->bin_ns = 10000000LL;          /* 10 ms */
    e->frame_ns = 1000000000LL;      /* 1 s   */
    e->nb = (int)(e->frame_ns / e->bin_ns);
    e->csv_path[0] = e->frames_path[0] = e->trace_path[0] = '\0';
    e->battery_mah = 0.0;
    e->nov = 0;
}

energest_engine_t *energest_engine_create(energest_sink_t sink) {
    energest_engine_t *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->sink = sink;
    set_defaults(e);
    return e;
}

static void free_lanes(energest_engine_t *e) {
    for (int i = 0; i < MAX_NODES; i++)
        for (int l = 0; l < e->n[i].nlanes; l++) {
            free(e->n[i].lanes[l].bins);
            e->n[i].lanes[l].bins = NULL;
        }
}

void energest_engine_reset(energest_engine_t *e) {
    if (!e) return;
    free_lanes(e);
    memset(e->n, 0, sizeof e->n);
    e->last_ns = 0;
    e->end_ns = 0;
    e->win_t0 = 0;
    e->frames_emitted = 0;
    e->late_stamps = e->late_bins = 0;
    e->lane_overflow = 0;
    e->dropped_events = 0;
    for (int i = 0; i < e->nov; i++) e->ov[i].used = false;
}

void energest_engine_free(energest_engine_t *e) {
    if (!e) return;
    free_lanes(e);
    if (e->frames_f) fclose(e->frames_f);
    if (e->csv_f) fclose(e->csv_f);
    if (e->trace_f) fclose(e->trace_f);
    free(e);
}

/* The component names a `currents` key may use, and their state sets. */
static const char *const *states_of_class(const char *cls, int *n) {
    if (strcmp(cls, "cpu") == 0)   { *n = SIM_CPU_MODE_COUNT; return CPU_STATES; }
    if (strcmp(cls, "radio") == 0) { *n = 5; return RADIO_STATES; }
    *n = 2; return ONOFF_STATES;
}
static const char *class_of_name(const char *comp) {
    static const char *const timers[] = { "timer", "Timer_A", "Timer_B", "timer0",
        "timer1", "timer2", "timer3", "timer4", "rtc0", "rtc1", "rtc2", NULL };
    static const char *const gates[] = { "gate", "gpt0", "gpt1", "gpt2", "gpt3",
        "ssi0", "ssi1", "uart0", "uart1", "i2c", "pka", "aes", "rfc", NULL };
    if (strcmp(comp, "cpu") == 0 || strcmp(comp, "radio") == 0) return comp;
    if (strcmp(comp, "led") == 0 ||
        (strncmp(comp, "led", 3) == 0 && comp[3] >= '0' && comp[3] <= '7' && !comp[4]))
        return "led";
    for (int i = 0; timers[i]; i++) if (strcmp(comp, timers[i]) == 0) return "timer";
    for (int i = 0; gates[i]; i++)  if (strcmp(comp, gates[i]) == 0)  return "gate";
    return NULL;
}

static int fail(char *err, size_t errsz, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (err && errsz) vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
    return -1;
}

int energest_engine_configure(energest_engine_t *e, const char *json,
                              char *err, size_t errsz) {
    set_defaults(e);
    if (!json || !json[0]) return 0;
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return fail(err, errsz, "energest: not a mapping"); }
    int rc = 0;
    const cJSON *c;
    double bin_ms = 10.0, frame_ms = 1000.0;
    bool frame_given = false;
    cJSON_ArrayForEach(c, root) {
        const char *k = c->string;
        if (strcmp(k, "bin_ms") == 0 || strcmp(k, "frame_ms") == 0 ||
            strcmp(k, "battery_mah") == 0) {
            if (!cJSON_IsNumber(c) || !isfinite(c->valuedouble) || c->valuedouble < 0) {
                rc = fail(err, errsz, "energest.%s must be a number >= 0", k); break;
            }
            if (k[0] == 'b' && k[1] == 'i') bin_ms = c->valuedouble;
            else if (k[0] == 'f') { frame_ms = c->valuedouble; frame_given = true; }
            else e->battery_mah = c->valuedouble;
        } else if (strcmp(k, "csv") == 0 || strcmp(k, "frames") == 0 ||
                   strcmp(k, "trace") == 0) {
            if (!cJSON_IsString(c) || !c->valuestring[0]) {
                rc = fail(err, errsz, "energest.%s must be a file path", k); break;
            }
            char *dst = k[0] == 'c' ? e->csv_path : k[0] == 'f' ? e->frames_path : e->trace_path;
            size_t cap = sizeof e->csv_path;
            if (snprintf(dst, cap, "%s", c->valuestring) >= (int)cap) {
                rc = fail(err, errsz, "energest.%s: path too long", k); break;
            }
        } else if (strcmp(k, "currents") == 0) {
            const cJSON *b;
            cJSON_ArrayForEach(b, c) {
                if (!cJSON_IsObject(b)) {
                    rc = fail(err, errsz, "energest.currents.%s must be a mapping of "
                              "component.state: mA", b->string); break;
                }
                if (e->sink.board_known && !e->sink.board_known(b->string)) {
                    rc = fail(err, errsz, "energest.currents: unknown board '%s'", b->string); break;
                }
                const cJSON *kv;
                cJSON_ArrayForEach(kv, b) {
                    const char *key = kv->string;
                    if (!cJSON_IsNumber(kv) || !isfinite(kv->valuedouble) || kv->valuedouble < 0) {
                        rc = fail(err, errsz, "energest.currents.%s.%s must be a number >= 0",
                                  b->string, key); break;
                    }
                    if (strcmp(key, "supply_v") != 0) {
                        char comp[48];
                        const char *dot = strrchr(key, '.');
                        if (!dot || dot == key || (size_t)(dot - key) >= sizeof comp) {
                            rc = fail(err, errsz, "energest.currents.%s: '%s' is not "
                                      "component.state", b->string, key); break;
                        }
                        memcpy(comp, key, (size_t)(dot - key));
                        comp[dot - key] = '\0';
                        const char *cls = class_of_name(comp);
                        if (!cls) {
                            rc = fail(err, errsz, "energest.currents.%s: unknown component '%s'",
                                      b->string, comp); break;
                        }
                        int ns; const char *const *sn = states_of_class(cls, &ns);
                        int ok = 0;
                        for (int s = 0; s < ns; s++) if (strcmp(sn[s], dot + 1) == 0) ok = 1;
                        if (!ok) {
                            rc = fail(err, errsz, "energest.currents.%s: '%s' has no state '%s'",
                                      b->string, comp, dot + 1); break;
                        }
                    }
                    if (e->nov >= MAX_OVERRIDES) { rc = fail(err, errsz, "energest.currents: too many"); break; }
                    override_t *o = &e->ov[e->nov++];
                    snprintf(o->board, sizeof o->board, "%s", b->string);
                    snprintf(o->key, sizeof o->key, "%s", key);
                    o->v = kv->valuedouble;
                }
                if (rc) break;
            }
            if (rc) break;
        } else {
            rc = fail(err, errsz, "energest: unknown key '%s' (bin_ms, frame_ms, csv, "
                      "frames, trace, battery_mah, currents)", k);
            break;
        }
    }
    cJSON_Delete(root);
    if (rc) return rc;
    if (bin_ms < 1.0 || bin_ms > 1000.0 || bin_ms != floor(bin_ms))
        return fail(err, errsz, "energest.bin_ms must be a whole number of ms, 1..1000");
    /* No frame_ms: the whole bins that fit in the default second. */
    if (!frame_given) frame_ms = floor(1000.0 / bin_ms) * bin_ms;
    if (frame_ms < bin_ms || fmod(frame_ms, bin_ms) != 0.0 || frame_ms / bin_ms > 1000.0)
        return fail(err, errsz, "energest.frame_ms must be a multiple of bin_ms, "
                    "at most 1000 bins");
    e->bin_ns = (int64_t)bin_ms * 1000000LL;
    e->frame_ns = (int64_t)frame_ms * 1000000LL;
    e->nb = (int)(e->frame_ns / e->bin_ns);
    if (e->frames_path[0]) {
        e->frames_f = fopen(e->frames_path, "w");
        if (!e->frames_f)
            return fail(err, errsz, "energest.frames: cannot create %s", e->frames_path);
    }
    if (e->csv_path[0]) {
        /* Created now, so that a path that cannot be written stops the run
         * before it starts, as frames and trace do; written at the end. */
        e->csv_f = fopen(e->csv_path, "w");
        if (!e->csv_f)
            return fail(err, errsz, "energest.csv: cannot create %s", e->csv_path);
    }
    if (e->trace_path[0]) {
        e->trace_f = fopen(e->trace_path, "w");
        if (!e->trace_f)
            return fail(err, errsz, "energest.trace: cannot create %s", e->trace_path);
        fputs("t_ns,node,component,state\n", e->trace_f);
    }
    return 0;
}

/* -------------------------------------------------------------- lanes */

static bool is_low_power(int mode) { return mode != SIM_CPU_MODE_ACTIVE; }

static bool override_for(energest_engine_t *e, const char *board, const char *key,
                         double *v) {
    for (int i = 0; i < e->nov; i++)
        if (strcmp(e->ov[i].board, board) == 0 && strcmp(e->ov[i].key, key) == 0) {
            e->ov[i].used = true;
            *v = e->ov[i].v;
            return true;
        }
    return false;
}

/* The current of lane `l` in state s: the config's "lane.state", then its
 * "class.state", then the model's, else 0. */
static void resolve_currents(energest_engine_t *e, node_t *n, lane_t *l) {
    for (int s = 0; s < l->nstates; s++) {
        char key[64];
        double v;
        l->ma[s] = 0.0;
        l->src[s] = "none (0)";
        snprintf(key, sizeof key, "%s.%s", l->name, l->snames[s]);
        if (override_for(e, n->board, key, &v)) { l->ma[s] = v; l->src[s] = "config"; continue; }
        snprintf(key, sizeof key, "%s.%s", l->cls, l->snames[s]);
        if (override_for(e, n->board, key, &v)) { l->ma[s] = v; l->src[s] = "config"; continue; }
        const char *src = NULL;
        if (energest_model_current(n->model, l->name, l->cls, l->snames[s], &v, &src)) {
            l->ma[s] = v; l->src[s] = src;
        }
    }
}

static int64_t node_alive_now(const node_t *n, int64_t t) {
    return n->alive_ns + (n->alive ? (t > n->life_t0 ? t - n->life_t0 : 0) : 0);
}

static void trace_life(energest_engine_t *e, const node_t *n, int64_t t, const char *what) {
    if (e->trace_f)
        fprintf(e->trace_f, "%" PRId64 ",%d,life,%s\n", t, n->id, what);
}

/* born_now: the node's first event is its MOTE_ADDED at t (added during
 * the run), so its life begins at t, whatever start describe gives. */
static node_t *node_get(energest_engine_t *e, sim_runtime_t *sim, int idx, int64_t t,
                        bool born_now) {
    if (idx < 0 || idx >= MAX_NODES) return NULL;
    node_t *n = &e->n[idx];
    if (n->seen) return n;
    energest_node_info_t info = { .id = idx + 1, .board = NULL, .start_ns = 0 };
    if (e->sink.describe) e->sink.describe(sim, idx, &info);
    n->seen = true;
    n->id = info.id;
    snprintf(n->board, sizeof n->board, "%s", info.board ? info.board : "");
    n->model = energest_model_for_board(info.board);
    n->radio_approx = energest_model_radio_approx(info.board);
    n->supply_v = n->model ? n->model->supply_v : 3.0;
    double v;
    if (override_for(e, n->board, "supply_v", &v)) n->supply_v = v;
    n->alive = true;
    /* The node's lanes open where it started executing (its startup delay),
     * or here if that is unknown / later than its first event. */
    int64_t t0 = info.start_ns;
    if (born_now || t0 < 0 || t0 > t) t0 = t;
    n->life_t0 = t0;
    n->tx_level = -1;
    n->pend_class = -1;
    trace_life(e, n, t0, "added");
    return n;
}

static void trace(energest_engine_t *e, const node_t *n, const lane_t *l, int64_t t) {
    if (e->trace_f)
        fprintf(e->trace_f, "%" PRId64 ",%d,%s,%s\n", t, n->id, l->name, l->snames[l->cur]);
}

static lane_t *lane_get(energest_engine_t *e, node_t *n, int kind, int index,
                        const char *name) {
    for (int i = 0; i < n->nlanes; i++)
        if (n->lanes[i].kind == kind && n->lanes[i].index == index) return &n->lanes[i];
    if (n->nlanes >= MAX_LANES) { e->lane_overflow++; return NULL; }
    lane_t *l = &n->lanes[n->nlanes];
    memset(l, 0, sizeof *l);
    l->kind = kind;
    l->index = index;
    switch (kind) {
    case LK_CPU:   l->cls = "cpu";   l->nstates = SIM_CPU_MODE_COUNT; l->snames = CPU_STATES; break;
    case LK_RADIO: l->cls = "radio"; l->nstates = 5; l->snames = RADIO_STATES; break;
    case SIM_COMP_LED:   l->cls = "led";   l->nstates = 2; l->snames = ONOFF_STATES; break;
    case SIM_COMP_TIMER: l->cls = "timer"; l->nstates = 2; l->snames = ONOFF_STATES; break;
    default:             l->cls = "gate";  l->nstates = 2; l->snames = ONOFF_STATES; break;
    }
    snprintf(l->name, sizeof l->name, "%s", name ? name : l->cls);
    l->bins = calloc((size_t)l->nstates * 2u * (size_t)e->nb, sizeof(int32_t));
    if (!l->bins) { e->lane_overflow++; return NULL; }
    n->nlanes++;
    /* Every lane starts in its first state (CPU executing, radio and
     * peripherals off: their reset states) when its node's life began. */
    l->cur = 0;
    l->since = n->life_t0;
    l->entered = n->life_t0;
    l->bin_since = n->life_t0 > e->win_t0 ? n->life_t0 : e->win_t0;
    l->alive_before = n->alive_ns;
    resolve_currents(e, n, l);
    trace(e, n, l, l->since);
    return l;
}

/* The current lane l draws in state s, now. */
static double lane_ma(const node_t *n, const lane_t *l, int s) {
    double ma = l->ma[s];
    if (l->kind == LK_RADIO && s == R_TX && n->tx_level >= 0 &&
        strcmp(l->src[s], "config") != 0)
        energest_model_tx_ma(n->model, n->tx_level, &ma);
    return ma;
}

/* Spread [a, b) of state s into the ring (bins from win_t0). */
static void bin_range(energest_engine_t *e, lane_t *l, int s, int64_t a, int64_t b) {
    int64_t lo = e->win_t0, hi = e->win_t0 + 2 * e->frame_ns;
    if (a < lo) a = lo;
    if (b > hi) b = hi;
    if (b <= a) return;
    int32_t *row = l->bins + (size_t)s * 2u * (size_t)e->nb;
    int64_t i = (a - lo) / e->bin_ns;
    while (a < b) {
        int64_t edge = lo + (i + 1) * e->bin_ns;
        int64_t end = b < edge ? b : edge;
        row[i] += (int32_t)(end - a);
        a = end;
        i++;
    }
}

/* Close lane l's open dwell at t (it stays in its state). */
static void lane_close(energest_engine_t *e, node_t *n, lane_t *l, int64_t t) {
    if (t < l->since) { e->late_stamps++; t = l->since; }
    int64_t dt = t - l->since;
    if (dt > 0) {
        double ma = lane_ma(n, l, l->cur);
        int64_t edge = e->win_t0 + e->frame_ns;
        int64_t pre = t <= edge ? dt : l->since >= edge ? 0 : edge - l->since;
        l->total_ns[l->cur] += dt;
        l->q[l->cur] += (double)dt * ma;
        l->edge_ns[l->cur] += pre;
        l->edge_q[l->cur] += (double)pre * ma;
        l->next_ns[l->cur] += dt - pre;
        l->next_q[l->cur] += (double)(dt - pre) * ma;
    }
    l->since = t;
    if (t < l->bin_since) { e->late_bins++; return; }
    bin_range(e, l, l->cur, l->bin_since, t);
    l->bin_since = t;
}

static void lane_set(energest_engine_t *e, node_t *n, lane_t *l, int state, int64_t t) {
    if (state < 0 || state >= l->nstates) return;
    if (l->kind == LK_RADIO && l->cur == R_TX && state != R_TX) {
        /* A TX dwell ends: pair it with the frame the bus timed in it. */
        int64_t dwell = t - l->since;
        if (n->pend_class < 0) {
            n->tx_unmatched++;
        } else {
            int c = n->pend_class;
            int64_t over = dwell - n->pend_air_ns;
            if (!n->matched[c] || over < n->over_min[c]) n->over_min[c] = over;
            if (!n->matched[c] || over > n->over_max[c]) n->over_max[c] = over;
            n->over_sum[c] += over;
            n->matched[c]++;
            n->pend_class = -1;
        }
    }
    bool wake = l->kind == LK_CPU && state == SIM_CPU_MODE_ACTIVE && l->cur != state;
    if (wake) n->wakes++;
    lane_close(e, n, l, t);
    if (wake) {
        int64_t lp = 0;
        for (int s = 0; s < l->nstates; s++) if (is_low_power(s)) lp += l->total_ns[s];
        if (!n->wake0_seen) {
            n->wake0_seen = true;
            n->lp_at_wake0 = lp;
            n->core_at_wake0 = n->core_lpm_at_event;
        } else {
            n->wakes_cmp++;
        }
        n->lp_at_wake = lp;
        n->core_at_wake = n->core_lpm_at_event;
    }
    bool changed = l->cur != state;
    l->cur = state;
    if (changed) { l->entered = l->since; trace(e, n, l, l->since); }
}

/* ------------------------------------------------------------- frames */

static double lane_uj(const node_t *n, const lane_t *l) {
    double q = 0;
    for (int s = 0; s < l->nstates; s++) q += l->q[s];
    return q * n->supply_v * 1e-6;          /* mA x ns x V = 1e-6 uJ */
}
/* Charge including the open dwell up to t (read-only). */
static double lane_q_at(const node_t *n, const lane_t *l, int64_t t) {
    double q = 0;
    for (int s = 0; s < l->nstates; s++) q += l->q[s];
    if (n->alive && t > l->since) q += (double)(t - l->since) * lane_ma(n, l, l->cur);
    return q;
}
static int64_t lane_ns_at(const node_t *n, const lane_t *l, int s, int64_t t) {
    int64_t v = l->total_ns[s];
    if (n->alive && s == l->cur && t > l->since) v += t - l->since;
    return v;
}
/* The totals at the end t of the frame being emitted (edge_*, above). */
static int64_t lane_edge_ns(const node_t *n, const lane_t *l, int s, int64_t t) {
    int64_t v = l->edge_ns[s];
    if (n->alive && s == l->cur && t > l->since) v += t - l->since;
    return v;
}
static double lane_edge_q(const node_t *n, const lane_t *l, int64_t t) {
    double q = 0;
    for (int s = 0; s < l->nstates; s++) q += l->edge_q[s];
    if (n->alive && t > l->since) q += (double)(t - l->since) * lane_ma(n, l, l->cur);
    return q;
}

static void publish_panel(energest_engine_t *e, sim_runtime_t *sim, int64_t t) {
    if (!e->sink.publish) return;
    sb_t b = {0};
    sb_printf(&b, "{\"title\":\"Energy (estimate)\",\"rows\":[");
    int rows = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        node_t *n = &e->n[i];
        if (!n->seen) continue;
        double q = 0;
        int64_t radio_on = 0, radio_all = 0;
        for (int l = 0; l < n->nlanes; l++) {
            q += lane_q_at(n, &n->lanes[l], t);
            if (n->lanes[l].kind == LK_RADIO)
                for (int s = 0; s < 5; s++) {
                    int64_t v = lane_ns_at(n, &n->lanes[l], s, t);
                    radio_all += v;
                    if (s) radio_on += v;
                }
        }
        int64_t life = node_alive_now(n, t);
        double avg = life > 0 ? q / (double)life : 0.0;
        sb_printf(&b, "%s[\"node %d\",\"%.1f mJ %.2f mA radio on %.1f%%\"]",
                  rows ? "," : "", n->id, q * n->supply_v * 1e-9, avg,
                  radio_all > 0 ? 100.0 * (double)radio_on / (double)radio_all : 0.0);
        rows++;
    }
    sb_printf(&b, "]}");
    if (!b.oom) e->sink.publish(sim, "energest", b.s);
    free(b.s);
}

/* Emit the frame of the ring's first `nbins` bins ([win_t0, +nbins*bin)),
 * then slide the ring by a frame. */
static void emit_frame(energest_engine_t *e, sim_runtime_t *sim, int nbins) {
    int64_t t0 = e->win_t0, t1 = t0 + (int64_t)nbins * e->bin_ns;
    bool want = e->frames_f || e->sink.push_frame;
    sb_t b = {0};
    if (want) {
        sb_printf(&b, "{\"v\":1,\"t0\":%" PRId64 ",\"bin\":%" PRId64 ",\"n\":%d,\"nodes\":[",
                  t0, e->bin_ns, nbins);
        int nn = 0;
        for (int i = 0; i < MAX_NODES; i++) {
            node_t *n = &e->n[i];
            if (!n->seen) continue;
            /* The bins with any of the node's time: a node removed and
             * added again since t0 has an earlier life in this frame that
             * life_t0 / gone_ns (its latest) do not show. */
            int fb = -1, lb = -1;
            for (int li = 0; li < n->nlanes; li++) {
                const lane_t *l = &n->lanes[li];
                for (int k = 0; k < nbins; k++) {
                    int64_t sum = 0;
                    for (int s = 0; s < l->nstates; s++)
                        sum += l->bins[(size_t)s * 2u * (size_t)e->nb + (size_t)k];
                    if (!sum) continue;
                    if (fb < 0 || k < fb) fb = k;
                    if (k > lb) lb = k;
                }
            }
            bool present = (n->life_t0 < t1 && (n->alive || n->gone_ns > t0)) || fb >= 0;
            if (!present) continue;
            sb_printf(&b, "%s{\"id\":%d", nn++ ? "," : "", n->id);
            /* The power model the uJ came from, so a view can name it
             * and flag figures nobody checked against the datasheet. */
            if (n->model)
                sb_printf(&b, ",\"model\":\"%s\"%s", n->model->title,
                          n->model->verified ? "" : ",\"unverified\":true");
            sb_printf(&b, ",\"vcc\":%.2f", n->supply_v);
            int b0 = n->life_t0 > t0 ? (int)((n->life_t0 - t0) / e->bin_ns) : 0;
            int b1 = n->alive ? (n->life_t0 < t1 ? nbins : 0)   /* 0: a later life */
                   : n->gone_ns < t1 ? (int)((n->gone_ns - t0 + e->bin_ns - 1) / e->bin_ns)
                   : nbins;
            if (fb >= 0 && fb < b0) b0 = fb;
            if (lb >= 0 && lb + 1 > b1) b1 = lb + 1;
            if (b0 > 0) sb_printf(&b, ",\"b0\":%d", b0);
            if (b1 < nbins) sb_printf(&b, ",\"b1\":%d", b1);
            sb_printf(&b, ",\"lanes\":[");
            for (int li = 0; li < n->nlanes; li++) {
                lane_t *l = &n->lanes[li];
                int64_t sum[MAX_STATES] = {0};
                int base = 0;
                for (int s = 0; s < l->nstates; s++) {
                    const int32_t *row = l->bins + (size_t)s * 2u * (size_t)e->nb;
                    for (int k = 0; k < nbins; k++) sum[s] += row[k];
                    if (sum[s] > sum[base]) base = s;
                }
                sb_printf(&b, "%s{\"c\":\"%s\",\"base\":\"%s\"%s", li ? "," : "",
                          l->name, l->snames[base],
                          l->kind == LK_RADIO && n->radio_approx ? ",\"approx\":true" : "");
                int nf = 0;
                for (int s = 0; s < l->nstates; s++) {
                    if (s == base || sum[s] == 0) continue;
                    const int32_t *row = l->bins + (size_t)s * 2u * (size_t)e->nb;
                    sb_printf(&b, "%s\"%s\":[", nf++ ? "," : ",\"f\":{", l->snames[s]);
                    /* Permille of each bin; a run of k >= 2 zero bins is
                     * written -k (docs/energest.md "UI frames"). */
                    int first = 1;
                    for (int k = 0; k < nbins;) {
                        int v = (int)(((int64_t)row[k] * 1000 + e->bin_ns / 2) / e->bin_ns);
                        int run = 1;
                        if (v == 0)
                            while (k + run < nbins &&
                                   ((int64_t)row[k + run] * 1000 + e->bin_ns / 2) / e->bin_ns == 0)
                                run++;
                        sb_printf(&b, "%s%d", first ? "" : ",", run >= 2 ? -run : v);
                        first = 0;
                        k += run;
                    }
                    sb_printf(&b, "]");
                }
                if (nf) sb_printf(&b, "}");
                sb_printf(&b, "}");
            }
            /* Cumulative totals at the frame's end (or the run's, in the
             * last frame): us per state, uJ per lane. */
            int64_t te = e->end_ns && e->end_ns < t1 ? e->end_ns : t1;
            sb_printf(&b, "],\"us\":{");
            for (int li = 0; li < n->nlanes; li++) {
                lane_t *l = &n->lanes[li];
                sb_printf(&b, "%s\"%s\":{", li ? "," : "", l->name);
                int k = 0;
                for (int s = 0; s < l->nstates; s++) {
                    int64_t v = lane_edge_ns(n, l, s, te) / 1000;
                    if (v) sb_printf(&b, "%s\"%s\":%" PRId64, k++ ? "," : "", l->snames[s], v);
                }
                sb_printf(&b, "}");
            }
            double qall = 0;
            sb_printf(&b, "},\"uj\":{");
            for (int li = 0; li < n->nlanes; li++) {
                double q = lane_edge_q(n, &n->lanes[li], te);
                qall += q;
                sb_printf(&b, "%s\"%s\":%.1f", li ? "," : "", n->lanes[li].name,
                          q * n->supply_v * 1e-6);
            }
            int64_t life = node_alive_now(n, te);
            sb_printf(&b, "},\"ma\":%.4f}", life > 0 ? qall / (double)life : 0.0);
        }
        sb_printf(&b, "]}");
        if (!b.oom) {
            if (e->frames_f) { fputs(b.s, e->frames_f); fputc('\n', e->frames_f); }
            if (e->sink.push_frame) e->sink.push_frame(sim, "energest", b.s);
        }
        free(b.s);
    }
    e->frames_emitted++;
    /* Slide the ring by one frame. */
    for (int i = 0; i < MAX_NODES; i++)
        for (int li = 0; li < e->n[i].nlanes; li++) {
            lane_t *l = &e->n[i].lanes[li];
            for (int s = 0; s < l->nstates; s++) {
                int32_t *row = l->bins + (size_t)s * 2u * (size_t)e->nb;
                memmove(row, row + e->nb, (size_t)e->nb * sizeof(int32_t));
                memset(row + e->nb, 0, (size_t)e->nb * sizeof(int32_t));
                l->edge_ns[s] += l->next_ns[s]; l->next_ns[s] = 0;
                l->edge_q[s] += l->next_q[s];   l->next_q[s] = 0;
            }
        }
    e->win_t0 += e->frame_ns;
    publish_panel(e, sim, e->end_ns && e->end_ns < t1 ? e->end_ns : t1);
}

/* Bring every live lane's bins up to t1 (the frame's end). */
static void bin_open_dwells(energest_engine_t *e, int64_t t1) {
    for (int i = 0; i < MAX_NODES; i++) {
        node_t *n = &e->n[i];
        if (!n->seen || !n->alive) continue;
        for (int li = 0; li < n->nlanes; li++) {
            lane_t *l = &n->lanes[li];
            if (l->bin_since < t1) {
                bin_range(e, l, l->cur, l->bin_since, t1);
                l->bin_since = t1;
            }
        }
    }
}

/* Emit every frame that ended a full frame before t. */
static void advance(energest_engine_t *e, sim_runtime_t *sim, int64_t t) {
    while (t >= e->win_t0 + 2 * e->frame_ns) {
        bin_open_dwells(e, e->win_t0 + e->frame_ns);
        emit_frame(e, sim, e->nb);
    }
}

/* -------------------------------------------------------------- events */

static void finish(energest_engine_t *e, sim_runtime_t *sim);

static void node_close_all(energest_engine_t *e, node_t *n, int64_t t) {
    for (int li = 0; li < n->nlanes; li++) lane_close(e, n, &n->lanes[li], t);
}

void energest_engine_on_event(energest_engine_t *e, sim_runtime_t *sim,
                              const sim_observer_event_t *ev) {
    int64_t t = ev->time_ns;
    if (ev->kind == SIM_OBS_SIM_STOP) {
        /* The run's end, before a --ui-record closes: the last frames go
         * out now, while there is still a recording to take them. */
        if (t > e->last_ns) e->last_ns = t;
        finish(e, sim);
        return;
    }
    if (e->end_ns && ev->kind != SIM_OBS_CPU_STATE) return;   /* after the end */
    if (ev->kind == SIM_OBS_MOTE_LOG_LINE) {
        /* The trace carries the console too, stamped as the state changes
         * are (a firmware's own energest report, checked against them). */
        if (e->trace_f && ev->u.log_line.line) {
            fprintf(e->trace_f, "%" PRId64 ",%d,log,\"", t, ev->u.log_line.node_id);
            for (int i = 0; i < ev->u.log_line.len; i++) {
                char ch = ev->u.log_line.line[i];
                if (ch == '\n' || ch == '\r') continue;
                if (ch == '"') fputc('"', e->trace_f);
                fputc(ch, e->trace_f);
            }
            fputs("\"\n", e->trace_f);
        }
        return;
    }
    int idx = ev->mote_index;
    switch (ev->kind) {
    case SIM_OBS_CPU_TRANSITION: case SIM_OBS_RADIO_STATE: case SIM_OBS_COMPONENT_STATE:
    case SIM_OBS_RADIO_AIRTIME:  case SIM_OBS_CPU_STATE:
    case SIM_OBS_MOTE_ADDED:     case SIM_OBS_MOTE_REMOVED:
        break;
    default:
        return;
    }
    if (idx < 0 || idx >= MAX_NODES) { e->dropped_events++; return; }
    if (t > e->last_ns) e->last_ns = t;
    /* Frames go out by the kernel's clock: a stamp may stand ahead of it
     * (a node resynced at its start gate, seconds ahead with a startup
     * delay), and emitting by it would close frames the other nodes have
     * not run yet. */
    advance(e, sim, e->sink.now ? e->sink.now(sim) : e->last_ns);
    bool born_now = ev->kind == SIM_OBS_MOTE_ADDED && !e->n[idx].seen;
    node_t *n = node_get(e, sim, idx, t, born_now);
    if (!n) return;

    switch (ev->kind) {
    case SIM_OBS_CPU_TRANSITION: {
        if (!n->alive) break;
        lane_t *l = lane_get(e, n, LK_CPU, 0, "cpu");
        n->core_lpm_at_event = ev->u.cpu_mode.core_lpm_ns;
        if (l) lane_set(e, n, l, ev->u.cpu_mode.mode, t);
        break;
    }
    case SIM_OBS_RADIO_STATE: {
        if (!n->alive) break;
        lane_t *l = lane_get(e, n, LK_RADIO, 0, "radio");
        if (l) lane_set(e, n, l, ev->u.radio_state.state, t);
        break;
    }
    case SIM_OBS_COMPONENT_STATE: {
        if (!n->alive) break;
        if (ev->u.component.kind == SIM_COMP_TX_POWER) {
            /* Close the radio's dwell so far at the old level. */
            lane_t *r = lane_get(e, n, LK_RADIO, 0, "radio");
            if (r) lane_close(e, n, r, t);
            n->tx_level = ev->u.component.state;
            break;
        }
        lane_t *l = lane_get(e, n, ev->u.component.kind, ev->u.component.index,
                             ev->u.component.name);
        if (l) lane_set(e, n, l, ev->u.component.state ? 1 : 0, t);
        break;
    }
    case SIM_OBS_RADIO_AIRTIME: {
        if (ev->u.airtime.is_ack) n->acks++; else n->frames++;
        if (e->trace_f)   /* the frame on air: its first and last byte */
            fprintf(e->trace_f, "%" PRId64 ",%d,air,%s\n%" PRId64 ",%d,air,idle\n",
                    ev->u.airtime.start_ns, n->id,
                    ev->u.airtime.is_ack || ev->u.airtime.psdu_len == 5 ? "ack" : "frame",
                    t, n->id);
        /* A frame ends inside its sender's TX dwell (the bus reports it at
         * its last byte, before the chip leaves TX). */
        lane_t *r = NULL;
        for (int li = 0; li < n->nlanes; li++)
            if (n->lanes[li].kind == LK_RADIO) r = &n->lanes[li];
        if (r && r->cur == R_TX && n->alive) {
            if (n->pend_class >= 0) n->air_unmatched++;
            n->pend_class = ev->u.airtime.psdu_len == 5 ? 1 : 0;
            n->pend_air_ns = t - ev->u.airtime.start_ns;
        } else {
            n->air_unmatched++;
        }
        n->air_ns += t - ev->u.airtime.start_ns;
        n->air_bytes += ev->u.airtime.air_bytes;
        n->air_bytes_ns += (int64_t)ev->u.airtime.air_bytes * ev->u.airtime.byte_ns;
        break;
    }
    case SIM_OBS_CPU_STATE:
        n->have_core = true;
        n->core_active_ns = ev->u.cpu.active_ns;
        n->core_lpm_ns = ev->u.cpu.lpm_ns;
        break;
    case SIM_OBS_MOTE_REMOVED:
        if (!n->alive) break;
        node_close_all(e, n, t);
        n->alive_ns += t > n->life_t0 ? t - n->life_t0 : 0;
        n->alive = false;
        n->gone_ns = t;
        trace_life(e, n, t, "removed");
        break;
    case SIM_OBS_MOTE_ADDED:
        /* A new life (reboot, add): the totals go on; every lane starts
         * over in its reset state.  A node seen first here was born now
         * (node_get): nothing to close. */
        if (born_now) break;
        if (n->alive) {
            node_close_all(e, n, t);
            n->alive_ns += t > n->life_t0 ? t - n->life_t0 : 0;
            trace_life(e, n, t, "removed");
        }
        n->alive = true;
        n->life_t0 = t;
        n->tx_level = -1;
        n->pend_class = -1;
        /* The low-power cross-check goes on per life: a new core. */
        if (n->wake0_seen) {
            n->lp_prev += n->lp_at_wake - n->lp_at_wake0;
            n->core_prev += n->core_at_wake - n->core_at_wake0;
            n->wakes_cmp_prev += n->wakes_cmp;
        }
        n->wake0_seen = false;
        n->wakes_cmp = 0;
        trace_life(e, n, t, "added");
        for (int li = 0; li < n->nlanes; li++) {
            lane_t *l = &n->lanes[li];
            l->cur = 0;
            l->since = t;
            l->entered = t;
            if (l->bin_since < t) l->bin_since = t;
            trace(e, n, l, t);
        }
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------------- report */

static void fmt_lifetime(char *out, size_t n, double hours) {
    if (!isfinite(hours) || hours <= 0) snprintf(out, n, "-");
    else if (hours < 48)        snprintf(out, n, "%.1f hours", hours);
    else if (hours < 24 * 365)  snprintf(out, n, "%.1f days", hours / 24.0);
    else                        snprintf(out, n, "%.2f years", hours / (24.0 * 365.25));
}


/* The run ends: close every lane, then emit the frames up to the end (the
 * last one partial).  Once; later state events are ignored. */
static void finish(energest_engine_t *e, sim_runtime_t *sim) {
    if (e->end_ns) return;
    int64_t end = e->last_ns > 0 ? e->last_ns : 1;
    /* The full frames first, so that the dwells closed at the end fall in
     * the ring's two frames (the frame totals split them at its middle). */
    advance(e, sim, end);
    for (int i = 0; i < MAX_NODES; i++) {
        node_t *n = &e->n[i];
        if (n->seen && n->alive) node_close_all(e, n, end);
    }
    e->end_ns = end;
    while (e->win_t0 < end) {
        int64_t left = end - e->win_t0;
        int nb = left >= e->frame_ns ? e->nb : (int)((left + e->bin_ns - 1) / e->bin_ns);
        int64_t t1 = e->win_t0 + (int64_t)nb * e->bin_ns;
        bin_open_dwells(e, t1 < end ? t1 : end);
        emit_frame(e, sim, nb);
    }
}

/* Close f; false if any write to it failed. */
static bool close_ok(FILE *f) {
    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    return ok;
}

int energest_engine_report(energest_engine_t *e, sim_runtime_t *sim) {
    int rc = 0;
    finish(e, sim);
    int64_t end = e->end_ns;
    if (e->trace_f) {
        if (!close_ok(e->trace_f)) {
            emit_line(e, "energest: writing %s failed\n", e->trace_path);
            rc = -1;
        }
        e->trace_f = NULL;
    }
    if (e->frames_f) {
        if (!close_ok(e->frames_f)) {
            emit_line(e, "energest: writing %s failed\n", e->frames_path);
            rc = -1;
        }
        e->frames_f = NULL;
    }

    emit_line(e, "energest: time in state per node and component (exact, from each "
                 "node's own clock); energy is a datasheet estimate\n");
    FILE *csv = e->csv_f;
    e->csv_f = NULL;
    if (csv)
        fputs("node,board,component,state,time_ns,fraction,current_ma,energy_uj,source\n", csv);
    double net_uj = 0.0;
    for (int i = 0; i < MAX_NODES; i++) {
        node_t *n = &e->n[i];
        if (!n->seen) continue;
        int64_t life = n->alive_ns + (n->alive && end > n->life_t0 ? end - n->life_t0 : 0);
        const char *mtitle = n->model ? n->model->title : "no datasheet model (currents 0 unless configured)";
        emit_line(e, "energest: node %d  %s, %.2f V%s%s  lifetime %.6f s\n", n->id, mtitle,
                  n->supply_v, n->model && !n->model->verified ? ", UNVERIFIED figures" : "",
                  n->radio_approx ? ", radio APPROXIMATE" : "", life / 1e9);
        double q_node = 0.0;
        for (int li = 0; li < n->nlanes; li++) {
            lane_t *l = &n->lanes[li];
            int64_t covered = life - l->alive_before;
            int64_t sum = 0;
            char buf[400];
            int off = snprintf(buf, sizeof buf, "energest:   %-8s", l->name);
            for (int s = 0; s < l->nstates; s++) {
                sum += l->total_ns[s];
                if (!l->total_ns[s]) continue;
                if (off > 0 && off < (int)sizeof buf)
                    off += snprintf(buf + off, sizeof buf - (size_t)off, " %s %.6f s (%.2f%%)",
                                    l->snames[s], l->total_ns[s] / 1e9,
                                    covered > 0 ? 100.0 * (double)l->total_ns[s] / (double)covered : 0.0);
                if (csv)
                    fprintf(csv, "%d,%s,%s,%s,%" PRId64 ",%.9f,%.6g,%.3f,\"%s\"\n", n->id,
                            n->board, l->name, l->snames[s], l->total_ns[s],
                            covered > 0 ? (double)l->total_ns[s] / (double)covered : 0.0,
                            l->total_ns[s] ? l->q[s] / (double)l->total_ns[s] : l->ma[s],
                            l->q[s] * n->supply_v * 1e-6, l->src[s]);
            }
            double uj = lane_uj(n, l);
            for (int s = 0; s < l->nstates; s++) q_node += l->q[s];
            emit_line(e, "%s  = %.3f mJ\n", buf, uj / 1000.0);
            if (sum != covered) {
                emit_line(e, "energest:   CHECK FAILED: %s states sum to %" PRId64
                          " ns, its lifetime is %" PRId64 " ns\n", l->name, sum, covered);
                rc = -1;
            }
        }
        double uj_node = q_node * n->supply_v * 1e-6;
        double avg_ma = life > 0 ? q_node / (double)life : 0.0;
        net_uj += uj_node;
        char lt[48] = "-";
        if (e->battery_mah > 0 && avg_ma > 0) fmt_lifetime(lt, sizeof lt, e->battery_mah / avg_ma);
        emit_line(e, "energest:   total    %.3f mJ, average %.4f mA%s%s\n", uj_node / 1000.0,
                  avg_ma, e->battery_mah > 0 ? ", battery life " : "",
                  e->battery_mah > 0 ? lt : "");
        if (csv) {
            fprintf(csv, "%d,%s,total,all,%" PRId64 ",1,%.6g,%.3f,\"average current\"\n",
                    n->id, n->board, life, avg_ma, uj_node);
            if (e->battery_mah > 0 && avg_ma > 0)
                fprintf(csv, "%d,%s,battery,lifetime_h,0,0,%.6g,0,\"%g mAh / average\"\n",
                        n->id, n->board, e->battery_mah / avg_ma, e->battery_mah);
        }
        /* Cross-checks against independent accounting. */
        for (int li = 0; li < n->nlanes; li++) {
            lane_t *l = &n->lanes[li];
            int wakes = n->wakes_cmp_prev + n->wakes_cmp;
            if (l->kind == LK_CPU && wakes > 0) {
                /* Between the first and the last wake (of each life): the
                 * ends are where the core has not yet accounted a sleep (it
                 * fast-forwards a sleep in the slices that run it; a node
                 * asleep at the end has not run the rest). */
                int64_t lp = n->lp_prev + (n->wakes_cmp ? n->lp_at_wake - n->lp_at_wake0 : 0);
                int64_t core = n->core_prev +
                               (n->wakes_cmp ? n->core_at_wake - n->core_at_wake0 : 0);
                emit_line(e, "energest:   check    cpu low-power between the first and the last "
                          "wake %" PRId64 " ns, the core's lpm_ns %" PRId64 " ns: %+" PRId64
                          " ns over %d wakes (%+.3f us each)\n", lp, core, lp - core,
                          wakes, (double)(lp - core) / wakes / 1000.0);
            }
            if (l->kind == LK_RADIO && (n->frames || n->acks)) {
                emit_line(e, "energest:   check    radio tx %" PRId64 " ns, frame air time %"
                          PRId64 " ns (%d frames + %d bus ACKs, %" PRId64 " bytes on air): "
                          "%s bytes x byte time\n",
                          l->total_ns[R_TX], n->air_ns, n->frames, n->acks, n->air_bytes,
                          n->air_ns == n->air_bytes_ns ? "equal to" : "NOT equal to");
                if (n->air_ns != n->air_bytes_ns) rc = -1;
                static const char *const cls[2] = { "data", "5-byte (ACK)" };
                bool short_tx = false;
                for (int c = 0; c < 2; c++) {
                    if (!n->matched[c]) continue;
                    emit_line(e, "energest:   check    tx dwell - air time, %d %s frames: "
                              "min %.3f max %.3f mean %.3f us\n", n->matched[c], cls[c],
                              n->over_min[c] / 1000.0, n->over_max[c] / 1000.0,
                              (double)n->over_sum[c] / n->matched[c] / 1000.0);
                    /* A radio is in TX at least while its frame is on air:
                     * a dwell shorter than that (beyond a stamp's cycle or
                     * two) is a chip model that does not time TX by the
                     * frame — known (radio_approx) or a defect. */
                    if (n->over_min[c] < -TX_SHORT_NS && !n->radio_approx) {
                        emit_line(e, "energest:   CHECK FAILED: a tx dwell %.3f us shorter "
                                  "than its frame's air time\n", -n->over_min[c] / 1000.0);
                        rc = -1;
                    }
                    if (n->over_min[c] < -TX_SHORT_NS) short_tx = true;
                }
                if (short_tx && n->radio_approx)
                    emit_line(e, "energest:   note     radio approximate: %s\n", n->radio_approx);
                if (n->tx_unmatched || n->air_unmatched)
                    emit_line(e, "energest:   check    %d tx dwells without a frame, %d frames "
                              "outside a tx dwell\n", n->tx_unmatched, n->air_unmatched);
            }
        }
    }
    if (csv && !close_ok(csv)) { emit_line(e, "energest: writing %s failed\n", e->csv_path); rc = -1; }
    for (int i = 0; i < e->nov; i++)
        if (!e->ov[i].used)
            emit_line(e, "energest: note: currents.%s.%s matched no component of any node\n",
                      e->ov[i].board, e->ov[i].key);
    if (e->late_stamps || e->late_bins || e->lane_overflow)
        emit_line(e, "energest: note: %ld stamps before their lane's last, %ld folded into "
                  "a later bin, %d events of lanes past the limit of %d per node (or out "
                  "of memory) not counted\n", e->late_stamps, e->late_bins,
                  e->lane_overflow, MAX_LANES);
    if (e->dropped_events) {
        emit_line(e, "energest: CHECK FAILED: %ld power-state events of nodes past slot %d "
                  "dropped (not counted)\n", e->dropped_events, MAX_NODES);
        rc = -1;
    }
    emit_line(e, "energest: network total ~%.3f mJ (datasheet estimate), %ld UI frames\n",
              net_uj / 1000.0, e->frames_emitted);
    return rc;
}
