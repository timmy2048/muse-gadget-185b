/*
 * Chat-driven todo list app. See muse_todo.h / muse_todo_parse.h.
 *
 * Data source priority (mirrors muse_stock):
 *   1. NVS namespace "todo", key "list" (written by chat / device.todo / on-device)
 *   2. Remote https://raw.githubusercontent.com/timmy2048/muse-gadget-185b/main/todos.json
 *   3. Empty list
 *
 * Reminder: no vibration motor on Waveshare 1.85B (SKU 34554) → beep + TTS.
 *
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0.
 */

#include "muse_todo.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "muse_audio.h"
#include "muse_state.h"
#include "muse_text.h"
#include "muse_ui.h"
#include "muse_voice.h"

#ifdef ESP_PLATFORM
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "minimp3.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#endif

static const char *TAG = "muse_todo";

#define COLOR_BG 0x0b0b12
#define COLOR_TEXT 0xf2efff
#define COLOR_DIM 0x8b84a8
#define COLOR_CAPTION 0xd8d2ff
#define COLOR_ACCENT 0xa77dff
#define COLOR_ROW 0x1e1b33
#define COLOR_METER 0x1d1733
#define COLOR_WARN 0xffb020
#define COLOR_DANGER 0xff5c5c
#define COLOR_OK 0x35c26e

#define TODO_REMOTE_URL \
    "https://raw.githubusercontent.com/timmy2048/muse-gadget-185b/main/todos.json"
#define TODO_POLL_S 30.0f
#define TODO_JSON_CAP 3072

static muse_todo_item_t s_items[MUSE_TODO_MAX];
static int s_count;
static const char *s_src = "empty"; /* "nvs" | "remote" | "empty" */
static bool s_dirty_ui;
static bool s_loaded;

static lv_obj_t *s_root;
static lv_obj_t *s_list;
static lv_obj_t *s_sub;
static lv_obj_t *s_popup;
static lv_obj_t *s_popup_title;
static lv_obj_t *s_popup_body;
static int s_w, s_h;
static int s_remind_idx = -1;
static float s_next_poll;
static char s_remote_hash[40];

#ifdef ESP_PLATFORM
static SemaphoreHandle_t s_mtx;
static char s_remote_body[TODO_JSON_CAP];
static bool s_remote_ready;
static TaskHandle_t s_fetch_task;
static TaskHandle_t s_tts_task;
static char s_tts_pending[MUSE_TODO_TITLE_LEN + 16];
#endif

#if CONFIG_MUSE_CJK_FONT
LV_FONT_DECLARE(muse_font_cjk_16)
/* Same pattern as muse_settings_ui: Montserrat Latin + Unifont CJK fallback.
 * Do NOT use unscii_8 as primary with a 16px CJK fallback — mixed cell sizes
 * look like 乱码 on the round display. */
static const lv_font_t *todo_font(void)
{
    static lv_font_t font;
    static bool ready;
    if (!ready) {
        font = lv_font_montserrat_16;
        font.fallback = &muse_font_cjk_16;
        ready = true;
    }
    return &font;
}
static const lv_font_t *todo_font_sm(void)
{
    static lv_font_t font;
    static bool ready;
    if (!ready) {
        font = lv_font_montserrat_14;
        font.fallback = &muse_font_cjk_16;
        ready = true;
    }
    return &font;
}
static void set_text_cjk(lv_obj_t *l, const char *text)
{
    if (!l || !text) {
        return;
    }
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}
#else
static const lv_font_t *todo_font(void) { return &lv_font_montserrat_16; }
static const lv_font_t *todo_font_sm(void) { return &lv_font_montserrat_14; }
static void set_text_cjk(lv_obj_t *l, const char *text)
{
    char shown[128];
    if (!l || !text) {
        return;
    }
    text = muse_text_showable(text, shown, sizeof(shown));
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}
#endif

/* ------------------------------------------------------------------ */
/* Persistence                                                         */
/* ------------------------------------------------------------------ */

