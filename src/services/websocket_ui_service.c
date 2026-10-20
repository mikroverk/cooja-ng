/*
 * Implementation of the WebSocket UI service — see
 * include/sim/websocket_ui_service.h.
 *
 * Phase 6 milestone 39 (§3.19).  Verbatim moves of the runner's
 * ui_message_handler, ui_add_console_line, the ws_server init, and the
 * full-state-JSON / CBOR-delta broadcast block, retargeted onto the
 * service struct + its stored shared-state pointers and describe/move
 * callbacks.  The wall-clock pacing and loop control stay in the runner.
 */
#include "websocket_ui_service.h"
#include "sim_runtime.h"      /* sim_runtime_ui_panels_json (plugin UI panels) */
#include "cbor_json.h"        /* --ui-record: a CBOR delta as JSON */

#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define UI_PAGE_PATH "ui/index.html"
/* The server answers GET / through the client's outgoing queue, which is
 * sized for this much page; a larger one would be cut off there. */
#define UI_PAGE_MAX  ((long)WS_SERVER_PAGE_MAX)

/* Serve ui/index.html if it is a regular file of at most UI_PAGE_MAX bytes;
 * otherwise say why and leave the server's built-in page.  The file is
 * opened non-blocking and checked with fstat before it is read: a FIFO with
 * no writer would block a plain fopen() before the simulation starts, an
 * unseekable stream has no length to size the buffer with, and a directory
 * opens and reads as an empty page. */
static void load_page(ws_server_t *server) {
    int fd = open(UI_PAGE_PATH, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "Warning: cannot open " UI_PAGE_PATH " (%s), serving "
                        "default page\n", strerror(errno));
        return;
    }
    struct stat st;
    char *html = NULL;
    size_t got = 0;
    int ok = 0;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size <= UI_PAGE_MAX &&
        (html = malloc((size_t)st.st_size + 1)) != NULL) {
        ok = 1;
        while (got < (size_t)st.st_size) {
            ssize_t n = read(fd, html + got, (size_t)st.st_size - got);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) { ok = 0; break; }
            if (n == 0) break;      /* shorter than fstat said: serve that */
            got += (size_t)n;
        }
    }
    if (ok)
        ok = ws_server_set_html(server, html, (int)got) == 0;
    if (!ok) {
        fprintf(stderr, "Warning: " UI_PAGE_PATH " is not a readable file of "
                        "at most %ld bytes, serving default page\n", UI_PAGE_MAX);
    }
    free(html);
    close(fd);
}

void ui_service_poll(websocket_ui_service_t *svc) {
    if (svc->server) ws_server_poll(svc->server);
}

void ui_service_set_stats(websocket_ui_service_t *svc, int rf_bytes,
                          int uart_bytes, int rx_frames_queued,
                          int rx_frames_collided) {
    svc->stat_rf_bytes = rf_bytes;
    svc->stat_uart_bytes = uart_bytes;
    svc->stat_rx_frames_queued = rx_frames_queued;
    svc->stat_rx_frames_collided = rx_frames_collided;
}

/* Incoming UI command: speed / pause / play / full / restart / move. */
static void ui_message_handler(const char *data, int len, void *userdata) {
    websocket_ui_service_t *svc = (websocket_ui_service_t *)userdata;
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) return;
    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    /* Under an external clock source (Renode) the master owns time: pause,
     * play and speed would stall or desynchronise it, so they are ignored. */
    bool clock_external = svc->rt && svc->rt->clock_source;
    if (cmd && cJSON_IsString(cmd) && clock_external &&
        (strcmp(cmd->valuestring, "speed") == 0 ||
         strcmp(cmd->valuestring, "pause") == 0 ||
         strcmp(cmd->valuestring, "play") == 0)) {
        fprintf(stderr, "ui: '%s' ignored: an external clock source drives the simulation\n",
                cmd->valuestring);
    } else if (cmd && cJSON_IsString(cmd)) {
        if (strcmp(cmd->valuestring, "speed") == 0) {
            cJSON *val = cJSON_GetObjectItem(root, "value");
            if (val && cJSON_IsNumber(val)) {
                double v = val->valuedouble;
                if (v >= 0.1 && v <= 1000.0 && svc->ctl)
                    sim_control_set_speed(svc->ctl, v);
            }
        } else if (strcmp(cmd->valuestring, "pause") == 0) {
            if (svc->ctl) sim_control_pause(svc->ctl);
        } else if (strcmp(cmd->valuestring, "play") == 0) {
            if (svc->ctl) sim_control_resume(svc->ctl);
        } else if (strcmp(cmd->valuestring, "full") == 0) {
            svc->full_state_requested = 1;
        } else if (strcmp(cmd->valuestring, "restart") == 0) {
            svc->restart_requested = 1;
            if (svc->ctl) sim_control_resume(svc->ctl);
        } else if (strcmp(cmd->valuestring, "move") == 0) {
            cJSON *jnode = cJSON_GetObjectItem(root, "node");
            cJSON *jx = cJSON_GetObjectItem(root, "x");
            cJSON *jy = cJSON_GetObjectItem(root, "y");
            if (jnode && cJSON_IsNumber(jnode) && jx && cJSON_IsNumber(jx) &&
                jy && cJSON_IsNumber(jy) && svc->ctl) {
                sim_control_move(svc->ctl, jnode->valueint,
                                 jx->valuedouble, jy->valuedouble);
            }
        }
    }
    cJSON_Delete(root);
}

