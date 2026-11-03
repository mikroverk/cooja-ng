/*
 * sim_plugin — host-side plugin loader.  See include/sim/sim_plugin.h.
 *
 * Mirrors the dlopen pattern in src/native/native_node.c.  The plugin reaches
 * the host only through the csim_api vtables we pass it, so we need no
 * -rdynamic / -ldl and use RTLD_LOCAL (NOT RTLD_DEEPBIND — the plugin shares
 * nothing with the host by symbol name).
 */
#include "sim_plugin.h"
#include "csim_plugin.h"
#include "sim_registry.h"
#include "sim_runtime.h"   /* sim_runtime_ui_publish_panel (the v3 ui sink) */

#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

/* Host-routed logging: a plain stdout printf so plugin output orders with the
 * kernel's stdout output (determinism). */
static void plugin_log_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static const csim_log_ops_t g_log_ops = {
    .printf = plugin_log_printf,
};

/* A plugin's service ops, copied into host memory: the catalog keeps a
 * pointer, and a pre-v4 plugin's struct ends after `poll`, so the host must
 * not read the v4 members from it.  Copies live for the process (the .so,
 * whose functions they point at, stays loaded too). */
static int register_copy(sim_registry_t *reg, const sim_service_ops_t *ops,
                         bool v4) {
    if (!reg || !ops || !ops->name) return -1;
    sim_service_ops_t *copy = calloc(1, sizeof *copy);
    if (!copy) return -1;
    copy->name     = ops->name;
    copy->init     = ops->init;
    copy->destroy  = ops->destroy;
    copy->on_event = ops->on_event;
    copy->poll     = ops->poll;
    if (v4) {
        copy->on_restart   = ops->on_restart;
        copy->wants_frames = ops->wants_frames;
    }
    int idx = sim_registry_register_service(reg, copy);
    if (idx < 0) free(copy);
    return idx;
}

/* v1: the five members every ABI version has. */
static int plugin_register_service(sim_registry_t *reg, const sim_service_ops_t *ops) {
    return register_copy(reg, ops, false);
}

/* v4: with on_restart and wants_frames. */
static int plugin_register_service_v4(sim_registry_t *reg, const sim_service_ops_t *ops) {
    return register_copy(reg, ops, true);
}

/* The registry capability is the real catalog-append, called with the
 * registry the host hands the plugin via api->reg. */
static const csim_registry_ops_t g_registry_ops = {
    .register_service      = plugin_register_service,
    .register_radio_medium = sim_registry_register_radio_medium,
    .register_service_v4   = plugin_register_service_v4,
};

/* v3 UI capability: the publish-panel sink writes into the runtime the plugin
 * passes back (the opaque sim_runtime_t it received in its callbacks). */
static const csim_ui_ops_t g_ui_ops = {
    .publish_panel = sim_runtime_ui_publish_panel,
};

/* v4 firmware capability: named-variable reads of emulated nodes. */
static const csim_fw_ops_t g_fw_ops = {
    .resolve    = sim_fwvar_resolve,
    .read       = sim_fwvar_read,
    .node_count = sim_fwvar_node_count,
    .node_id    = sim_fwvar_node_id,
};

int sim_plugin_load(const char *path, sim_registry_t *reg,
                    char *errbuf, size_t errlen) {
    if (!path || !reg) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "null path/registry");
        return -1;
    }
    dlerror();  /* clear any stale error */
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "dlopen: %s", dlerror());
        return -1;
    }
    /* POSIX: cast through the object pointer to a function pointer. */
    int (*plugin_init)(const csim_api_t *) =
        (int (*)(const csim_api_t *))(uintptr_t)dlsym(handle, "csim_plugin_init");
    if (!plugin_init) {
        if (errbuf && errlen)
            snprintf(errbuf, errlen, "dlsym(csim_plugin_init): %s", dlerror());
        dlclose(handle);
        return -1;
    }
    csim_api_t api = {
        .version  = CSIM_PLUGIN_API_VERSION,
        .registry = &g_registry_ops,
        .log      = &g_log_ops,
        .reg      = reg,
        .ui       = &g_ui_ops,
        .fw       = &g_fw_ops,
    };
    int rc = plugin_init(&api);
    if (rc != 0) {
        /* Keep the handle loaded: the plugin may have registered services
         * whose ops point into this .so.  The caller skips the plugin's
         * services (it only attaches those registered without error). */
        if (errbuf && errlen)
            snprintf(errbuf, errlen, "csim_plugin_init returned %d", rc);
        return -1;
    }
    /* Success: leave the .so loaded for the process lifetime. */
    return 0;
}
