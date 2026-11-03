/*
 * thread_overlay_engine — see thread_overlay_engine.h.
 */
#include "thread_overlay_engine.h"
#include "thread_layout.h"
#include "packet_analyzer.h"
#include "sim_layer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NODES    128                 /* SIM_EQ_MAX_NODES */
#define REFRESH_NS   500000000LL         /* rebuild at most every 500 ms */
#define MAX_ROUTERS  32
#define INSTANCE_SYM "_ZN2ot12gInstanceRawE"   /* ot::gInstanceRaw */

/* OpenThread's otDeviceRole, Neighbor::State and DeviceMode bits. */
enum { ROLE_DISABLED = 0, ROLE_DETACHED, ROLE_CHILD, ROLE_ROUTER, ROLE_LEADER };
enum { NEIGHBOR_VALID = 7 };
enum { MODE_FULL_NETWORK_DATA = 1 << 0, MODE_FTD = 1 << 1, MODE_RX_ON_WHEN_IDLE = 1 << 3 };

typedef struct {
    int      id;                  /* Cooja node id, -1 = no node in the slot  */
    enum { SRC_NONE, SRC_FW, SRC_PKT } src;
    int      role;
    uint8_t  mode;                /* firmware only                           */
    uint16_t rloc16;
    uint32_t partition;           /* firmware only                           */
    uint8_t  weighting;           /* the partition's leader weight (firmware) */
    uint8_t  leader_router_id;
    uint16_t parent_rloc16;       /* 0xFFFF = none                           */
    int      nrouters;
    uint16_t routers[MAX_ROUTERS];/* routers it has a valid link to          */
    bool     layout_unknown;
    bool     not_thread;          /* its image has no OpenThread instance    */
} node_view_t;

struct thread_overlay {
    thread_overlay_host_t host;
    uint16_t pkt_rloc16[MAX_NODES];   /* short source address of its frames */
    bool     pkt_seen[MAX_NODES];
    int64_t  pkt_last_ns[MAX_NODES];  /* when it last sent from it          */
    node_view_t view[MAX_NODES];
    uint8_t  linked[MAX_NODES][MAX_NODES / 8];   /* router links drawn, by slot pair */
    int64_t  last_ns;
    bool     restarted;   /* refresh at the first event that finds the new nodes */
};

thread_overlay_t *thread_overlay_create(thread_overlay_host_t host) {
    thread_overlay_t *o = calloc(1, sizeof(*o));
    if (o) o->host = host;
    return o;
}

void thread_overlay_free(thread_overlay_t *o) { free(o); }

/* --- packets --------------------------------------------------------------- */

static void learn_frame(thread_overlay_t *o, int slot, const uint8_t *bytes, int len, int64_t t) {
    if (slot < 0 || slot >= MAX_NODES || !bytes || len < 5 || len > 127) return;
    uint8_t buf[128];
    buf[0] = (uint8_t)len;
    memcpy(buf + 1, bytes, (size_t)len);
    pkt_info_t pi;
    pkt_analyze(buf, len + 1, &pi);
    /* A data or MAC command frame (a child's Data Request) from a short
     * address: after attaching, that is the node's RLOC16 (0xFFFE and up are
     * reserved, not RLOC16s).  MLE itself goes between link-local addresses
     * made from the extended address, so this fills in as traffic flows. */
    if ((pi.frame_type == PKT_802154_DATA || pi.frame_type == PKT_802154_CMD) &&
        pi.src_mode == 2 && pi.src_short < 0xFFFE) {
        o->pkt_rloc16[slot] = pi.src_short;
        o->pkt_seen[slot] = true;
        o->pkt_last_ns[slot] = t;
    }
}