/* Copy src into dst (size n) as valid UTF-8: a byte that does not start a
 * complete, well-formed sequence becomes U+FFFD, and a sequence that would
 * not fit is left out rather than cut.  Console lines are raw UART bytes;
 * the full state travels as a WebSocket text frame and both it and a
 * recording are JSON, all of which must be UTF-8. */
static void utf8_sanitize(char *dst, size_t n, const char *src) {
    const unsigned char *p = (const unsigned char *)src;
    size_t o = 0;
    while (*p && o + 1 < n) {
        unsigned char c = p[0];
        int len = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2
                : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        for (int k = 1; k < len; k++)
            if ((p[k] & 0xC0) != 0x80) { len = 0; break; }
        if (len == 3 && ((c == 0xE0 && p[1] < 0xA0) || (c == 0xED && p[1] > 0x9F)))
            len = 0;                       /* overlong, or a UTF-16 surrogate */
        if (len == 4 && ((c == 0xF0 && p[1] < 0x90) || (c == 0xF4 && p[1] > 0x8F)))
            len = 0;                       /* overlong, or above U+10FFFF */
        if (len == 0) {
            if (o + 3 >= n) break;
            dst[o++] = (char)0xEF; dst[o++] = (char)0xBF; dst[o++] = (char)0xBD;
            p++;
            continue;
        }
        if (o + (size_t)len >= n) break;
        for (int k = 0; k < len; k++) dst[o++] = (char)p[k];
        p += len;
    }
    dst[o] = '\0';
}

void ui_service_add_console_line(websocket_ui_service_t *svc, int idx,
                                 int64_t sim_ns, const char *line) {
    /* Prepend simulation timestamp */
    char raw[UI_CONSOLE_LINELEN], stamped[UI_CONSOLE_LINELEN];
    double sim_s = (double)sim_ns / 1e9;
    snprintf(raw, sizeof(raw), "[%7.3f] %s", sim_s, line);
    utf8_sanitize(stamped, sizeof(stamped), raw);

    /* Add to ring buffer */
    int slot = (svc->console_head[idx] + svc->console_count[idx]) % UI_CONSOLE_LINES;
    if (svc->console_count[idx] < UI_CONSOLE_LINES)
        svc->console_count[idx]++;
    else
        svc->console_head[idx] = (svc->console_head[idx] + 1) % UI_CONSOLE_LINES;
    strncpy(svc->console[idx][slot], stamped, UI_CONSOLE_LINELEN - 1);
    svc->console[idx][slot][UI_CONSOLE_LINELEN - 1] = '\0';

    /* The recording's first full state carries every line since it began. */
    if (svc->record && svc->record_begun && svc->record_deltas < 0 &&
        svc->record_boot_n[idx] < 100000) {
        char **a = realloc(svc->record_boot[idx],
                           (size_t)(svc->record_boot_n[idx] + 1) * sizeof *a);
        char *line_copy = strdup(stamped);
        if (a && line_copy) {
            a[svc->record_boot_n[idx]++] = line_copy;
            svc->record_boot[idx] = a;
        } else {
            if (a) svc->record_boot[idx] = a;
            free(line_copy);
        }
    }

    /* Add to new-lines buffer for next broadcast */
    if (svc->console_new_count[idx] < UI_CONSOLE_LINES) {
        int ni = svc->console_new_count[idx]++;
        strncpy(svc->console_new[idx][ni], stamped, UI_CONSOLE_LINELEN - 1);
        svc->console_new[idx][ni][UI_CONSOLE_LINELEN - 1] = '\0';
    }
}

