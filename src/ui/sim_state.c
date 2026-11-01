/*
 * Simulation state serializer
 *
 * Two message types:
 *   "full"  — JSON, complete state for initial sync (connect/reload)
 *   "delta" — CBOR binary, only changed data for regular updates
 */
#include "sim_state.h"
#include "timeline.h"
#include "cbor.h"
#include "cJSON.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define MS_TO_NS 1000000LL

static const char *radio_state_str(sim_radio_state_t s) {
    switch (s) {
    case SIM_RADIO_ON:   return "on";
    case SIM_RADIO_TX:   return "tx";
    case SIM_RADIO_RX:   return "rx";
    case SIM_RADIO_INTF: return "intf";
    default:             return "off";
    }
}

char *sim_state_to_json(const sim_node_info_t *nodes, int node_count,
                        const sim_radio_info_t *radio,
                        const sim_stats_t *stats,
                        const sim_node_state_t *node_states,
                        const char *timeline_json) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject(root, "type", "full");

    /* Simulation time in ms */
    cJSON_AddNumberToObject(root, "sim_time_ms",
                            (double)(stats->sim_time_ns / MS_TO_NS));

    /* Nodes array — full info */
    cJSON *jarr = cJSON_CreateArray();
    for (int i = 0; i < node_count; i++) {
        cJSON *jnode = cJSON_CreateObject();
        cJSON_AddNumberToObject(jnode, "id", nodes[i].id);
        cJSON_AddStringToObject(jnode, "type", nodes[i].type);
        cJSON_AddNumberToObject(jnode, "x", nodes[i].x);
        cJSON_AddNumberToObject(jnode, "y", nodes[i].y);
        cJSON_AddNumberToObject(jnode, "cycles", (double)nodes[i].cycles);
        cJSON_AddNumberToObject(jnode, "freq", nodes[i].freq_hz);
        if (nodes[i].name && nodes[i].name[0])
            cJSON_AddStringToObject(jnode, "name", nodes[i].name);
        if (nodes[i].board && nodes[i].board[0])
            cJSON_AddStringToObject(jnode, "board", nodes[i].board);
        if (nodes[i].firmware && nodes[i].firmware[0])
            cJSON_AddStringToObject(jnode, "firmware", nodes[i].firmware);
        if (nodes[i].meta && nodes[i].meta[0]) {
            cJSON *jmeta = cJSON_Parse(nodes[i].meta);
            if (jmeta) cJSON_AddItemToObject(jnode, "meta", jmeta);
        }

        if (nodes[i].last_tx_ns > 0)
            cJSON_AddNumberToObject(jnode, "last_tx_ms",
                                    (double)(nodes[i].last_tx_ns / MS_TO_NS));

        /* Console lines */
        cJSON *jcons = cJSON_CreateArray();
        for (int c = 0; c < nodes[i].console_count; c++)
            cJSON_AddItemToArray(jcons, cJSON_CreateString(nodes[i].console[c]));
        cJSON_AddItemToObject(jnode, "console", jcons);

        /* Node state (radio + LEDs) */
        if (node_states) {
            cJSON_AddStringToObject(jnode, "radio",
                                    radio_state_str(node_states[i].radio_state));
            cJSON *jleds = cJSON_CreateArray();
            for (int l = 0; l < SIM_MAX_LEDS; l++)
                cJSON_AddItemToArray(jleds,
                    cJSON_CreateNumber(node_states[i].led[l]));
            cJSON_AddItemToObject(jnode, "leds", jleds);
        }

        cJSON_AddItemToArray(jarr, jnode);
    }
    cJSON_AddItemToObject(root, "nodes", jarr);

    /* Radio info */
    cJSON *jradio = cJSON_CreateObject();
    cJSON_AddStringToObject(jradio, "type", radio->type);
    cJSON_AddNumberToObject(jradio, "tx_range", radio->tx_range);

    /* Neighbor adjacency lists */
    cJSON *jneighbors = cJSON_CreateArray();
    for (int i = 0; i < radio->node_count; i++) {
        cJSON *jnl = cJSON_CreateArray();
        for (int j = 0; j < radio->neighbor_counts[i]; j++)
            cJSON_AddItemToArray(jnl, cJSON_CreateNumber(radio->neighbors[i][j]));
        cJSON_AddItemToArray(jneighbors, jnl);
    }
    cJSON_AddItemToObject(jradio, "neighbors", jneighbors);
    cJSON_AddItemToObject(root, "radio", jradio);

    /* Stats */
    cJSON *jstats = cJSON_CreateObject();
    cJSON_AddNumberToObject(jstats, "rf_bytes", stats->rf_bytes);
    cJSON_AddNumberToObject(jstats, "uart_bytes", stats->uart_bytes);
    cJSON_AddNumberToObject(jstats, "rx_frames_queued", stats->rx_frames_queued);
    cJSON_AddNumberToObject(jstats, "rx_frames_collided", stats->rx_frames_collided);
    cJSON_AddNumberToObject(jstats, "speed", stats->speed_ratio);
    cJSON_AddBoolToObject(jstats, "paused", stats->paused);
    cJSON_AddItemToObject(root, "stats", jstats);

    /* Timeline events (pre-serialized JSON array) */
    if (timeline_json) {
        cJSON *tl = cJSON_Parse(timeline_json);
        if (tl) {
            cJSON_AddItemToObject(root, "timeline", tl);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

char *sim_state_delta_json(const sim_stats_t *stats,
                           const sim_node_state_t *node_states,
                           int node_count,
                           const int *node_ids,
                           const int64_t *node_cycles,
                           const uint32_t *node_freqs,
                           const int64_t *node_last_tx_ns,
                           /* sparse console: arrays indexed by node index */
                           const char ***console_lines,
                           const int *console_counts,
                           const char *timeline_json) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject(root, "type", "delta");
    cJSON_AddNumberToObject(root, "sim_time_ms",
                            (double)(stats->sim_time_ns / MS_TO_NS));

    /* Stats — always included (small, always changing) */
    cJSON *jstats = cJSON_CreateObject();
    cJSON_AddNumberToObject(jstats, "rf_bytes", stats->rf_bytes);
    cJSON_AddNumberToObject(jstats, "uart_bytes", stats->uart_bytes);
    cJSON_AddNumberToObject(jstats, "rx_frames_queued", stats->rx_frames_queued);
    cJSON_AddNumberToObject(jstats, "rx_frames_collided", stats->rx_frames_collided);
    cJSON_AddNumberToObject(jstats, "speed", stats->speed_ratio);
    cJSON_AddBoolToObject(jstats, "paused", stats->paused);
    cJSON_AddItemToObject(root, "stats", jstats);

    /* Per-node state: cycles, freq, radio, LEDs + sparse console */
    cJSON *jnodes = cJSON_CreateArray();
    for (int i = 0; i < node_count; i++) {
        cJSON *jn = cJSON_CreateObject();
        cJSON_AddNumberToObject(jn, "id", node_ids[i]);
        cJSON_AddNumberToObject(jn, "cycles", (double)node_cycles[i]);
        cJSON_AddNumberToObject(jn, "freq", node_freqs[i]);
        if (node_states) {
            cJSON_AddStringToObject(jn, "radio",
                                    radio_state_str(node_states[i].radio_state));
            cJSON *jleds = cJSON_CreateArray();
            for (int l = 0; l < SIM_MAX_LEDS; l++)
                cJSON_AddItemToArray(jleds,
                    cJSON_CreateNumber(node_states[i].led[l]));
            cJSON_AddItemToObject(jn, "leds", jleds);
        }
        /* Last TX time for communication arrows */
        if (node_last_tx_ns && node_last_tx_ns[i] > 0)
            cJSON_AddNumberToObject(jn, "last_tx_ms",
                                    (double)(node_last_tx_ns[i] / MS_TO_NS));
        /* Console: only include if there are new lines */
        if (console_counts && console_counts[i] > 0 && console_lines) {
            cJSON *jcons = cJSON_CreateArray();
            for (int c = 0; c < console_counts[i]; c++)
                cJSON_AddItemToArray(jcons,
                    cJSON_CreateString(console_lines[i][c]));
            cJSON_AddItemToObject(jn, "console", jcons);
        }
        cJSON_AddItemToArray(jnodes, jn);
    }
    cJSON_AddItemToObject(root, "nodes", jnodes);

    /* Timeline events (pre-serialized JSON array, new only) */
    if (timeline_json) {
        cJSON *tl = cJSON_Parse(timeline_json);
        if (tl)
            cJSON_AddItemToObject(root, "timeline", tl);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

/* ================================================================
 * CBOR delta serializer — compact binary, change-only
 * ================================================================ */

/* Timeline event type to integer enum (matches JS decoder) */
static int tl_type_to_int(int type) {
    switch (type) {
    case TL_RADIO_OFF:  return 0;
    case TL_RADIO_ON:   return 1;
    case TL_RADIO_TX:   return 2;
    case TL_RADIO_RX:   return 3;
    case TL_RADIO_INTF: return 4;
    case TL_LED_ON:     return 5;
    case TL_LED_OFF:    return 6;
    case TL_RF_FRAME:   return 7;
    case TL_LOG:        return 8;
    default:            return 9;
    }
}

/* Bytes a CBOR head (major type and argument) takes as lib/cbor writes it:
 * the shortest form, as RFC 8949 asks. */
static size_t cbor_head_len(uint64_t v) {
    return v < 24 ? 1 : v <= UINT8_MAX ? 2 : v <= UINT16_MAX ? 3
         : v <= UINT32_MAX ? 5 : 9;
}

/* The CBOR size of one timeline entry without its frame bytes:
 * [t_us, node, type, value] plus the summary when it has one. */
static size_t tl_entry_cbor_len(const tl_event_t *ev) {
    size_t n = 1 + cbor_head_len((uint64_t)(ev->time_ns / 1000)) +
               cbor_head_len(ev->node_id) +
               cbor_head_len((uint64_t)tl_type_to_int(ev->type)) +
               cbor_head_len(ev->value);
    if (ev->extra_len > 0) n += cbor_head_len(ev->extra_len) + ev->extra_len;
    return n;
}

int tl_events_to_cbor(struct timeline_s *tl_ptr, uint8_t *buf, int buf_size,
                      int *events_written) {
    timeline_t *tl = (timeline_t *)tl_ptr;
    *events_written = 0;
    if (tl->new_count == 0 || buf_size <= 9) return 0;

    /* Budget first, so frame bytes never cost an event: as many entries
     * as fit without their bytes (the rest wait for the next delta), and
     * what that leaves over goes to frame bytes, oldest first.  9 bytes
     * are the outer array's head at its widest. */
    int start = (tl->head + tl->count - tl->new_count) % TL_MAX_EVENTS;
    size_t room = (size_t)buf_size - 9, need = 0;
    int n = 0;
    for (; n < tl->new_count; n++) {
        size_t e = tl_entry_cbor_len(&tl->events[(start + n) % TL_MAX_EVENTS]);
        if (need + e > room) break;
        need += e;
    }
    size_t spare = room - need;

    cbor_writer_state_t w;
    cbor_init_writer(&w, buf, (size_t)buf_size);

    cbor_open_array(&w);
    for (int i = 0; i < n; i++) {
        int idx = (start + i) % TL_MAX_EVENTS;
        const tl_event_t *ev = &tl->events[idx];
        cbor_open_array(&w);
        /* time in microseconds as integer (avoids float) */
        cbor_write_unsigned(&w, (uint64_t)(ev->time_ns / 1000));
        cbor_write_unsigned(&w, ev->node_id);
        cbor_write_unsigned(&w, (uint64_t)tl_type_to_int(ev->type));
        cbor_write_unsigned(&w, ev->value);
        int blen = 0;
        const uint8_t *b = tl_frame_bytes(tl, ev, &blen);
        /* A frame's bytes follow its summary (an empty one if it has
         * none), while the budget has room for them. */
        if (b) {
            size_t cost = (ev->extra_len > 0 ? 0 : 1) +
                          cbor_head_len((uint64_t)blen) + (size_t)blen;
            if (cost <= spare) spare -= cost;
            else b = NULL;
        }
        if (ev->extra_len > 0 || b)
            cbor_write_text(&w, ev->extra, ev->extra_len);
        if (b)
            cbor_write_data(&w, b, (size_t)blen);
        cbor_close_array(&w);
    }
    cbor_close_array(&w);

    size_t len = cbor_end_writer(&w);
    if (len > 0) *events_written = n;
    return (int)len;
}

int sim_state_delta_cbor(uint8_t *buf, int buf_size,
                         const sim_stats_t *stats,
                         const sim_node_state_t *node_states,
                         const sim_node_state_t *prev_node_states,
                         int node_count,
                         const int *node_ids,
                         const int64_t *node_last_tx_ns,
                         const int64_t *prev_last_tx_ns,
                         const char ***console_lines,
                         const int *console_counts,
                         const uint8_t *tl_cbor,
                         int tl_cbor_len) {
    cbor_writer_state_t w;
    cbor_init_writer(&w, buf, (size_t)buf_size);

    cbor_open_map(&w);

    /* Key 0: sim_time_ms */
    cbor_write_unsigned(&w, 0);
    cbor_write_unsigned(&w, (uint64_t)(stats->sim_time_ns / MS_TO_NS));

    /* Key 1: stats [rf_bytes, uart_bytes, rx_queued, rx_collided, speed_x10, paused] */
    cbor_write_unsigned(&w, 1);
    cbor_open_array(&w);
    cbor_write_unsigned(&w, (uint64_t)stats->rf_bytes);
    cbor_write_unsigned(&w, (uint64_t)stats->uart_bytes);
    cbor_write_unsigned(&w, (uint64_t)stats->rx_frames_queued);
    cbor_write_unsigned(&w, (uint64_t)stats->rx_frames_collided);
    cbor_write_unsigned(&w, (uint64_t)(stats->speed_ratio * 10.0));
    cbor_write_bool(&w, stats->paused);
    cbor_close_array(&w);

    /* Key 2: radio changes { node_id: state_enum } — only changed nodes */
    if (node_states && prev_node_states) {
        int radio_changes = 0;
        for (int i = 0; i < node_count; i++)
            if (node_states[i].radio_state != prev_node_states[i].radio_state)
                radio_changes++;
        if (radio_changes > 0) {
            cbor_write_unsigned(&w, 2);
            cbor_open_map(&w);
            for (int i = 0; i < node_count; i++) {
                if (node_states[i].radio_state != prev_node_states[i].radio_state) {
                    cbor_write_unsigned(&w, (uint64_t)node_ids[i]);
                    cbor_write_unsigned(&w, (uint64_t)node_states[i].radio_state);
                }
            }
            cbor_close_map(&w);
        }
    }

    /* Key 3: LED changes { node_id: [r,g,b] } — only changed nodes */
    if (node_states && prev_node_states) {
        int led_changes = 0;
        for (int i = 0; i < node_count; i++)
            if (memcmp(node_states[i].led, prev_node_states[i].led, SIM_MAX_LEDS) != 0)
                led_changes++;
        if (led_changes > 0) {
            cbor_write_unsigned(&w, 3);
            cbor_open_map(&w);
            for (int i = 0; i < node_count; i++) {
                if (memcmp(node_states[i].led, prev_node_states[i].led, SIM_MAX_LEDS) != 0) {
                    cbor_write_unsigned(&w, (uint64_t)node_ids[i]);
                    cbor_open_array(&w);
                    for (int l = 0; l < SIM_MAX_LEDS; l++)
                        cbor_write_unsigned(&w, node_states[i].led[l]);
                    cbor_close_array(&w);
                }
            }
            cbor_close_map(&w);
        }
    }

    /* Key 4: last_tx_ms { node_id: ms } — only nodes that transmitted since last update */
    if (node_last_tx_ns && prev_last_tx_ns) {
        int tx_changes = 0;
        for (int i = 0; i < node_count; i++)
            if (node_last_tx_ns[i] != prev_last_tx_ns[i])
                tx_changes++;
        if (tx_changes > 0) {
            cbor_write_unsigned(&w, 4);
            cbor_open_map(&w);
            for (int i = 0; i < node_count; i++) {
                if (node_last_tx_ns[i] != prev_last_tx_ns[i]) {
                    cbor_write_unsigned(&w, (uint64_t)node_ids[i]);
                    cbor_write_unsigned(&w, (uint64_t)(node_last_tx_ns[i] / MS_TO_NS));
                }
            }
            cbor_close_map(&w);
        }
    }

    /* Key 5: console { node_id: [lines...] } — only nodes with new output */
    if (console_lines && console_counts) {
        int con_changes = 0;
        for (int i = 0; i < node_count; i++)
            if (console_counts[i] > 0) con_changes++;
        if (con_changes > 0) {
            cbor_write_unsigned(&w, 5);
            cbor_open_map(&w);
            for (int i = 0; i < node_count; i++) {
                if (console_counts[i] > 0) {
                    cbor_write_unsigned(&w, (uint64_t)node_ids[i]);
                    cbor_open_array(&w);
                    for (int c = 0; c < console_counts[i]; c++)
                        cbor_write_text(&w, console_lines[i][c],
                                        strlen(console_lines[i][c]));
                    cbor_close_array(&w);
                }
            }
            cbor_close_map(&w);
        }
    }

    /* Key 6: timeline (pre-encoded CBOR array, embed as raw object) */
    if (tl_cbor && tl_cbor_len > 0) {
        cbor_write_unsigned(&w, 6);
        cbor_write_object(&w, tl_cbor, (size_t)tl_cbor_len);
    }

    cbor_close_map(&w);

    return (int)cbor_end_writer(&w);
}