static void pkt_view(const thread_overlay_t *o, int slot, node_view_t *nv, int64_t now_ns) {
    /* A node that has sent nothing from its RLOC16 for THREAD_PKT_AGE_NS
     * (5 min) has most likely detached, been stopped or left: its old RLOC16
     * no longer says much.  Only data and MAC command frames carry it (MLE
     * goes from the extended address), so a quiet attached node is silent
     * on it too: in examples/thread-overlay.yaml the longest such silence
     * of a node is 152 s.  A node quieter than 5 min drops out of this view
     * until it sends again. */
    if (!o->pkt_seen[slot] || now_ns - o->pkt_last_ns[slot] > THREAD_PKT_AGE_NS) return;
    nv->src = SRC_PKT;
    nv->rloc16 = o->pkt_rloc16[slot];
    nv->role = (nv->rloc16 & 0x01FF) == 0 ? ROLE_ROUTER : ROLE_CHILD;
    if (nv->role == ROLE_CHILD) nv->parent_rloc16 = nv->rloc16 & 0xFC00;
}

/* --- firmware -------------------------------------------------------------- */

#define ROWS thread_layout_rows, sizeof(thread_layout_rows) / sizeof(thread_layout_rows[0])

/* A field of `type` at base + offset of the instance, its bits picked out. */
static bool fw_get(thread_overlay_t *o, sim_runtime_t *sim, int slot, const sim_fwvar_t *v,
                   const char *layout, const char *type, const char *field,
                   uint32_t base, uint32_t *out) {
    const struct_layout_row_t *r = struct_layout_row(ROWS, layout, type, field);
    uint8_t b[4] = {0};
    if (!r || r->size == 0 || r->size > 4 || !o->host.read(sim, slot, v, base + r->offset, b, r->size))
        return false;
    *out = struct_layout_value(r, b);
    return true;
}

/* What the firmware source made of a node. */
enum { FW_READ,       /* read from memory                                   */
       FW_PACKETS,    /* no memory to read, or a layout we do not know: use  */
                      /* the packets                                        */
       FW_NOT_THREAD  /* an image with symbols but no OpenThread instance:  */
                      /* not a Thread node, so nothing is drawn             */
};

static int fw_view(thread_overlay_t *o, sim_runtime_t *sim, int slot, node_view_t *nv) {
    if (!o->host.resolve || !o->host.read) return FW_PACKETS;
    sim_fwvar_t inst;
    if (!o->host.resolve(sim, slot, INSTANCE_SYM, &inst)) {
        /* An image whose symbols can be read (it has a main) but no
         * ot::gInstanceRaw is not OpenThread (Contiki-NG, RIOT, Zephyr in a
         * mixed network): its short-address frames are not RLOC16s, so it
         * is not drawn.  No symbols at all (a native, JS or Renode node, a
         * stripped image): the packets are all there is. */
        sim_fwvar_t m;
        return o->host.resolve(sim, slot, "main", &m) ? FW_NOT_THREAD : FW_PACKETS;
    }
    const char *L = struct_layout_pick(ROWS, inst.machine, "Instance", inst.size);
    if (!L) { nv->layout_unknown = true; return FW_PACKETS; }
    uint32_t role, mode, rloc, part, weight, lrid, prloc, pstate, nrt;
    if (!fw_get(o, sim, slot, &inst, L, "Instance", "mMle.mRole", 0, &role) ||
        !fw_get(o, sim, slot, &inst, L, "Instance", "mMle.mDeviceMode.mMode", 0, &mode) ||
        !fw_get(o, sim, slot, &inst, L, "Instance", "mMle.mRloc16", 0, &rloc) ||
        !fw_get(o, sim, slot, &inst, L, "Instance", "mMle.mLeaderData.mPartitionId", 0, &part) ||
        !fw_get(o, sim, slot, &inst, L, "Instance", "mMle.mLeaderData.mWeighting", 0, &weight) ||
        !fw_get(o, sim, slot, &inst, L, "Instance", "mMle.mLeaderData.mLeaderRouterId", 0, &lrid) ||
        !fw_get(o, sim, slot, &inst, L, "Instance", "mMle.mParent.mRloc16", 0, &prloc) ||
        !fw_get(o, sim, slot, &inst, L, "Instance", "mMle.mParent.mState", 0, &pstate) ||
        !fw_get(o, sim, slot, &inst, L, "Instance", "mMle.mRouterTable.mRouters.mLength", 0, &nrt))
        return FW_PACKETS;
    nv->src = SRC_FW;
    nv->role = (int)role;
    nv->mode = (uint8_t)mode;
    nv->rloc16 = (uint16_t)rloc;
    nv->partition = part;
    nv->weighting = (uint8_t)weight;
    nv->leader_router_id = (uint8_t)lrid;
    nv->parent_rloc16 = (role == ROLE_CHILD && pstate == NEIGHBOR_VALID) ? (uint16_t)prloc : 0xFFFF;
    if (role == ROLE_ROUTER || role == ROLE_LEADER) {
        /* Its router table: the routers it has a valid link to, with a
         * link quality in either direction (in or out): OTNS draws a router
         * link as soon as either end has one, and so do we. */
        const struct_layout_row_t *el = struct_layout_row(ROWS, L, "Instance", "mMle.mRouterTable.mRouters.mElements");
        const struct_layout_row_t *rs = struct_layout_row(ROWS, L, "Router", "");
        if (el && rs && rs->size) {
            if (nrt > MAX_ROUTERS) nrt = MAX_ROUTERS;
            for (uint32_t i = 0; i < nrt; i++) {
                uint32_t base = el->offset + i * rs->size, rr, st, lqi, lqo;
                if (!fw_get(o, sim, slot, &inst, L, "Router", "mRloc16", base, &rr) ||
                    !fw_get(o, sim, slot, &inst, L, "Router", "mState", base, &st) ||
                    !fw_get(o, sim, slot, &inst, L, "Router", "mLinkInfo.mLinkQualityIn", base, &lqi) ||
                    !fw_get(o, sim, slot, &inst, L, "Router", "mLinkInfo.mLinkQualityOut", base, &lqo))
                    break;
                if (st == NEIGHBOR_VALID && (lqi || lqo) && rr != rloc && nv->nrouters < MAX_ROUTERS)
                    nv->routers[nv->nrouters++] = (uint16_t)rr;
            }
        }
    }
    return FW_READ;
}