#ifdef ESP_PLATFORM
static bool nvs_load(char *buf, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open("todo", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = cap;
    esp_err_t e = nvs_get_str(h, "list", buf, &len);
    nvs_close(h);
    return e == ESP_OK && len > 2;
}

static bool nvs_save(const char *json)
{
    nvs_handle_t h;
    if (nvs_open("todo", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t e = nvs_set_str(h, "list", json);
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    return e == ESP_OK;
}
#else
static bool nvs_load(char *buf, size_t cap)
{
    FILE *f = fopen("nvs_todo_list.json", "r");
    if (!f) {
        return false;
    }
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n > 2;
}
static bool nvs_save(const char *json)
{
    FILE *f = fopen("nvs_todo_list.json", "w");
    if (!f) {
        return false;
    }
    fputs(json, f);
    fclose(f);
    return true;
}
#endif

static void persist(void)
{
    char json[TODO_JSON_CAP];
    if (muse_todo_list_to_json(s_items, s_count, json, sizeof(json)) < 0) {
        ESP_LOGW(TAG, "persist: serialize failed");
        return;
    }
    if (nvs_save(json)) {
        s_src = "nvs";
        ESP_LOGI(TAG, "todo saved (%d items)", s_count);
    } else {
        ESP_LOGW(TAG, "todo NVS save failed");
    }
}

static void ensure_ids(void)
{
    static int seq;
    for (int i = 0; i < s_count; i++) {
        if (s_items[i].id[0]) {
            continue;
        }
        snprintf(s_items[i].id, sizeof(s_items[i].id), "t%ld%02d",
                 (long)(time(NULL) % 100000), (seq++ + i) % 100);
    }
}

static void load_list(void)
{
    char body[TODO_JSON_CAP];
    s_count = 0;
    s_src = "empty";
    if (nvs_load(body, sizeof(body))) {
        int n = muse_todo_parse_list(body, s_items, MUSE_TODO_MAX);
        if (n >= 0) {
            s_count = n;
            s_src = "nvs";
            ESP_LOGI(TAG, "todo: %d from NVS", s_count);
            s_loaded = true;
            return;
        }
    }
#ifdef ESP_PLATFORM
    /* remote adopted on first fetch tick */
#else
    FILE *f = fopen("todos.json", "r");
    if (f) {
        size_t n = fread(body, 1, sizeof(body) - 1, f);
        fclose(f);
        body[n] = '\0';
        int c = muse_todo_parse_list(body, s_items, MUSE_TODO_MAX);
        if (c >= 0) {
            s_count = c;
            s_src = "remote";
        }
    }
#endif
    s_loaded = true;
}

/* ------------------------------------------------------------------ */
/* Command apply                                                       */
/* ------------------------------------------------------------------ */

static void lock(void)
{
#ifdef ESP_PLATFORM
    if (s_mtx) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
    }
#endif
}

static void unlock(void)
{
#ifdef ESP_PLATFORM
    if (s_mtx) {
        xSemaphoreGive(s_mtx);
    }
#endif
}

static int find_by_id_or_title(const char *id, const char *title)
{
    if (id && id[0]) {
        for (int i = 0; i < s_count; i++) {
            if (!strcmp(s_items[i].id, id)) {
                return i;
            }
        }
    }
    if (title && title[0]) {
        for (int i = 0; i < s_count; i++) {
            if (!strcmp(s_items[i].title, title)) {
                return i;
            }
        }
    }
    return -1;
}

int muse_todo_apply_cmd(const muse_todo_cmd_t *cmd)
{
    if (!cmd) {
        return -1;
    }
    if (!s_loaded) {
        load_list();
    }
    lock();
    int rc = -1;
    switch (cmd->op) {
    case MUSE_TODO_OP_ADD: {
        if (s_count >= MUSE_TODO_MAX || !cmd->title[0]) {
            break;
        }
        muse_todo_item_t *it = &s_items[s_count];
        memset(it, 0, sizeof(*it));
        if (cmd->id[0]) {
            snprintf(it->id, sizeof(it->id), "%s", cmd->id);
        } else {
            snprintf(it->id, sizeof(it->id), "t%ld%02d", (long)(time(NULL) % 100000), s_count);
        }
        /* Keep JSON-safe: drop double quotes inside the title. */
        size_t ti = 0;
        for (const char *s = cmd->title; *s && ti + 1 < sizeof(it->title); s++) {
            if (*s != '"') {
                it->title[ti++] = *s;
            }
        }
        it->title[ti] = '\0';
        it->due = cmd->due;
        it->done = false;
        it->reminded = false;
        s_count++;
        rc = 0;
        ESP_LOGI(TAG, "add '%s' due=%lld", it->title, (long long)it->due);
        break;
    }
    case MUSE_TODO_OP_COMPLETE: {
        int i = find_by_id_or_title(cmd->id, cmd->title);
        if (i >= 0) {
            s_items[i].done = true;
            s_items[i].reminded = true;
            rc = 0;
            ESP_LOGI(TAG, "complete '%s'", s_items[i].title);
        }
        break;
    }
    case MUSE_TODO_OP_REMOVE: {
        int i = find_by_id_or_title(cmd->id, cmd->title);
        if (i >= 0) {
            ESP_LOGI(TAG, "remove '%s'", s_items[i].title);
            memmove(&s_items[i], &s_items[i + 1], (size_t)(s_count - i - 1) * sizeof(s_items[0]));
            s_count--;
            rc = 0;
        }
        break;
    }
    case MUSE_TODO_OP_CLEAR:
        s_count = 0;
        rc = 0;
        ESP_LOGI(TAG, "clear all");
        break;
    case MUSE_TODO_OP_REPLACE:
        if (cmd->nitems < 0 || cmd->nitems > MUSE_TODO_MAX) {
            break;
        }
        memcpy(s_items, cmd->items, (size_t)cmd->nitems * sizeof(s_items[0]));
        s_count = cmd->nitems;
        ensure_ids();
        rc = 0;
        ESP_LOGI(TAG, "replace -> %d items", s_count);
        break;
    default:
        break;
    }
    if (rc == 0) {
        persist();
        s_dirty_ui = true;
    }
    unlock();
    return rc;
}

static int apply_cmd_cb(const muse_todo_cmd_t *cmd, void *user)
{
    (void)user;
    return muse_todo_apply_cmd(cmd);
}

int muse_todo_apply_chat_markers(char *text)
{
    if (!text || !strstr(text, MUSE_TODO_MARKER_OPEN)) {
        return 0;
    }
    return muse_todo_scan_markers(text, apply_cmd_cb, NULL);
}

int muse_todo_count(void)
{
    return s_count;
}

int muse_todo_get(int idx, muse_todo_item_t *out)
{
    if (!out || idx < 0 || idx >= s_count) {
        return -1;
    }
    *out = s_items[idx];
    return 0;
}

/* ------------------------------------------------------------------ */
/* Remote sync                                                         */
/* ------------------------------------------------------------------ */

#ifdef ESP_PLATFORM
static uint32_t body_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h ^= (uint8_t)*s;
        h *= 16777619u;
    }
    return h;
}

