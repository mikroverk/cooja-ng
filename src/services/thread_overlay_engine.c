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
    uint8_t  leader_router_id;
    uint16_t parent_rloc16;       /* 0xFFFF = none                           */
    int      nrouters;
    uint16_t routers[MAX_ROUTERS];/* routers it has a valid link to          */
    bool     layout_unknown;
} node_view_t;

struct thread_overlay {
    thread_overlay_host_t host;
    uint16_t pkt_rloc16[MAX_NODES];   /* short source address of its frames */
    bool     pkt_seen[MAX_NODES];
    node_view_t view[MAX_NODES];
    int64_t  last_ns;
};

thread_overlay_t *thread_overlay_create(thread_overlay_host_t host) {
    thread_overlay_t *o = calloc(1, sizeof(*o));
    if (o) o->host = host;
    return o;
}

void thread_overlay_free(thread_overlay_t *o) { free(o); }

/* --- packets --------------------------------------------------------------- */

static void learn_frame(thread_overlay_t *o, int slot, const uint8_t *bytes, int len) {
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
    }
}

static void pkt_view(const thread_overlay_t *o, int slot, node_view_t *nv) {
    if (!o->pkt_seen[slot]) return;
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

static bool fw_view(thread_overlay_t *o, sim_runtime_t *sim, int slot, node_view_t *nv) {
    if (!o->host.resolve || !o->host.read) return false;
    sim_fwvar_t inst;
    if (!o->host.resolve(sim, slot, INSTANCE_SYM, &inst)) return false;     /* not OpenThread */
    const char *L = struct_layout_pick(ROWS, inst.machine, "Instance", inst.size);
    if (!L) { nv->layout_unknown = true; return false; }
    uint32_t role, mode, rloc, part, lrid, prloc, pstate, nrt;
    if (!fw_get(o, sim, slot, &inst, L, "Instance", "mMle.mRole", 0, &role) ||
        !fw_get(o, sim, slot, &inst, L, "Instance", "mMle.mDeviceMode.mMode", 0, &mode) ||
        !fw_get(o, sim, slot, &inst, L, "Instance", "mMle.mRloc16", 0, &rloc) ||
        !fw_get(o, sim, slot, &inst, L, "Instance", "mMle.mLeaderData.mPartitionId", 0, &part) ||
        !fw_get(o, sim, slot, &inst, L, "Instance", "mMle.mLeaderData.mLeaderRouterId", 0, &lrid) ||
        !fw_get(o, sim, slot, &inst, L, "Instance", "mMle.mParent.mRloc16", 0, &prloc) ||
        !fw_get(o, sim, slot, &inst, L, "Instance", "mMle.mParent.mState", 0, &pstate) ||
        !fw_get(o, sim, slot, &inst, L, "Instance", "mMle.mRouterTable.mRouters.mLength", 0, &nrt))
        return false;
    nv->src = SRC_FW;
    nv->role = (int)role;
    nv->mode = (uint8_t)mode;
    nv->rloc16 = (uint16_t)rloc;
    nv->partition = part;
    nv->leader_router_id = (uint8_t)lrid;
    nv->parent_rloc16 = (role == ROLE_CHILD && pstate == NEIGHBOR_VALID) ? (uint16_t)prloc : 0xFFFF;
    if (role == ROLE_ROUTER || role == ROLE_LEADER) {
        /* Its router table: the routers it has a valid two-way link to. */
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
    return true;
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
#define LINK_PARENT   "#8bc34a"
#define LINK_ROUTER   "#1976d2"
#define FILL_UNKNOWN  "#9e9e9e"           /* packets: no partition known */

static void partition_colour(uint32_t p, char out[8]) {
    snprintf(out, 8, "#%06x", (unsigned)(p & 0xFFFFFF));
}

/* The node whose RLOC16 is `r` (a router: a child's parent, a router link). */
static int node_of_rloc16(const thread_overlay_t *o, int n, uint16_t r) {
    for (int s = 0; s < n; s++) {
        const node_view_t *v = &o->view[s];
        if (v->id >= 0 && v->src != SRC_NONE && v->rloc16 == r &&
            (v->role == ROLE_ROUTER || v->role == ROLE_LEADER))
            return v->id;
    }
    return -1;
}

static void append_id(char *buf, size_t sz, int id) {
    size_t l = strlen(buf);
    if (l + 8 < sz) snprintf(buf + l, sz - l, "%s%d", l ? ", " : "", id);
}

void thread_overlay_refresh(thread_overlay_t *o, sim_runtime_t *sim, int64_t now_ns) {
    o->last_ns = now_ns;
    if (!o->host.publish) return;
    int n = o->host.node_count ? o->host.node_count(sim) : 0;
    if (n <= 0) return;        /* no slots (the nodes are being torn down): keep the last panel */
    if (n > MAX_NODES) n = MAX_NODES;
    for (int s = 0; s < n; s++) {
        node_view_t *nv = &o->view[s];
        memset(nv, 0, sizeof(*nv));
        nv->parent_rloc16 = 0xFFFF;
        nv->id = o->host.node_id ? o->host.node_id(sim, s) : s + 1;
        if (nv->id < 0) continue;
        if (!fw_view(o, sim, s, nv)) pkt_view(o, s, nv);
    }

    sim_layer_t *l = sim_layer_begin(o->host.title ? o->host.title : "Thread");
    int leaders = 0, routers = 0, children = 0, sleepy = 0, detached = 0, from_pkt = 0;
    uint32_t partitions[MAX_NODES];
    int npart = 0;
    char unknown[128] = "";
    for (int s = 0; s < n; s++) {
        const node_view_t *nv = &o->view[s];
        if (nv->id < 0) continue;
        if (nv->layout_unknown) append_id(unknown, sizeof unknown, nv->id);
        if (nv->src == SRC_NONE || nv->role == ROLE_DISABLED) continue;
        bool pkt = nv->src == SRC_PKT;
        if (pkt) from_pkt++;
        char label[24];
        if (nv->role == ROLE_DETACHED) {
            detached++;
            sim_layer_node(l, nv->id, &(sim_layer_style_t){
                .glyph = "circle", .ring = RING_DETACHED, .label = "detached",
                .actions = (const char *const[]){ "Rejoin (thread start)", "thread start",
                                                  "Disable (thread stop)", "thread stop", NULL } });
            continue;
        }
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
        bool ftd = pkt || (nv->mode & MODE_FTD);
        sim_layer_style_t st = { .fill = fill, .label = label };
        if (nv->role == ROLE_LEADER) {
            leaders++;
            st.glyph = "hex"; st.ring = RING_LEADER; st.badge = "L";
            st.actions = (const char *const[]){ "Become router (state router)", "state router",
                                                "Detach (thread stop)", "thread stop", NULL };
        } else if (nv->role == ROLE_ROUTER) {
            routers++;
            st.glyph = "hex"; st.ring = RING_ROUTER; st.badge = "R";
            st.actions = (const char *const[]){ "Become leader (leaderweight 128, state leader)", "leaderweight 128\nstate leader",
                                                "Become child (state child)", "state child",
                                                "Detach (thread stop)", "thread stop", NULL };
        } else {
            children++;
            st.glyph = "circle"; st.ring = RING_CHILD;
            if (sed) { sleepy++; st.badge = "S"; st.ring_dash = true; }
            else st.badge = "C";
            st.actions = ftd ? (const char *const[]){ "Become router (state router)", "state router",
                                                      "Become leader (leaderweight 128, state leader)", "leaderweight 128\nstate leader",
                                                      "Detach (thread stop)", "thread stop", NULL }
                             : (const char *const[]){ "Detach (thread stop)", "thread stop", NULL };
        }
        sim_layer_node(l, nv->id, &st);
        if (nv->parent_rloc16 != 0xFFFF) {
            int p = node_of_rloc16(o, n, nv->parent_rloc16);
            if (p >= 0) sim_layer_link(l, nv->id, p, "parent", LINK_PARENT);
        }
        for (int k = 0; k < nv->nrouters; k++) {
            int p = node_of_rloc16(o, n, nv->routers[k]);
            if (p > nv->id) sim_layer_link(l, nv->id, p, "router", LINK_ROUTER);   /* each pair once */
        }
    }
    sim_layer_legend(l, "ring", "role: red leader, blue router, green child (dashed: sleepy)");
    sim_layer_legend(l, "fill", "partition (its ID as the colour, as OTNS does)");
    sim_layer_legend(l, "lines", "green: child to parent; blue: router link");
    if (from_pkt) sim_layer_legend(l, "*", "from MAC headers (RLOC16): no leader or partition");
    char note[160];
    snprintf(note, sizeof note, "%d leader%s, %d router%s, %d child%s (%d sleepy), %d detached; %d partition%s",
             leaders, leaders == 1 ? "" : "s", routers, routers == 1 ? "" : "s",
             children, children == 1 ? "" : "ren", sleepy, detached, npart, npart == 1 ? "" : "s");
    sim_layer_note(l, note);
    if (unknown[0]) {
        snprintf(note, sizeof note, "layout unknown (packets used): node %s", unknown);
        sim_layer_note(l, note);
    }
    sim_layer_note(l, "right-click a node to change its role");
    char *json = sim_layer_end(l);
    if (json) o->host.publish(sim, o->host.panel_id ? o->host.panel_id : "thread", json);
    free(json);
}

void thread_overlay_on_event(thread_overlay_t *o, sim_runtime_t *sim,
                             const sim_observer_event_t *ev) {
    /* A restart (the UI's or the shell's) starts the clock again at 0: a
     * new run, so forget what the old one's packets said.  Events from
     * different nodes can arrive a little out of time order (each node runs
     * in its own slices), so only a clock back near zero, well behind the
     * last refresh, counts. */
    if (ev->time_ns < 2 * REFRESH_NS && o->last_ns > ev->time_ns + 10 * REFRESH_NS) {
        memset(o->pkt_seen, 0, sizeof(o->pkt_seen));
        o->last_ns = ev->time_ns;
    }
    if (ev->kind == SIM_OBS_PACKET_FRAME && ev->u.frame.is_tx)
        learn_frame(o, ev->mote_index, ev->u.frame.bytes, ev->u.frame.bytes_len);
    if (ev->time_ns - o->last_ns >= REFRESH_NS)
        thread_overlay_refresh(o, sim, ev->time_ns);
}