/* --- the panel -------------------------------------------------------------- */

/* OTNS-Web's colours (web/site/js/vis/Node.js, PixiVisualizer.js), so a
 * network reads the same in both: the ring is the role, the fill is the
 * partition — OTNS uses the partition ID itself as the RGB colour, and so
 * do we, so one partition has one colour in either simulator. */
#define RING_LEADER   "#c62828"
#define RING_ROUTER   "#1565c0"
#define RING_CHILD    "#4caf50"
#define RING_DETACHED "#546e7a"
#define RING_DISABLED "#9e9e9e"
#define LINK_PARENT   "#8bc34a"
#define LINK_ROUTER   "#1976d2"
#define FILL_UNKNOWN  "#9e9e9e"           /* packets: no partition known */

static void partition_colour(uint32_t p, char out[8]) {
    snprintf(out, 8, "#%06x", (unsigned)(p & 0xFFFFFF));
}

/* The right-click commands, as OpenThread CLI lines (cli.cpp, mle*.cpp at
 * the bundled images' OpenThread):
 *   thread start / thread stop   enable / disable Thread (disabled: drawn
 *                                grey, with "Start Thread" to bring it back)
 *   state detached               BecomeDetached: leave the partition, then
 *                                attach again to any partition by itself
 *   state router / state child   an FTD child asks for a router ID; a router
 *                                attaches again as a child
 *   leaderweight N, state leader BecomeLeader starts a NEW partition (it
 *                                does not take over this one), and refuses
 *                                unless the node's leader weight is above
 *                                its partition's.  N is the partition's
 *                                weight + 1, so the command works however
 *                                often it is used, and the old partition,
 *                                whose weight is lower, merges into the new
 *                                one as its nodes hear it.  A detached
 *                                node is in no partition (its leader data
 *                                is stale), so it is not weighed: it gets
 *                                plain "state leader", its own weight.
 * A leader has no "become router": OpenThread's BecomeRouter does nothing
 * for a leader (and says Done). */
