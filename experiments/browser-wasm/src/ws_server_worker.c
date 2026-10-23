/*
 * ws_server_worker — ws_server.h for the browser build.
 *
 * The native ws_server.c serves the UI over a WebSocket.  In the browser the
 * simulator runs in a Web Worker and the page is the only client, so the
 * "socket" is the worker's message port: broadcasts are postMessage()s, and
 * the page's commands arrive through csim_wasm_ui_recv().  Everything above
 * this interface (websocket_ui_service, the runner loop, the UI page) is the
 * same code the native build runs.
 *
 * The poll is also where the simulation gives the worker its event loop
 * back.  The runner calls ui_service_poll() on every pass of its main loop,
 * running or paused, so yielding here (emscripten_sleep, ASYNCIFY) lets the
 * page's pause / speed / move commands in without any change to the loop.
 * Yields are rate-limited by wall time: one per pass would cap a fast run at
 * the browser's setTimeout granularity.  The runner's pacing waits go
 * through usleep(), which is redefined below to yield instead of spinning.
 *
 * Yielding never changes the simulation: commands are applied between loop
 * passes, which is where the native WebSocket poll applies them too.
 */
#include "ws_server.h"

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Wall time between yields while the simulation runs flat out (ms). */
#define YIELD_INTERVAL_MS 30.0

struct ws_server {
    ws_message_cb_t cb;
    void           *userdata;
};

static ws_server_t g_server;
static int         g_server_live;

/* Commands from the page, queued while the simulation is suspended and
 * handed to the UI service at the next poll — never from inside a JS
 * callback, so the service is not re-entered mid-pass. */
typedef struct inbound {
    struct inbound *next;
    int             len;
    char            data[];
} inbound_t;

static inbound_t *g_in_head, *g_in_tail;
static double     g_last_yield_ms;

EMSCRIPTEN_KEEPALIVE void csim_wasm_ui_recv(const char *data, int len) {
    if (len < 0) return;
    inbound_t *m = malloc(sizeof *m + (size_t)len + 1);
    if (!m) return;
    m->next = NULL;
    m->len = len;
    memcpy(m->data, data, (size_t)len);
    m->data[len] = '\0';
    if (g_in_tail) g_in_tail->next = m; else g_in_head = m;
    g_in_tail = m;
}

/* Outside a worker (the Node profiling build) there is no page: the frames
 * are built exactly as in the browser and then dropped. */
EM_JS(void, post_text, (const char *data, int len), {
    const s = UTF8ToString(data, len);
    if (typeof postMessage === 'function') postMessage({ text: s });
});

EM_JS(void, post_binary, (const uint8_t *data, int len), {
    const buf = HEAPU8.slice(data, data + len).buffer;
    if (typeof postMessage === 'function') postMessage({ binary: buf }, [buf]);
});

static void drain_inbound(void) {
    while (g_in_head) {
        inbound_t *m = g_in_head;
        g_in_head = m->next;
        if (!g_in_head) g_in_tail = NULL;
        if (g_server_live && g_server.cb) g_server.cb(m->data, m->len, g_server.userdata);
        free(m);
    }
}

ws_server_t *ws_server_init(const char *bind_addr, int port) {
    (void)bind_addr; (void)port;
    memset(&g_server, 0, sizeof g_server);
    g_server_live = 1;
    g_last_yield_ms = emscripten_get_now();
    return &g_server;
}

int ws_server_bind_addr_valid(const char *bind_addr) { (void)bind_addr; return 1; }

void ws_server_url(const char *bind_addr, int port, char *out, size_t outsz) {
    (void)bind_addr; (void)port;
    if (outsz) { strncpy(out, "worker:", outsz - 1); out[outsz - 1] = '\0'; }
}

void ws_server_poll(ws_server_t *srv) {
    double now = emscripten_get_now();
    if (now - g_last_yield_ms >= YIELD_INTERVAL_MS) {
        emscripten_sleep(0);
        g_last_yield_ms = emscripten_get_now();
    }
    if (srv) drain_inbound();
}

void ws_server_broadcast(ws_server_t *srv, const char *data, int len) {
    if (srv && len > 0) post_text(data, len);
}

void ws_server_broadcast_binary(ws_server_t *srv, const uint8_t *data, int len) {
    if (srv && len > 0) post_binary(data, len);
}

/* The page is served by the static host, not by the simulator. */
int ws_server_set_html(ws_server_t *srv, const char *html, int len) {
    (void)srv; (void)html; (void)len;
    return 0;
}

void ws_server_set_message_callback(ws_server_t *srv, ws_message_cb_t cb, void *userdata) {
    if (!srv) return;
    srv->cb = cb;
    srv->userdata = userdata;
}

/* The page that started the worker is always connected. */
int ws_server_client_count(ws_server_t *srv) { return srv ? 1 : 0; }

void ws_server_destroy(ws_server_t *srv) {
    if (srv) g_server_live = 0;
}

/* The runner's wall-clock pacing sleeps in usleep(); in a worker that would
 * spin.  Sleep by yielding to the event loop instead, which also lets the
 * page's commands in while the simulation waits for the wall clock. */
int usleep(useconds_t us) {
    emscripten_sleep((unsigned)(us / 1000));
    g_last_yield_ms = emscripten_get_now();
    return 0;
}