static void bind_state(websocket_ui_service_t *svc,
                       const sim_node_state_t *node_states,
                       sim_node_state_t *prev_node_states,
                       const int64_t *node_last_tx_ns,
                       int64_t *prev_last_tx_ns,
                       radio_medium_t *medium, timeline_t *tl,
                       const int *node_count,
                       ui_describe_fn describe, sim_control_t *ctl) {
    svc->node_states = node_states;
    svc->prev_node_states = prev_node_states;
    svc->node_last_tx_ns = node_last_tx_ns;
    svc->prev_last_tx_ns = prev_last_tx_ns;
    svc->medium = medium;
    svc->tl = tl;
    svc->node_count = node_count;
    svc->describe = describe;
    svc->ctl = ctl;
}

bool ui_service_start(websocket_ui_service_t *svc, const char *bind_addr, int port,
                      const sim_node_state_t *node_states,
                      sim_node_state_t *prev_node_states,
                      const int64_t *node_last_tx_ns,
                      int64_t *prev_last_tx_ns,
                      radio_medium_t *medium, timeline_t *tl,
                      const int *node_count,
                      ui_describe_fn describe, sim_control_t *ctl) {
    bind_state(svc, node_states, prev_node_states, node_last_tx_ns,
               prev_last_tx_ns, medium, tl, node_count, describe, ctl);

    svc->server = ws_server_init(bind_addr, port);
    if (!svc->server)
        return false;
    ws_server_set_message_callback(svc->server, ui_message_handler, svc);
    load_page(svc->server);
    svc->full_state_requested = 1;
    return true;
}

/* --- --ui-record ---------------------------------------------------------
 * The document is written as the run goes:
 *   {"format":"cooja-ng-ui-replay/1","run":{...},"full":{...},"deltas":[
 *    {...},{...},...]}
 * record_deltas is -1 until the first full state is in, so a delta can
 * never precede it.  Each delta is the CBOR map the server broadcasts,
 * transcoded, with the plugin panels of the same tick as key "p".  Status
 * goes to stderr only, so stdout stays byte-identical with the flag. */

bool ui_service_record_open(websocket_ui_service_t *svc, const char *path) {
    /* Create it (or check an existing one can be written) without
     * truncating: a run that fails before the recording begins leaves an
     * earlier recording at this path as it was.  _begin truncates. */
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f) {
        fprintf(stderr, "--ui-record: cannot create %s: %s\n", path, strerror(errno));
        if (fd >= 0) close(fd);
        return false;
    }
    svc->record = f;
    snprintf(svc->record_path, sizeof svc->record_path, "%s", path);
    svc->record_begun = false;
    svc->record_failed = false;
    return true;
}

void ui_service_record_begin(websocket_ui_service_t *svc,
                             const char *run_json, int64_t end_ns,
                             const sim_node_state_t *node_states,
                             sim_node_state_t *prev_node_states,
                             const int64_t *node_last_tx_ns,
                             int64_t *prev_last_tx_ns,
                             radio_medium_t *medium, timeline_t *tl,
                             const int *node_count,
                             ui_describe_fn describe, sim_control_t *ctl) {
    if (!svc->record || svc->record_begun)
        return;     /* not asked for, finished (a restart), or begun already */
    bind_state(svc, node_states, prev_node_states, node_last_tx_ns,
               prev_last_tx_ns, medium, tl, node_count, describe, ctl);
    if (ftruncate(fileno(svc->record), 0) != 0)
        svc->record_failed = true;
    fprintf(svc->record, "{\"format\":\"cooja-ng-ui-replay/1\",\"run\":%s,",
            run_json && run_json[0] ? run_json : "{}");
    svc->record_begun = true;
    svc->record_deltas = -1;
    svc->record_end_ns = end_ns;
    svc->record_last_ns = INT64_MIN;
    svc->full_state_requested = 1;
    fprintf(stderr, "  --ui-record: recording the web UI stream to %s\n",
            svc->record_path);
}

static void record_boot_free(websocket_ui_service_t *svc) {
    for (int i = 0; i < UI_SVC_MAX_NODES; i++) {
        for (int c = 0; c < svc->record_boot_n[i]; c++)
            free(svc->record_boot[i][c]);
        free(svc->record_boot[i]);
        svc->record_boot[i] = NULL;
        svc->record_boot_n[i] = 0;
    }
}