#define ACT_START  "Start Thread (thread start)", "thread start"
#define ACT_STOP   "Stop Thread (thread stop)", "thread stop"
#define ACT_DETACH "Detach and reattach (state detached)", "state detached"
#define ACT_ROUTER "Become router (state router)", "state router"
#define ACT_CHILD  "Become child (state child)", "state child"
#define ACT_LEAD   "Become leader of a new partition (state leader)", "state leader"
#define MAX_ACTS   6                      /* per node */

typedef struct {
    const char *v[2 * MAX_ACTS + 1];      /* label, line, ..., NULL */
    int  n;
    char lead_label[80], lead_line[40];   /* "Become leader", with its weight */
} acts_t;

static void act(acts_t *a, const char *label, const char *line) {
    if (a->n < 2 * MAX_ACTS) { a->v[a->n++] = label; a->v[a->n++] = line; }
    a->v[a->n] = NULL;
}

/* Each use leaves the new partition at weight + 1, so the weight climbs
 * by one per "Become leader"; at 255 there is nothing above it and
 * OpenThread would refuse, so the item is shown disabled with the reason
 * (an empty command: the page greys it out and sends nothing). */
static void act_leader(acts_t *a, const node_view_t *nv) {
    if (nv->weighting == 255) {
        act(a, "Become leader: no (the partition's leader weight is already 255, the most)", "");
        return;
    }
    unsigned w = nv->weighting + 1u;
    snprintf(a->lead_label, sizeof a->lead_label,
             "Become leader of a new partition (leaderweight %u, state leader)", w);
    snprintf(a->lead_line, sizeof a->lead_line, "leaderweight %u\nstate leader", w);
    act(a, a->lead_label, a->lead_line);
}

/* The commands that apply to the node's role and mode.  From packets alone
 * the mode and the weights are unknown, so only leaving is offered. */
static void role_actions(acts_t *a, const node_view_t *nv) {
    memset(a, 0, sizeof *a);
    bool pkt = nv->src == SRC_PKT, ftd = !pkt && (nv->mode & MODE_FTD);
    switch (nv->role) {
    case ROLE_DISABLED: act(a, ACT_START); return;
    case ROLE_DETACHED: if (ftd) act(a, ACT_LEAD); act(a, ACT_STOP); return;
    case ROLE_LEADER:   break;
    case ROLE_ROUTER:   if (!pkt) { act_leader(a, nv); act(a, ACT_CHILD); } break;
    default:            if (ftd) { act(a, ACT_ROUTER); act_leader(a, nv); } break;
    }
    act(a, ACT_DETACH);
    act(a, ACT_STOP);
}

/* The slot of the router or leader with RLOC16 `r` that `from` links to (a
 * child's parent, a router-table entry), or -1.  An RLOC16 is unique only
 * within a partition: after a split two leaders can both be 0x0400, and a
 * router table keeps a departed router's entry until it ages out.  So when
 * both partitions are known (firmware), only the looking node's own
 * partition counts; a node seen in packets alone has no partition and
 * matches on the RLOC16 alone, after any same-partition match. */
static int slot_of_rloc16(const thread_overlay_t *o, int n, const node_view_t *from, uint16_t r) {
    int loose = -1;
    for (int s = 0; s < n; s++) {
        const node_view_t *v = &o->view[s];
        if (v == from || v->id < 0 || v->src == SRC_NONE || v->rloc16 != r ||
            (v->role != ROLE_ROUTER && v->role != ROLE_LEADER))
            continue;
        if (from->src == SRC_FW && v->src == SRC_FW) {
            if (v->partition == from->partition) return s;
        } else if (loose < 0) {
            loose = s;
        }
    }
    return loose;
}

/* Add `id` to a comma list; one that no longer fits ends it with " ..."
 * (once), so a cut list says it was cut. */
static void append_id(char *buf, size_t sz, int id) {
    size_t l = strlen(buf);
    if (l >= 3 && !strcmp(buf + l - 3, "...")) return;
    char item[16];
    snprintf(item, sizeof item, "%s%d", l ? ", " : "", id);
    if (l + strlen(item) + 5 <= sz) memcpy(buf + l, item, strlen(item) + 1);
    else if (l + 4 < sz) memcpy(buf + l, l ? " ..." : "...", l ? 5 : 4);
}