static int http_get(const char *url, char *buf, size_t cap, size_t *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return -1;
    }
    int status = -1;
    size_t len = 0;
    if (esp_http_client_open(client, 0) == ESP_OK &&
        esp_http_client_fetch_headers(client) >= 0) {
        status = esp_http_client_get_status_code(client);
        int n;
        while (len + 1 < cap &&
               (n = esp_http_client_read(client, buf + len, (int)(cap - 1 - len))) > 0) {
            len += (size_t)n;
        }
        buf[len] = '\0';
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (out_len) {
        *out_len = len;
    }
    return status;
}

static void todo_fetch_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((uint32_t)(TODO_POLL_S * 1000)));
        if (strcmp(s_src, "nvs") == 0) {
            continue; /* local edits win, same as stock */
        }
        char buf[TODO_JSON_CAP];
        size_t len = 0;
        int st = http_get(TODO_REMOTE_URL, buf, sizeof(buf), &len);
        if (st != 200 || len < 2) {
            ESP_LOGW(TAG, "remote todos fetch failed (%d)", st);
            continue;
        }
        lock();
        if (strcmp(s_remote_body, buf) != 0) {
            snprintf(s_remote_body, sizeof(s_remote_body), "%s", buf);
            s_remote_ready = true;
            s_dirty_ui = true;
            ESP_LOGI(TAG, "remote todos changed (%u bytes hash=%08lx)",
                     (unsigned)len, (unsigned long)body_hash(buf));
        }
        unlock();
    }
}