/* Finish the document, once.  A write error anywhere (ferror is sticky) or
 * a failing close means the file is not the run: say so, and flag it for
 * the runner's exit code. */
static void record_finish(websocket_ui_service_t *svc, const char *why) {
    if (!svc->record) return;
    FILE *f = svc->record;
    svc->record = NULL;
    record_boot_free(svc);
    if (!svc->record_begun) {
        if (fclose(f) != 0) svc->record_failed = true;
        fprintf(stderr, "UI recording: none — the run ended before it began "
                        "(%s); %s is empty\n", why, svc->record_path);
        return;
    }
    if (svc->record_deltas < 0)
        fputs("\"full\":null,\"deltas\":[", f);
    fputs("]}\n", f);
    /* ferror is sticky but says nothing of why; only a failing fclose
     * leaves a reason in errno. */
    const char *reason = ferror(f) ? "write error" : NULL;
    if (fclose(f) != 0) reason = strerror(errno);
    if (reason || svc->record_failed) {
        svc->record_failed = true;
        fprintf(stderr, "--ui-record: writing %s failed (%s): the recording "
                        "is incomplete\n", svc->record_path,
                reason ? reason : "a delta was left out");
        return;
    }
    fprintf(stderr, "UI recording: %ld deltas written to %s (%s)\n",
            svc->record_deltas < 0 ? 0 : svc->record_deltas,
            svc->record_path, why);
}

/* This tick's delta — what changed since the previous one — in
 * delta_cbor_buf; consumes the new console lines and timeline events and
 * advances the previous-state snapshot.  Returns its length (0 = none). */
static uint8_t delta_cbor_buf[524288];
static int build_delta(websocket_ui_service_t *svc, const sim_stats_t *st, int n) {
    int ids[UI_SVC_MAX_NODES];
    const char *con_new_ptrs[UI_SVC_MAX_NODES][UI_CONSOLE_LINES];
    const char **con_ptr_arr[UI_SVC_MAX_NODES];
    int con_counts[UI_SVC_MAX_NODES];

    for (int i = 0; i < n; i++) {
        int id = 0; const char *type = ""; int64_t cycles = 0;
        uint32_t freq = 0; int64_t simt = 0;
        if (svc->describe) svc->describe(i, &id, &type, &cycles, &freq, &simt);
        ids[i] = id;
        con_counts[i] = svc->console_new_count[i];
        for (int c = 0; c < svc->console_new_count[i]; c++)
            con_new_ptrs[i][c] = svc->console_new[i][c];
        con_ptr_arr[i] = con_new_ptrs[i];
        svc->console_new_count[i] = 0;
    }

    static uint8_t tl_cbor_buf[262144];
    int tl_cbor_len = tl_events_to_cbor(
        (struct timeline_s *)svc->tl, tl_cbor_buf, (int)sizeof(tl_cbor_buf));
    tl_flush_new(svc->tl);

    int cbor_len = sim_state_delta_cbor(
        delta_cbor_buf, (int)sizeof(delta_cbor_buf),
        st, svc->node_states, svc->prev_node_states,
        n, ids,
        svc->node_last_tx_ns, svc->prev_last_tx_ns,
        (const char ***)con_ptr_arr, con_counts,
        tl_cbor_buf, tl_cbor_len);

    /* Save current state as previous for next delta */
    memcpy(svc->prev_node_states, svc->node_states,
           sizeof(sim_node_state_t) * UI_SVC_MAX_NODES);
    memcpy(svc->prev_last_tx_ns, svc->node_last_tx_ns,
           sizeof(int64_t) * UI_SVC_MAX_NODES);
    return cbor_len;
}

/* One delta: the CBOR map as JSON, with the tick's panels spliced in
 * before its closing brace. */
static void record_delta(websocket_ui_service_t *svc, const uint8_t *cbor,
                         int len, const char *panels) {
    char *js = NULL;
    size_t js_len = 0;
    FILE *m = open_memstream(&js, &js_len);
    if (!m) {
        fprintf(stderr, "--ui-record: out of memory; a delta is left out\n");
        svc->record_failed = true;
        return;
    }
    long used = cbor_item_to_json(cbor, (size_t)len, m);
    fclose(m);
    if (used == len && js_len >= 2 && js[0] == '{' && js[js_len - 1] == '}') {
        if (svc->record_deltas > 0) fputc(',', svc->record);
        fwrite(js, 1, js_len - 1, svc->record);
        /* Panels come from plugins as JSON text; one that does not parse
         * would make the whole document unreadable, so it is left out of
         * this delta (the live UI only loses that one frame either). */
        if (panels) {
            cJSON *pj = cJSON_Parse(panels);
            if (pj) fprintf(svc->record, ",\"p\":%s", panels);
            else if (!svc->record_bad_panels++)
                fprintf(stderr, "--ui-record: a plugin published panels that "
                                "are not JSON; left out of the recording\n");
            cJSON_Delete(pj);
        }
        fputc('}', svc->record);
        svc->record_deltas++;
    } else {
        fprintf(stderr, "--ui-record: a %d-byte delta did not transcode; "
                        "left out of the recording\n", len);
        svc->record_failed = true;
    }
    free(js);
}