void thread_overlay_refresh(thread_overlay_t *o, sim_runtime_t *sim, int64_t now_ns) {
    o->last_ns = now_ns;
    if (!o->host.publish) return;
    int n = o->host.node_count ? o->host.node_count(sim) : 0;
    if (n <= 0) return;        /* no slots (the nodes are being torn down): keep the last panel */
    o->restarted = false;
    if (n > MAX_NODES) n = MAX_NODES;
    for (int s = 0; s < n; s++) {
        node_view_t *nv = &o->view[s];
        memset(nv, 0, sizeof(*nv));
        nv->parent_rloc16 = 0xFFFF;
        nv->id = o->host.node_id ? o->host.node_id(sim, s) : s + 1;
        if (nv->id < 0) continue;
        int fw = fw_view(o, sim, s, nv);
        if (fw == FW_PACKETS) pkt_view(o, s, nv, now_ns);
        else if (fw == FW_NOT_THREAD) nv->not_thread = true;
    }

    sim_layer_t *l = sim_layer_begin(o->host.title ? o->host.title : "Thread");
    int leaders = 0, routers = 0, children = 0, sleepy = 0, detached = 0, disabled = 0, from_pkt = 0;
    uint32_t partitions[MAX_NODES];
    int npart = 0;
    char unknown[128] = "", other[128] = "";
    memset(o->linked, 0, sizeof o->linked);
    for (int s = 0; s < n; s++) {
        const node_view_t *nv = &o->view[s];
        if (nv->id < 0) continue;
        if (nv->layout_unknown) append_id(unknown, sizeof unknown, nv->id);
        if (nv->not_thread) append_id(other, sizeof other, nv->id);
        if (nv->src == SRC_NONE) continue;
        bool pkt = nv->src == SRC_PKT;
        if (pkt) from_pkt++;
        acts_t acts;
        role_actions(&acts, nv);
        if (nv->role == ROLE_DISABLED || nv->role == ROLE_DETACHED) {
            bool off = nv->role == ROLE_DISABLED;
            if (off) disabled++; else detached++;
            sim_layer_node(l, nv->id, &(sim_layer_style_t){
                .glyph = "circle", .ring = off ? RING_DISABLED : RING_DETACHED,
                .label = off ? "disabled" : "detached", .actions = acts.v });
            continue;
        }
        char label[24];
        snprintf(label, sizeof label, "%04X%s", nv->rloc16, pkt ? "*" : "");
        char fillbuf[8];
        partition_colour(nv->partition, fillbuf);
        const char *fill = pkt ? FILL_UNKNOWN : fillbuf;
        if (!pkt) {
            bool known = false;
            for (int k = 0; k < npart; k++) known |= partitions[k] == nv->partition;
            if (!known && npart < MAX_NODES) partitions[npart++] = nv->partition;
        }
        bool sed = !pkt && !(nv->mode & MODE_RX_ON_WHEN_IDLE);
        sim_layer_style_t st = { .fill = fill, .label = label, .actions = acts.v };
        if (nv->role == ROLE_LEADER) {
            leaders++;
            st.glyph = "hex"; st.ring = RING_LEADER; st.badge = "L";
        } else if (nv->role == ROLE_ROUTER) {
            routers++;
            st.glyph = "hex"; st.ring = RING_ROUTER; st.badge = "R";
        } else {
            children++;
            st.glyph = "circle"; st.ring = RING_CHILD;
            if (sed) { sleepy++; st.badge = "S"; st.ring_dash = true; }
            else st.badge = "C";
        }
        sim_layer_node(l, nv->id, &st);
        if (nv->parent_rloc16 != 0xFFFF) {
            int p = slot_of_rloc16(o, n, nv, nv->parent_rloc16);
            if (p >= 0) sim_layer_link(l, nv->id, o->view[p].id, "parent", LINK_PARENT);
        }
        /* A router link once per pair, from the lower id to the higher,
         * whichever of the two lists the other (their tables can disagree
         * while a link comes up or ages out). */
        for (int k = 0; k < nv->nrouters; k++) {
            int p = slot_of_rloc16(o, n, nv, nv->routers[k]);
            if (p < 0) continue;
            int a = s < p ? s : p, b = s < p ? p : s;
            if (o->linked[a][b / 8] & (1u << (b % 8))) continue;
            o->linked[a][b / 8] |= (uint8_t)(1u << (b % 8));
            int lo = nv->id < o->view[p].id ? nv->id : o->view[p].id;
            int hi = nv->id < o->view[p].id ? o->view[p].id : nv->id;
            sim_layer_link(l, lo, hi, "router", LINK_ROUTER);
        }
    }
    sim_layer_legend(l, "ring", "role: red leader, blue router, green child (dashed: sleepy), grey detached or disabled");
    sim_layer_legend(l, "fill", "partition (its ID as the colour, as OTNS does)");
    sim_layer_legend(l, "lines", "green: child to parent; blue: router link");
    if (from_pkt) sim_layer_legend(l, "*", "from MAC headers (RLOC16): no leader or partition");
    char note[192];
    snprintf(note, sizeof note,
             "%d leader%s, %d router%s, %d child%s (%d sleepy), %d detached, %d disabled; %d partition%s",
             leaders, leaders == 1 ? "" : "s", routers, routers == 1 ? "" : "s",
             children, children == 1 ? "" : "ren", sleepy, detached, disabled, npart, npart == 1 ? "" : "s");
    sim_layer_note(l, note);
    if (unknown[0]) {
        snprintf(note, sizeof note, "layout unknown (packets used): node %s", unknown);
        sim_layer_note(l, note);
    }
    if (other[0]) {
        snprintf(note, sizeof note, "not OpenThread (not drawn): node %s", other);
        sim_layer_note(l, note);
    }
    sim_layer_note(l, "right-click a node to change its role");
    char *json = sim_layer_end(l);
    if (json) o->host.publish(sim, o->host.panel_id ? o->host.panel_id : "thread", json);
    free(json);
}