static void adopt_remote_if_needed(void)
{
    if (!s_remote_ready || strcmp(s_src, "nvs") == 0) {
        return;
    }
    lock();
    muse_todo_item_t tmp[MUSE_TODO_MAX];
    int n = muse_todo_parse_list(s_remote_body, tmp, MUSE_TODO_MAX);
    if (n >= 0) {
        memcpy(s_items, tmp, (size_t)n * sizeof(tmp[0]));
        s_count = n;
        ensure_ids();
        s_src = "remote";
        snprintf(s_remote_hash, sizeof(s_remote_hash), "%08lx", (unsigned long)body_hash(s_remote_body));
        ESP_LOGI(TAG, "adopted remote todos: %d", s_count);
    }
    s_remote_ready = false;
    unlock();
}

static void fetch_kick(void)
{
    if (!s_fetch_task) {
        xTaskCreate(todo_fetch_task, "todo_fetch", 6144, NULL, 3, &s_fetch_task);
    }
    if (s_fetch_task) {
        xTaskNotifyGive(s_fetch_task);
    }
}
#endif

/* ------------------------------------------------------------------ */
/* Reminder TTS (beep + HTTP MP3)                                      */
/* ------------------------------------------------------------------ */

#ifdef ESP_PLATFORM
static size_t urlencode(const char *in, char *out, size_t cap)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = '\0';
    return o;
}