/* One broadcast tick.  recorder_only: the closing tick of a recording,
 * which live clients do not get; force: record it even if simulated time has
 * not moved since the last recorded tick (events may have come in since). */
static void broadcast_tick(websocket_ui_service_t *svc, int64_t sim_ns,
                           int recorder_only, int force) {
    if (!svc->server && !svc->record) return;
    int n = svc->node_count ? *svc->node_count : 0;

    sim_stats_t st = {
        .sim_time_ns = sim_ns,
        .rf_bytes = svc->stat_rf_bytes,
        .uart_bytes = svc->stat_uart_bytes,
        .rx_frames_queued = svc->stat_rx_frames_queued,
        .rx_frames_collided = svc->stat_rx_frames_collided,
        .speed_ratio = svc->ctl ? sim_control_speed(svc->ctl) : 0.0,
        .paused = svc->ctl ? (sim_control_paused(svc->ctl) ? 1 : 0) : 0,
    };

    /* The recording is a client that is always there — for ticks that
     * moved.  A paused run (a shell prompt, the UI's pause button) comes
     * here every ~50 ms of wall time with simulated time standing still;
     * each pass would otherwise be one more identical delta in the file. */
    int live = !recorder_only && svc->server &&
               ws_server_client_count(svc->server) > 0;
    int rec = svc->record && svc->record_begun &&
              (force || sim_ns != svc->record_last_ns);
    int has_clients = live || rec;
    char *json = NULL;
    /* Plugin panels for this tick: the live frame below, and key "p" of the
     * recorded delta. */
    char *panels = (has_clients && svc->rt) ? sim_runtime_ui_panels_json(svc->rt)
                                            : NULL;

    /* A full state is for a client that has none: a browser connecting,
     * or the recording before its first one.  A recording that has its
     * full state already gets this tick as a delta first — building the
     * full state flushes the tick's console lines and timeline events, and
     * they would otherwise be in neither. */
    int want_full = svc->full_state_requested &&
                    (live || (rec && svc->record_deltas < 0));
    if (want_full && rec && svc->record_deltas >= 0) {
        int cbor_len = build_delta(svc, &st, n);
        if (cbor_len > 0) {
            record_delta(svc, delta_cbor_buf, cbor_len, panels);
            svc->record_last_ns = sim_ns;
        }
    }

    if (want_full) {
        /* === Full state: on connect/reload === */
        svc->full_state_requested = 0;

        sim_node_info_t ni[UI_SVC_MAX_NODES] = {{0}};
        const char *con_ptrs[UI_SVC_MAX_NODES][UI_CONSOLE_LINES];
        for (int i = 0; i < n; i++) {
            int id = 0; const char *type = ""; int64_t cycles = 0;
            uint32_t freq = 0; int64_t simt = 0;
            if (svc->describe) svc->describe(i, &id, &type, &cycles, &freq, &simt);
            ni[i].id = id;
            ni[i].type = type;
            ni[i].x = svc->medium->nodes[i].x;
            ni[i].y = svc->medium->nodes[i].y;
            ni[i].cycles = cycles;
            ni[i].freq_hz = freq;
            ni[i].sim_time_ns = simt;
            ni[i].last_tx_ns = svc->node_last_tx_ns[i];
            /* Send ALL console history for full state — for the
             * recording's first one, every line since it began. */
            if (rec && svc->record_deltas < 0 &&
                svc->record_boot_n[i] > svc->console_count[i]) {
                ni[i].console_count = svc->record_boot_n[i];
                ni[i].console = (const char **)svc->record_boot[i];
                continue;
            }
            ni[i].console_count = svc->console_count[i];
            /* console_head is the oldest line (add_console_line writes at
             * head + count); reading from head - count returned empty
             * slots until the ring had wrapped. */
            int base = svc->console_head[i];
            for (int c = 0; c < svc->console_count[i]; c++)
                con_ptrs[i][c] = svc->console[i][(base + c) % UI_CONSOLE_LINES];
            ni[i].console = con_ptrs[i];
        }

        int neighbor_counts[UI_SVC_MAX_NODES];
        const int *neighbor_ptrs[UI_SVC_MAX_NODES];
        for (int i = 0; i < n; i++) {
            neighbor_ptrs[i] = svc->medium->neighbors[i].neighbors;
            neighbor_counts[i] = svc->medium->neighbors[i].count;
        }
        sim_radio_info_t ri = {
            .type = svc->medium->type == RADIO_MEDIUM_UDGM ? "UDGM" : "NONE",
            .tx_range = svc->medium->udgm.tx_range,
            .node_count = n,
            .neighbors = neighbor_ptrs,
            .neighbor_counts = neighbor_counts,
        };

        /* Skip timeline in full state — the viewer accumulates events from
         * the CBOR deltas after connect (keeps full state ~10 KB). */
        json = sim_state_to_json(ni, n, &ri, &st, svc->node_states, NULL);
        /* Flush old timeline events so the first delta only contains events
         * from NOW, not stale history. */
        tl_flush_new(svc->tl);
        for (int i = 0; i < n; i++)
            svc->console_new_count[i] = 0;

    } else if (has_clients) {
        /* === Delta: CBOR binary, change-only === */
        int cbor_len = build_delta(svc, &st, n);
        if (cbor_len > 0 && live)
            ws_server_broadcast_binary(svc->server, delta_cbor_buf, cbor_len);
        if (cbor_len > 0 && rec && svc->record_deltas >= 0) {
            record_delta(svc, delta_cbor_buf, cbor_len, panels);
            svc->record_last_ns = sim_ns;
        }
    }

    if (json) {
        if (live)
            ws_server_broadcast(svc->server, json, (int)strlen(json));
        /* The recording takes the first full state only: the replay player
         * rebuilds from it, and a later one (a browser connecting; this
         * tick went into the file as a delta above) has no place in the
         * document. */
        if (rec && svc->record_deltas < 0) {
            fprintf(svc->record, "\"full\":%s,\"deltas\":[", json);
            svc->record_deltas = 0;
            svc->record_last_ns = sim_ns;
            record_boot_free(svc);
        }
        free(json);
    }

    /* Plugin UI panels — a small text frame, independent of the full/CBOR
     * state paths.  Sent every broadcast tick so panels track live; the
     * front-end replaces its panel set from this message. */
    if (panels && live) {
        size_t need = strlen(panels) + 40;
        char *msg = malloc(need);
        if (msg) {
            int len = snprintf(msg, need,
                               "{\"type\":\"panels\",\"panels\":%s}", panels);
            ws_server_broadcast(svc->server, msg, len);
            free(msg);
        }
    }
    free(panels);

    /* A --ui run goes on past its end; the recording does not. */
    if (svc->record && svc->record_begun && sim_ns > svc->record_end_ns)
        record_finish(svc, "end of run");
}