void thread_overlay_on_event(thread_overlay_t *o, sim_runtime_t *sim,
                             const sim_observer_event_t *ev) {
    if (ev->kind == SIM_OBS_PACKET_FRAME && ev->u.frame.is_tx)
        learn_frame(o, ev->mote_index, ev->u.frame.bytes, ev->u.frame.bytes_len, ev->time_ns);
    /* A node rebooted, removed or added on its own: its RLOC16 was the old
     * node's. */
    if ((ev->kind == SIM_OBS_MOTE_ADDED || ev->kind == SIM_OBS_MOTE_REMOVED) &&
        ev->mote_index >= 0 && ev->mote_index < MAX_NODES)
        o->pkt_seen[ev->mote_index] = false;
    if (ev->time_ns - o->last_ns >= REFRESH_NS) {
        thread_overlay_refresh(o, sim, ev->time_ns);
    } else if (o->restarted) {
        /* After a restart, the first event that finds the new nodes draws
         * them at once; the throttle keeps its phase from time 0, as in a
         * fresh run. */
        int64_t keep = o->last_ns;
        thread_overlay_refresh(o, sim, ev->time_ns);
        o->last_ns = keep;
    }
}

void thread_overlay_restart(thread_overlay_t *o, sim_runtime_t *sim) {
    memset(o->pkt_seen, 0, sizeof(o->pkt_seen));
    memset(o->pkt_rloc16, 0, sizeof(o->pkt_rloc16));
    memset(o->pkt_last_ns, 0, sizeof(o->pkt_last_ns));
    memset(o->view, 0, sizeof(o->view));
    o->last_ns = 0;
    o->restarted = true;
    /* The old run's network must not stay drawn on the new nodes (paused
     * after the restart, until the first refresh): an empty layer now. */
    if (!o->host.publish) return;
    sim_layer_t *l = sim_layer_begin(o->host.title ? o->host.title : "Thread");
    sim_layer_note(l, "restarted: no Thread seen yet");
    char *json = sim_layer_end(l);
    if (json) o->host.publish(sim, o->host.panel_id ? o->host.panel_id : "thread", json);
    free(json);
}
