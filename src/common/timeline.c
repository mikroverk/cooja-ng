/*
 * Timeline event logger implementation
 */
#include "timeline.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

void tl_init(timeline_t *tl) {
    memset(tl, 0, sizeof(*tl));
    tl->enabled = 1;
}

static void tl_push(timeline_t *tl, const tl_event_t *ev) {
    if (!tl->enabled) return;

    int idx;
    if (tl->count < TL_MAX_EVENTS) {
        idx = (tl->head + tl->count) % TL_MAX_EVENTS;
        tl->count++;
    } else {
        /* Ring buffer full: overwrite oldest */
        idx = tl->head;
        tl->head = (tl->head + 1) % TL_MAX_EVENTS;
        if (tl->new_count > 0) tl->new_count--;
    }
    tl->events[idx] = *ev;
    tl->new_count++;
}

void tl_radio_event(timeline_t *tl, int node_id, int64_t time_ns,
                    tl_event_type_t type) {
    tl_event_t ev = {
        .time_ns = time_ns,
        .node_id = (uint8_t)node_id,
        .type = (uint8_t)type,
        .value = 0,
        .extra_len = 0,
    };
    tl_push(tl, &ev);
}

void tl_led_event(timeline_t *tl, int node_id, int64_t time_ns,
                  int led_num, int on) {
    tl_event_t ev = {
        .time_ns = time_ns,
        .node_id = (uint8_t)node_id,
        .type = (uint8_t)(on ? TL_LED_ON : TL_LED_OFF),
        .value = (uint8_t)led_num,
        .extra_len = 0,
    };
    tl_push(tl, &ev);
}

void tl_frame_event(timeline_t *tl, int node_id, int64_t time_ns,
                    int is_tx, const char *summary) {
    tl_frame_event_bytes(tl, node_id, time_ns, is_tx, summary, NULL, 0);
}

void tl_frame_event_bytes(timeline_t *tl, int node_id, int64_t time_ns,
                          int is_tx, const char *summary,
                          const uint8_t *bytes, int len) {
    tl_event_t ev = {
        .time_ns = time_ns,
        .node_id = (uint8_t)node_id,
        .type = TL_RF_FRAME,
        .value = (uint8_t)(is_tx ? 1 : 0),
    };
    if (summary) {
        int slen = (int)strlen(summary);
        if (slen > (int)sizeof(ev.extra) - 1)
            slen = (int)sizeof(ev.extra) - 1;
        memcpy(ev.extra, summary, slen);
        ev.extra[slen] = '\0';
        ev.extra_len = (uint8_t)slen;
    }
    if (tl->enabled && bytes && len > 0 && len <= TL_FRAME_MAX) {
        if (++tl->frame_next == 0) tl->frame_next = 1;   /* 0 means "none" */
        int slot = (int)(tl->frame_next % TL_FRAME_SLOTS);
        tl->frame_seq_at[slot] = tl->frame_next;
        tl->frame_len[slot] = (uint8_t)len;
        memcpy(tl->frame_bytes[slot], bytes, (size_t)len);
        ev.frame_seq = tl->frame_next;
    }
    tl_push(tl, &ev);
}

const uint8_t *tl_frame_bytes(const timeline_t *tl, const tl_event_t *ev, int *len) {
    if (!ev->frame_seq) return NULL;
    int slot = (int)(ev->frame_seq % TL_FRAME_SLOTS);
    if (tl->frame_seq_at[slot] != ev->frame_seq) return NULL;
    *len = tl->frame_len[slot];
    return tl->frame_bytes[slot];
}

void tl_log_event(timeline_t *tl, int node_id, int64_t time_ns,
                  const char *line) {
    tl_event_t ev = {
        .time_ns = time_ns,
        .node_id = (uint8_t)node_id,
        .type = TL_LOG,
        .value = 0,
    };
    if (line) {
        int slen = (int)strlen(line);
        if (slen > (int)sizeof(ev.extra) - 1)
            slen = (int)sizeof(ev.extra) - 1;
        memcpy(ev.extra, line, slen);
        ev.extra[slen] = '\0';
        ev.extra_len = (uint8_t)slen;
    }
    tl_push(tl, &ev);
}

const tl_event_t *tl_get_new(timeline_t *tl, int *count) {
    *count = tl->new_count;
    if (tl->new_count == 0) return NULL;
    /* New events are the last new_count entries */
    return tl->events;  /* caller uses tl_flush_new after processing */
}

void tl_flush_new(timeline_t *tl) {
    tl->new_count = 0;
}

static const char *event_type_str(int type) {
    switch (type) {
    case TL_RADIO_OFF:  return "off";
    case TL_RADIO_ON:   return "on";
    case TL_RADIO_TX:   return "tx";
    case TL_RADIO_RX:   return "rx";
    case TL_RADIO_INTF: return "intf";
    case TL_LED_ON:     return "led_on";
    case TL_LED_OFF:    return "led_off";
    case TL_RF_FRAME:   return "frame";
    case TL_LOG:        return "log";
    default:            return "?";
    }
}

/* Serialize helper: add event to cJSON array */
static void tl_event_to_cjson(const timeline_t *tl, cJSON *arr, const tl_event_t *ev) {
    cJSON *jev = cJSON_CreateObject();
    cJSON_AddNumberToObject(jev, "t", (double)ev->time_ns / 1000000.0);
    cJSON_AddNumberToObject(jev, "n", ev->node_id);
    cJSON_AddStringToObject(jev, "e", event_type_str(ev->type));
    cJSON_AddNumberToObject(jev, "v", ev->value);
    if (ev->extra_len > 0)
        cJSON_AddStringToObject(jev, "s", ev->extra);
    int blen = 0;
    const uint8_t *b = tl_frame_bytes(tl, ev, &blen);
    if (b) {
        char hex[2 * TL_FRAME_MAX + 1];
        for (int i = 0; i < blen; i++) snprintf(hex + 2 * i, 3, "%02x", b[i]);
        hex[2 * blen] = '\0';
        cJSON_AddStringToObject(jev, "b", hex);   /* the frame, for a dissector */
    }
    cJSON_AddItemToArray(arr, jev);
}

char *tl_events_to_json(timeline_t *tl) {
    if (tl->new_count == 0) return NULL;

    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    /* Iterate over new events (tail of ring buffer) */
    int start = (tl->head + tl->count - tl->new_count) % TL_MAX_EVENTS;
    for (int i = 0; i < tl->new_count; i++) {
        int idx = (start + i) % TL_MAX_EVENTS;
        tl_event_to_cjson(tl, arr, &tl->events[idx]);
    }

    /* NOTE: caller must call tl_flush_new() after successful delivery */

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

char *tl_all_events_to_json(timeline_t *tl) {
    if (tl->count == 0) return NULL;

    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    for (int i = 0; i < tl->count; i++) {
        int idx = (tl->head + i) % TL_MAX_EVENTS;
        tl_event_to_cjson(tl, arr, &tl->events[idx]);
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}