void ui_service_broadcast(websocket_ui_service_t *svc, int64_t sim_ns) {
    broadcast_tick(svc, sim_ns, 0, 0);
}

void ui_service_record_finish(websocket_ui_service_t *svc, int64_t sim_ns,
                              const char *why) {
    if (!svc->record) return;
    if (svc->record_begun)
        broadcast_tick(svc, sim_ns, 1, 1);   /* the run's last interval */
    record_finish(svc, why);
}

void ui_service_reset(websocket_ui_service_t *svc) {
    memset(svc->console, 0, sizeof(svc->console));
    memset(svc->console_head, 0, sizeof(svc->console_head));
    memset(svc->console_count, 0, sizeof(svc->console_count));
    memset(svc->console_new, 0, sizeof(svc->console_new));
    memset(svc->console_new_count, 0, sizeof(svc->console_new_count));
    svc->full_state_requested = 1;
    /* A restart starts simulation time over, which a replay document has
     * no way to say: the recording ends with the run that came before.  The
     * runner closes it with ui_service_record_finish first, which records
     * the run's last interval; this only catches a caller that did not. */
    record_finish(svc, "restart");
}

void ui_service_destroy(websocket_ui_service_t *svc) {
    record_finish(svc, "end of run");
    if (svc->server) {
        ws_server_destroy(svc->server);
        svc->server = NULL;
    }
}