static void todo_tts_task(void *arg)
{
    (void)arg;
    char title[sizeof(s_tts_pending)];
    lock();
    snprintf(title, sizeof(title), "%s", s_tts_pending);
    s_tts_pending[0] = '\0';
    unlock();

    muse_voice_request_chirp();
    vTaskDelay(pdMS_TO_TICKS(200));

    char phrase[MUSE_TODO_TITLE_LEN + 32];
    snprintf(phrase, sizeof(phrase), "该%s了", title);

    char enc[512];
    urlencode(phrase, enc, sizeof(enc));
    char url[640];
#if defined(CONFIG_MUSE_TTS_URL_ZH)
    const char *prefix = CONFIG_MUSE_TTS_URL_ZH[0] ? CONFIG_MUSE_TTS_URL_ZH
                                                    : "http://10.0.0.1:8787/tts?q=";
#else
    const char *prefix = "http://10.0.0.1:8787/tts?q=";
#endif
    if (snprintf(url, sizeof(url), "%s%s", prefix, enc) >= (int)sizeof(url)) {
        s_tts_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* Fetch MP3 into a heap buffer. */
    const size_t cap = 96 * 1024;
    uint8_t *mp3 = malloc(cap);
    if (!mp3) {
        s_tts_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    size_t len = 0;
    int st = http_get(url, (char *)mp3, cap, &len);
    if (st < 200 || st >= 300 || len < 64) {
        ESP_LOGW(TAG, "reminder TTS fetch failed st=%d len=%u", st, (unsigned)len);
        free(mp3);
        s_tts_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    muse_audio_power(true);
    mp3dec_t dec;
    mp3dec_init(&dec);
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t off = 0;
    while (off + 4 < len) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&dec, mp3 + off, (int)(len - off), pcm, &info);
        if (info.frame_bytes <= 0) {
            off++;
            continue;
        }
        off += (size_t)info.frame_bytes;
        if (samples <= 0) {
            continue;
        }
        /* Downmix / resample-ish: muse_audio wants 16 kHz mono. Many TTS are 16/24k.
         * If stereo, take left; if rate != 16k, naive drop/repeat. */
        int ch = info.channels > 0 ? info.channels : 1;
        int hz = info.hz > 0 ? info.hz : 16000;
        int16_t mono[1152];
        int nmono = samples;
        for (int i = 0; i < samples; i++) {
            mono[i] = pcm[i * ch];
        }
        if (hz == 16000) {
            muse_audio_write(mono, (size_t)nmono);
        } else if (hz == 24000) {
            /* 3:2 decimate */
            int out_n = 0;
            for (int i = 0; i + 2 < nmono; i += 3) {
                mono[out_n++] = mono[i];
                mono[out_n++] = mono[i + 1];
            }
            muse_audio_write(mono, (size_t)out_n);
        } else if (hz == 8000) {
            int16_t up[2304];
            int u = 0;
            for (int i = 0; i < nmono && u + 1 < 2304; i++) {
                up[u++] = mono[i];
                up[u++] = mono[i];
            }
            muse_audio_write(up, (size_t)u);
        } else {
            muse_audio_write(mono, (size_t)nmono);
        }
    }
    free(mp3);
    s_tts_task = NULL;
    vTaskDelete(NULL);
}

static void start_reminder_tts(const char *title)
{
    lock();
    snprintf(s_tts_pending, sizeof(s_tts_pending), "%s", title);
    unlock();
    if (s_tts_task) {
        return; /* already speaking */
    }
    /* minimp3 wants a roomy stack */
    if (xTaskCreate(todo_tts_task, "todo_tts", 16384, NULL, 4, &s_tts_task) != pdPASS) {
        s_tts_task = NULL;
        muse_voice_request_chirp();
    }
}
#else
static void start_reminder_tts(const char *title)
{
    (void)title;
    muse_voice_request_chirp();
}
#endif

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    set_text_cjk(l, text ? text : "");
    return l;
}

static void format_due(const muse_todo_item_t *it, char *buf, size_t cap, uint32_t *color)
{
    *color = COLOR_DIM;
    if (it->done) {
        snprintf(buf, cap, "已完成");
        return;
    }
    if (it->due <= 0) {
        snprintf(buf, cap, "无截止时间");
        return;
    }
    time_t now = time(NULL);
    time_t due = (time_t)it->due;
    struct tm tm_due;
    localtime_r(&due, &tm_due);
    char when[32];
    strftime(when, sizeof(when), "%m-%d %H:%M", &tm_due);
    double diff = difftime(due, now);
    if (diff < 0) {
        snprintf(buf, cap, "%s - 到期", when);
        *color = COLOR_DANGER;
    } else if (diff < 3600) {
        snprintf(buf, cap, "%s - 即将到期", when);
        *color = COLOR_WARN;
    } else {
        snprintf(buf, cap, "%s", when);
    }
}

static void on_row_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_count) {
        return;
    }
    lock();
    s_items[idx].done = !s_items[idx].done;
    if (s_items[idx].done) {
        s_items[idx].reminded = true;
    }
    persist();
    unlock();
    s_dirty_ui = true;
    muse_todo_tick(0); /* rebuild immediately */
}

static void rebuild_list(void);

static void hide_popup(void)
{
    if (s_popup) {
        lv_obj_add_flag(s_popup, LV_OBJ_FLAG_HIDDEN);
    }
    s_remind_idx = -1;
}

static void on_popup_done(lv_event_t *e)
{
    (void)e;
    muse_todo_reminder_dismiss(true);
}

static void on_popup_later(lv_event_t *e)
{
    (void)e;
    muse_todo_reminder_dismiss(false);
}

void muse_todo_reminder_dismiss(bool complete)
{
    if (s_remind_idx >= 0 && s_remind_idx < s_count) {
        lock();
        if (complete) {
            s_items[s_remind_idx].done = true;
        }
        s_items[s_remind_idx].reminded = true;
        /* snooze 10 min if later */
        if (!complete && s_items[s_remind_idx].due > 0) {
            s_items[s_remind_idx].due = (int64_t)time(NULL) + 600;
            s_items[s_remind_idx].reminded = false;
        }
        persist();
        unlock();
    }
    hide_popup();
    s_dirty_ui = true;
    rebuild_list();
}

static void show_reminder(int idx)
{
    if (idx < 0 || idx >= s_count || !s_popup) {
        return;
    }
    s_remind_idx = idx;
    set_text_cjk(s_popup_title, s_items[idx].title);
    char body[64];
    uint32_t c;
    format_due(&s_items[idx], body, sizeof(body), &c);
    set_text_cjk(s_popup_body, body);
    lv_obj_remove_flag(s_popup, LV_OBJ_FLAG_HIDDEN);
    muse_state_poke();
    start_reminder_tts(s_items[idx].title);
}

static int open_count(void)
{
    int n = 0;
    for (int i = 0; i < s_count; i++) {
        if (!s_items[i].done) {
            n++;
        }
    }
    return n;
}

static void rebuild_list(void)
{
    if (!s_root) {
        return;
    }
    if (s_list) {
        lv_obj_clean(s_list);
    }
    char sub[48];
    int nopen = open_count();
    snprintf(sub, sizeof(sub), "%d 项待办", nopen);
    if (s_sub) {
        set_text_cjk(s_sub, sub);
    }

    if (s_count == 0) {
        lv_obj_t *empty = label(s_list, todo_font(), COLOR_DIM, "暂无待办");
        lv_obj_set_width(empty, lv_pct(100));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(empty, 24, 0);
        return;
    }

    for (int i = 0; i < s_count; i++) {
        const muse_todo_item_t *it = &s_items[i];
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 52);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_ROW), 0);
        lv_obj_set_style_bg_opa(row, it->done ? LV_OPA_50 : LV_OPA_COVER, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_row_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *check = lv_obj_create(row);
        lv_obj_remove_style_all(check);
        lv_obj_set_size(check, 18, 18);
        lv_obj_set_style_radius(check, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(check, 2, 0);
        lv_obj_set_style_border_color(check, lv_color_hex(it->done ? COLOR_OK : COLOR_ACCENT), 0);
        if (it->done) {
            lv_obj_set_style_bg_color(check, lv_color_hex(COLOR_OK), 0);
            lv_obj_set_style_bg_opa(check, LV_OPA_COVER, 0);
        }
        lv_obj_align(check, LV_ALIGN_LEFT_MID, 0, 0);
        if (it->done) {
            lv_obj_t *tick = label(check, &lv_font_montserrat_14, 0x0b0b12, LV_SYMBOL_OK);
            lv_obj_center(tick);
        }

        lv_obj_t *body = lv_obj_create(row);
        lv_obj_remove_style_all(body);
        lv_obj_set_size(body, s_w - 56 - 48, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(body, 2, 0);
        lv_obj_align(body, LV_ALIGN_LEFT_MID, 28, 0);

        lv_obj_t *title = label(body, todo_font(), it->done ? COLOR_DIM : COLOR_TEXT, it->title);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title, s_w - 56 - 48);

        char meta[48];
        uint32_t mc;
        format_due(it, meta, sizeof(meta), &mc);
        label(body, todo_font_sm(), mc, meta);
    }
}

static void build_popup(lv_obj_t *parent)
{
    s_popup = lv_obj_create(parent);
    lv_obj_remove_style_all(s_popup);
    lv_obj_set_size(s_popup, s_w, s_h);
    lv_obj_set_style_bg_color(s_popup, lv_color_hex(0x0b0b12), 0);
    lv_obj_set_style_bg_opa(s_popup, LV_OPA_70, 0);
    lv_obj_add_flag(s_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = lv_obj_create(s_popup);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 260, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x16122a), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_radius(box, 22, 0);
    lv_obj_set_style_pad_all(box, 16, 0);
    lv_obj_center(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 8, 0);

    lv_obj_t *badge = lv_obj_create(box);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(badge, lv_color_hex(COLOR_WARN), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(badge, 8, 0);
    lv_obj_set_style_pad_hor(badge, 10, 0);
    lv_obj_set_style_pad_ver(badge, 3, 0);
    label(badge, todo_font_sm(), 0x1a1000, "REMINDER");

    s_popup_title = label(box, todo_font(), COLOR_TEXT, "");
    lv_obj_set_style_text_align(s_popup_title, LV_TEXT_ALIGN_CENTER, 0);
    s_popup_body = label(box, todo_font_sm(), COLOR_CAPTION, "");

    lv_obj_t *tts = label(box, todo_font_sm(), COLOR_DIM, "蜂鸣 + TTS 播报");
    lv_obj_set_style_text_align(tts, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *actions = lv_obj_create(box);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(actions, 8, 0);

    lv_obj_t *done = lv_obj_create(actions);
    lv_obj_remove_style_all(done);
    lv_obj_set_style_bg_color(done, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(done, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(done, 20, 0);
    lv_obj_set_style_pad_hor(done, 16, 0);
    lv_obj_set_style_pad_ver(done, 7, 0);
    lv_obj_add_flag(done, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(done, on_popup_done, LV_EVENT_CLICKED, NULL);
    label(done, todo_font_sm(), 0xffffff, "完成");

    lv_obj_t *later = lv_obj_create(actions);
    lv_obj_remove_style_all(later);
    lv_obj_set_style_bg_color(later, lv_color_hex(COLOR_METER), 0);
    lv_obj_set_style_bg_opa(later, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(later, 20, 0);
    lv_obj_set_style_pad_hor(later, 16, 0);
    lv_obj_set_style_pad_ver(later, 7, 0);
    lv_obj_add_flag(later, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(later, on_popup_later, LV_EVENT_CLICKED, NULL);
    label(later, todo_font_sm(), COLOR_CAPTION, "稍后");
}

void muse_todo_build(lv_obj_t *tile, int w, int h)
{
    s_w = w;
    s_h = h;
#ifdef ESP_PLATFORM
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
    }
#endif
    load_list();

    s_root = lv_obj_create(tile);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, w, h);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = label(s_root, &lv_font_montserrat_20, COLOR_TEXT, "TODO");
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    s_sub = label(s_root, todo_font_sm(), COLOR_DIM, "0 项待办");
    lv_obj_align(s_sub, LV_ALIGN_TOP_MID, 0, 66);

    s_list = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, w - 96, h - 140);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    build_popup(s_root);
    rebuild_list();

#ifdef ESP_PLATFORM
    fetch_kick();
#endif
    s_next_poll = 0;
}

void muse_todo_show(void)
{
    muse_ui_show_todos();
    if (!s_loaded) {
        load_list();
    }
    rebuild_list();
}

static void check_due(void)
{
    if (s_remind_idx >= 0) {
        return; /* already showing one */
    }
    time_t now = time(NULL);
    if (now < 1700000000) {
        return; /* clock not set yet */
    }
    for (int i = 0; i < s_count; i++) {
        if (s_items[i].done || s_items[i].reminded || s_items[i].due <= 0) {
            continue;
        }
        if ((time_t)s_items[i].due <= now) {
            show_reminder(i);
            return;
        }
    }
}

void muse_todo_tick(float now)
{
    if (!s_root) {
        return;
    }
#ifdef ESP_PLATFORM
    adopt_remote_if_needed();
#endif
    if (s_dirty_ui) {
        s_dirty_ui = false;
        rebuild_list();
    }
    check_due();

    if (s_next_poll == 0) {
        s_next_poll = now + TODO_POLL_S;
        return;
    }
    if (now >= s_next_poll) {
        s_next_poll = now + TODO_POLL_S;
#ifdef ESP_PLATFORM
        if (strcmp(s_src, "nvs") != 0) {
            fetch_kick();
        }
#endif
    }
}
