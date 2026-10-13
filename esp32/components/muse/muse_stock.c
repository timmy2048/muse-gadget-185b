/*
 * Stock watchlist app (preview). See muse_stock.h for the data-source contract:
 * NVS > remote watchlist.json > built-in default; in-app manager; DEMO quotes.
 *
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0.
 */

#include "muse_stock.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "muse_ui.h"

#ifdef ESP_PLATFORM
#include "esp_http_client.h"
#include "nvs.h"
#include "nvs_flash.h"
#endif

static const char *TAG = "muse_stock";

/* Remote watchlist: JSON array of symbol strings, e.g. ["META","SPCX"]. */
#define STOCK_WATCHLIST_URL \
    "https://raw.githubusercontent.com/timmy2048/muse-gadget-185b/main/watchlist.json"
#define STOCK_SYMBOL_LEN 16
#define STOCK_WATCHLIST_MAX 8
#define STOCK_POLL_S 60.0f /* quote + watchlist poll interval during trading hours */

/* Built-in fallback when NVS and remote both fail. */
static const char *const s_default_watchlist[] = { "META", "SPCX" };

/* China convention: red = up, green = down. */
#define COLOR_BG 0x0b0b12
#define COLOR_TEXT 0xf2efff
#define COLOR_DIM 0x8b84a8
#define COLOR_UP 0xff5c5c
#define COLOR_DOWN 0x35c26e
#define COLOR_ROW_SEL 0x1e1b33
#define COLOR_DEMO 0xffb020
#define COLOR_ACCENT 0xa77dff
#define COLOR_DANGER 0xff5c5c

typedef struct {
    char symbol[STOCK_SYMBOL_LEN];
    double price;      /* DEMO */
    double change_pct; /* DEMO */
    double open;       /* DEMO */
    double prev_close; /* DEMO */
} stock_quote_t;

typedef enum {
    SV_QUOTES, /* price list */
    SV_MANAGE, /* edit the watchlist */
    SV_ADD,    /* text entry for a new symbol */
} stock_view_t;

static stock_quote_t s_quotes[STOCK_WATCHLIST_MAX];
static int s_count;
static const char *s_src_name = "default"; /* nvs | remote | default */
static stock_view_t s_view = SV_QUOTES;
static int s_selected = -1;
static float s_next_poll;
static bool s_was_trading;
static uint32_t s_remote_hash;  /* FNV-1a of the last seen remote body */
static bool s_have_remote_hash;
/* Test/sim hook: <0 = automatic trading-hours detection, 0 = forced off,
 * >0 = forced on. */
static int s_trading_override = -1;

static void show_view(stock_view_t v);

static lv_obj_t *s_root;
static lv_obj_t *s_quote_list; /* scrollable quote rows */
static int s_w, s_h;

/* Add-view text entry. */
static char s_input[STOCK_SYMBOL_LEN];
static lv_obj_t *s_input_lbl;

/* ------------------------------------------------------------------ */
/* Watchlist persistence: NVS with a simulator file stand-in.          */
/* Device NVS location: namespace "stock", key "watchlist" (JSON).     */
/* ------------------------------------------------------------------ */

#ifdef ESP_PLATFORM
static int nvs_load(char *buf, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open("stock", NVS_READONLY, &h) != ESP_OK) {
        return -1;
    }
    size_t len = cap;
    esp_err_t e = nvs_get_str(h, "watchlist", buf, &len);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}

static int nvs_save(const char *json)
{
    nvs_handle_t h;
    if (nvs_open("stock", NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }
    esp_err_t e = nvs_set_str(h, "watchlist", json);
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}
#else
/*
 * Simulator stand-in for NVS: ./nvs_stock_watchlist.json in the working
 * directory. Same load/save path as the device; only the backend differs.
 */
static int nvs_load(char *buf, size_t cap)
{
    FILE *f = fopen("nvs_stock_watchlist.json", "r");
    if (!f) {
        return -1;
    }
    size_t len = fread(buf, 1, cap - 1, f);
    fclose(f);
    if (len == 0) {
        return -1;
    }
    buf[len] = '\0';
    return 0;
}

static int nvs_save(const char *json)
{
    FILE *f = fopen("nvs_stock_watchlist.json", "w");
    if (!f) {
        return -1;
    }
    size_t len = strlen(json);
    bool ok = fwrite(json, 1, len, f) == len && fclose(f) == 0;
    return ok ? 0 : -1;
}
#endif

/* ------------------------------------------------------------------ */
/* Remote fetch.                                                       */
/* ------------------------------------------------------------------ */

#ifdef ESP_PLATFORM
/* On device: HTTPS GET of the watchlist URL. */
static int fetch_remote(char *buf, size_t cap)
{
    esp_http_client_config_t cfg = {
        .url = STOCK_WATCHLIST_URL,
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return -1;
    }
    int rc = -1;
    size_t len = 0;
    if (esp_http_client_open(client, 0) == ESP_OK &&
        esp_http_client_fetch_headers(client) >= 0) {
        int n;
        while (len + 1 < cap &&
               (n = esp_http_client_read(client, buf + len, cap - 1 - len)) > 0) {
            len += (size_t)n;
        }
        if (len > 0) {
            buf[len] = '\0';
            rc = 0;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return rc;
}
#else
/*
 * Simulator stand-in for the HTTPS GET: reads ./watchlist.json from the
 * working directory. The fetch -> parse -> fallback path below is identical
 * to the device's; only the transport differs.
 */
static int fetch_remote(char *buf, size_t cap)
{
    FILE *f = fopen("watchlist.json", "r");
    if (!f) {
        return -1;
    }
    size_t len = fread(buf, 1, cap - 1, f);
    fclose(f);
    if (len == 0) {
        return -1;
    }
    buf[len] = '\0';
    return 0;
}
#endif

/* Minimal parser for a JSON array of strings: ["META","SPCX"]. Returns the
 * symbol count, or -1 on malformed input. */
static int parse_watchlist_json(const char *json, char out[][STOCK_SYMBOL_LEN], int max)
{
    const char *p = json;
    int n = 0;
    while (isspace((unsigned char)*p)) p++;
    if (*p++ != '[') {
        return -1;
    }
    for (;;) {
        while (isspace((unsigned char)*p)) p++;
        if (*p == ']') {
            return n;
        }
        if (n > 0) {
            if (*p++ != ',') {
                return -1;
            }
            while (isspace((unsigned char)*p)) p++;
        }
        if (*p++ != '"') {
            return -1;
        }
        if (n >= max) {
            return -1;
        }
        int len = 0;
        while (*p && *p != '"') {
            char c = *p++;
            if (c == '\\' && *p) {
                c = *p++;
            }
            if (len + 1 < STOCK_SYMBOL_LEN) {
                out[n][len++] = c;
            }
        }
        if (*p++ != '"') {
            return -1;
        }
        out[n][len] = '\0';
        if (len == 0) {
            return -1;
        }
        n++;
    }
}

static void watchlist_to_json(char symbols[][STOCK_SYMBOL_LEN], int n, char *out, size_t cap)
{
    size_t len = 0;
    len += snprintf(out + len, cap - len, "[");
    for (int i = 0; i < n && len < cap; i++) {
        len += snprintf(out + len, cap - len, "%s\"%s\"", i ? "," : "", symbols[i]);
    }
    snprintf(out + len, cap - len, "]");
}

/* DEMO quotes. Real feed plugs in here later. */
static void demo_quote(const char *symbol, stock_quote_t *q)
{
    snprintf(q->symbol, sizeof(q->symbol), "%.*s",
             (int)(sizeof(q->symbol) - 1), symbol);
    if (!strcmp(symbol, "META")) {
        q->price = 742.85;
        q->change_pct = 1.24;
        q->open = 738.10;
        q->prev_close = 733.72;
    } else if (!strcmp(symbol, "SPCX")) {
        q->price = 38.42;
        q->change_pct = -2.15;
        q->open = 39.31;
        q->prev_close = 39.26;
    } else {
        /* Deterministic pseudo-quote so any symbol still renders. */
        unsigned h = 0;
        for (const char *p = symbol; *p; p++) {
            h = h * 31 + (unsigned char)*p;
        }
        q->price = 20.0 + (h % 8000) / 100.0;
        q->change_pct = ((int)(h % 401) - 200) / 100.0;
        q->open = q->price / (1.0 + q->change_pct / 100.0);
        q->prev_close = q->open;
    }
}

static uint32_t fnv1a(const char *s, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

/* US market 9:30-16:00 ET == 13:30-20:00 UTC (EST) / 14:30-21:00 UTC (EDT).
 * Preview simplification: 13:30-21:00 UTC covers both; weekends excluded. */
static bool trading_hours_now(void)
{
    if (s_trading_override >= 0) {
        return s_trading_override > 0;
    }
    time_t t = time(NULL);
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    if (tm_utc.tm_wday == 0 || tm_utc.tm_wday == 6) {
        return false;
    }
    int mins = tm_utc.tm_hour * 60 + tm_utc.tm_min;
    return mins >= 13 * 60 + 30 && mins < 21 * 60;
}

/* Priority: NVS local > remote GitHub > built-in default. */
static void load_watchlist(void)
{
    char symbols[STOCK_WATCHLIST_MAX][STOCK_SYMBOL_LEN];
    char body[1024];
    int n = -1;

    if (nvs_load(body, sizeof(body)) == 0) {
        n = parse_watchlist_json(body, symbols, STOCK_WATCHLIST_MAX);
        if (n > 0) {
            s_src_name = "nvs";
            ESP_LOGI(TAG, "watchlist: %d symbols from NVS", n);
        }
    }
    if (n <= 0 && fetch_remote(body, sizeof(body)) == 0) {
        n = parse_watchlist_json(body, symbols, STOCK_WATCHLIST_MAX);
        if (n > 0) {
            s_src_name = "remote";
            s_remote_hash = fnv1a(body, strlen(body));
            s_have_remote_hash = true;
            ESP_LOGI(TAG, "watchlist: %d symbols from remote", n);
        }
    }
    if (n <= 0) {
        s_src_name = "default";
        ESP_LOGI(TAG, "watchlist: NVS and remote failed, using built-in default");
        n = 0;
        for (size_t i = 0; i < sizeof(s_default_watchlist) / sizeof(s_default_watchlist[0]); i++) {
            snprintf(symbols[n++], STOCK_SYMBOL_LEN, "%.*s",
                     STOCK_SYMBOL_LEN - 1, s_default_watchlist[i]);
        }
    }

    s_count = n;
    for (int i = 0; i < n; i++) {
        demo_quote(symbols[i], &s_quotes[i]);
    }
    if (s_selected >= s_count) {
        s_selected = -1;
    }
}

static void persist_watchlist(void)
{
    char symbols[STOCK_WATCHLIST_MAX][STOCK_SYMBOL_LEN];
    char json[512];
    for (int i = 0; i < s_count; i++) {
        snprintf(symbols[i], sizeof(symbols[i]), "%.*s",
                 (int)(sizeof(symbols[i]) - 1), s_quotes[i].symbol);
    }
    watchlist_to_json(symbols, s_count, json, sizeof(json));
    if (nvs_save(json) == 0) {
        ESP_LOGI(TAG, "watchlist saved to NVS: %s", json);
    } else {
        ESP_LOGW(TAG, "watchlist NVS save failed");
    }
}

/* Each quote poll re-fetches the remote watchlist and compares its content
 * hash. A changed list is adopted immediately and the UI rebuilt, no
 * restart needed. Skipped while the list comes from NVS: local edits win
 * over the remote file. */
static bool remote_watchlist_check(void)
{
    char body[1024];
    if (fetch_remote(body, sizeof(body)) != 0) {
        return false;
    }
    uint32_t h = fnv1a(body, strlen(body));
    if (s_have_remote_hash && h == s_remote_hash) {
        return false; /* unchanged */
    }
    char symbols[STOCK_WATCHLIST_MAX][STOCK_SYMBOL_LEN];
    int n = parse_watchlist_json(body, symbols, STOCK_WATCHLIST_MAX);
    if (n <= 0) {
        ESP_LOGW(TAG, "remote watchlist changed but unparseable, keeping current");
        return false;
    }
    bool same = s_have_remote_hash && n == s_count;
    for (int i = 0; same && i < n; i++) {
        same = !strcmp(s_quotes[i].symbol, symbols[i]);
    }
    s_remote_hash = h;
    s_have_remote_hash = true;
    if (same) {
        return false;
    }
    ESP_LOGI(TAG, "remote watchlist changed: %d -> %d symbols, updating live", s_count, n);
    s_count = n;
    for (int i = 0; i < n; i++) {
        demo_quote(symbols[i], &s_quotes[i]);
    }
    if (s_selected >= s_count) {
        s_selected = -1;
    }
    s_src_name = "remote";
    if (s_root && (s_view == SV_QUOTES || s_view == SV_MANAGE)) {
        show_view(s_view);
    }
    return true;
}

/* DEMO quote refresh: tiny random walk so polling is visible. The real feed
 * replaces the body of this function. */
static void quote_poll(void)
{
    for (int i = 0; i < s_count; i++) {
        stock_quote_t *q = &s_quotes[i];
        double drift = ((rand() % 41) - 20) / 10000.0; /* +/-0.2% */
        q->price *= (1.0 + drift);
        if (q->prev_close > 0) {
            q->change_pct = (q->price - q->prev_close) / q->prev_close * 100.0;
        }
    }
    ESP_LOGI(TAG, "quote poll: %d symbols (DEMO)", s_count);
    if (s_root && s_view == SV_QUOTES) {
        show_view(SV_QUOTES);
    }
}

/* ------------------------------------------------------------------ */
/* Watchlist editing.                                                  */
/* ------------------------------------------------------------------ */

static bool symbol_valid(const char *s)
{
    size_t len = strlen(s);
    if (len == 0 || len >= STOCK_SYMBOL_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.')) {
            return false;
        }
    }
    return true;
}

static void normalize_symbol(char *dst, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < STOCK_SYMBOL_LEN; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') {
            c -= 'a' - 'A';
        }
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.') {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

static void delete_symbol(int idx)
{
    if (idx < 0 || idx >= s_count) {
        return;
    }
    ESP_LOGI(TAG, "delete %s", s_quotes[idx].symbol);
    for (int i = idx; i + 1 < s_count; i++) {
        s_quotes[i] = s_quotes[i + 1];
    }
    s_count--;
    if (s_selected >= s_count) {
        s_selected = -1;
    }
    s_src_name = "nvs";
    persist_watchlist();
}

static bool add_symbol(const char *raw)
{
    char sym[STOCK_SYMBOL_LEN];
    normalize_symbol(sym, raw);
    if (!symbol_valid(sym)) {
        ESP_LOGW(TAG, "reject invalid symbol '%s'", raw);
        return false;
    }
    for (int i = 0; i < s_count; i++) {
        if (!strcmp(s_quotes[i].symbol, sym)) {
            ESP_LOGW(TAG, "duplicate symbol '%s'", sym);
            return false;
        }
    }
    if (s_count >= STOCK_WATCHLIST_MAX) {
        ESP_LOGW(TAG, "watchlist full");
        return false;
    }
    ESP_LOGI(TAG, "add %s", sym);
    demo_quote(sym, &s_quotes[s_count++]);
    s_src_name = "nvs";
    persist_watchlist();
    return true;
}

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *pill_button(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_30, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_set_style_pad_ver(b, 6, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *t = label(b, &lv_font_unscii_16, color, text);
    lv_obj_center(t);
    return b;
}

static void on_row_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    muse_stock_select(s_selected == idx ? -1 : idx);
}

static void on_manage_btn(lv_event_t *e)
{
    (void)e;
    muse_stock_manage();
}

static void on_done_btn(lv_event_t *e)
{
    (void)e;
    show_view(SV_QUOTES);
}

static void on_add_btn(lv_event_t *e)
{
    (void)e;
    show_view(SV_ADD);
}

static void on_delete_btn(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    delete_symbol(idx);
    show_view(SV_MANAGE);
}

static void on_add_confirm_btn(lv_event_t *e)
{
    (void)e;
    muse_stock_text_done();
}

static void on_add_cancel_btn(lv_event_t *e)
{
    (void)e;
    muse_stock_text_cancel();
}

/* --- quotes view --- */

static void build_quotes(void)
{
    char buf[64];

    lv_obj_t *title = label(s_root, &lv_font_montserrat_20, COLOR_TEXT, "STOCKS");
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    /* Subtitle: DEMO badge + watchlist source, centred under the title. */
    lv_obj_t *sub = lv_obj_create(s_root);
    lv_obj_remove_style_all(sub);
    lv_obj_set_size(sub, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sub, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sub, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(sub, 8, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_t *badge = lv_obj_create(sub);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(badge, lv_color_hex(COLOR_DEMO), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_30, 0);
    lv_obj_set_style_radius(badge, 8, 0);
    lv_obj_set_style_pad_hor(badge, 8, 0);
    lv_obj_set_style_pad_ver(badge, 3, 0);
    lv_obj_t *bt = label(badge, &lv_font_unscii_8, COLOR_DEMO, "DEMO");
    lv_obj_center(bt);
    snprintf(buf, sizeof(buf), "watchlist: %s", s_src_name);
    label(sub, &lv_font_unscii_8, COLOR_DIM, buf);

    lv_obj_t *list = lv_obj_create(s_root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, s_w - 56, s_h - 200);
    lv_obj_align(list, LV_ALIGN_CENTER, 0, 18);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    s_quote_list = list;

    for (int i = 0; i < s_count; i++) {
        const stock_quote_t *q = &s_quotes[i];
        bool up = q->change_pct >= 0;
        bool sel = i == s_selected;

        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 84);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_pad_hor(row, 14, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(sel ? COLOR_ROW_SEL : COLOR_BG), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *sym = label(row, &lv_font_montserrat_28, COLOR_TEXT, q->symbol);
        lv_obj_align(sym, LV_ALIGN_LEFT_MID, 0, sel ? -10 : 0);

        lv_obj_t *right = lv_obj_create(row);
        lv_obj_remove_style_all(right);
        lv_obj_set_size(right, 132, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_row(right, 3, 0);
        lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
        snprintf(buf, sizeof(buf), "%.2f", q->price);
        label(right, &lv_font_montserrat_20, COLOR_TEXT, buf);
        lv_obj_t *pill = lv_obj_create(right);
        lv_obj_remove_style_all(pill);
        lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(pill, 10, 0);
        lv_obj_set_style_bg_color(pill, lv_color_hex(up ? COLOR_UP : COLOR_DOWN), 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(pill, 10, 0);
        lv_obj_set_style_pad_ver(pill, 4, 0);
        snprintf(buf, sizeof(buf), "%s%.2f%%", up ? "+" : "", q->change_pct);
        lv_obj_t *pct = label(pill, &lv_font_montserrat_14, 0xffffff, buf);
        lv_obj_center(pct);

        if (sel) {
            snprintf(buf, sizeof(buf), "Open %.2f   Prev %.2f", q->open, q->prev_close);
            lv_obj_t *detail = label(row, &lv_font_unscii_8, COLOR_DIM, buf);
            lv_obj_align(detail, LV_ALIGN_BOTTOM_LEFT, 0, -6);
        }
    }

    lv_obj_t *manage = pill_button(s_root, "Manage", COLOR_ACCENT);
    lv_obj_add_event_cb(manage, on_manage_btn, LV_EVENT_CLICKED, NULL);
    lv_obj_align(manage, LV_ALIGN_BOTTOM_MID, 0, -44);
}

/* --- manage view --- */

static void build_manage(void)
{
    char buf[64];

    lv_obj_t *title = label(s_root, &lv_font_montserrat_20, COLOR_TEXT, "MANAGE");
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

    snprintf(buf, sizeof(buf), "%d/%d", s_count, STOCK_WATCHLIST_MAX);
    lv_obj_t *cnt = label(s_root, &lv_font_unscii_8, COLOR_DIM, buf);
    lv_obj_align(cnt, LV_ALIGN_TOP_RIGHT, -34, 30);

    lv_obj_t *list = lv_obj_create(s_root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, s_w - 56, s_h - 190);
    lv_obj_align(list, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < s_count; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 52);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_hor(row, 14, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_ROW_SEL), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);

        lv_obj_t *sym = label(row, &lv_font_montserrat_20, COLOR_TEXT, s_quotes[i].symbol);
        lv_obj_align(sym, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *del = lv_obj_create(row);
        lv_obj_remove_style_all(del);
        lv_obj_set_size(del, 34, 34);
        lv_obj_set_style_radius(del, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(del, lv_color_hex(COLOR_DANGER), 0);
        lv_obj_set_style_bg_opa(del, LV_OPA_30, 0);
        lv_obj_align(del, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_flag(del, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(del, on_delete_btn, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *minus = label(del, &lv_font_montserrat_20, COLOR_DANGER, "-");
        lv_obj_center(minus);
    }

    lv_obj_t *add = pill_button(s_root, "+ Add symbol", COLOR_ACCENT);
    lv_obj_add_event_cb(add, on_add_btn, LV_EVENT_CLICKED, NULL);
    lv_obj_align(add, LV_ALIGN_BOTTOM_MID, 0, -64);

    lv_obj_t *done = pill_button(s_root, "Done", COLOR_TEXT);
    lv_obj_add_event_cb(done, on_done_btn, LV_EVENT_CLICKED, NULL);
    lv_obj_align(done, LV_ALIGN_BOTTOM_MID, 0, -24);
}

/* --- add view --- */

static void build_add(void)
{
    lv_obj_t *title = label(s_root, &lv_font_montserrat_20, COLOR_TEXT, "ADD SYMBOL");
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

    lv_obj_t *box = lv_obj_create(s_root);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, s_w - 96, 64);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, -30);

    s_input_lbl = label(box, &lv_font_montserrat_28, COLOR_TEXT, "_");
    lv_obj_center(s_input_lbl);

    lv_obj_t *hint = label(s_root, &lv_font_unscii_8, COLOR_DIM, "Type A-Z 0-9");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 36);

    /* On-screen confirm/cancel for the touch panel (keyboard Enter/Esc
     * also work in the simulator). */
    lv_obj_t *bar = lv_obj_create(s_root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 12, 0);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -44);
    lv_obj_t *ok = pill_button(bar, "Add", COLOR_ACCENT);
    lv_obj_add_event_cb(ok, on_add_confirm_btn, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel = pill_button(bar, "Cancel", COLOR_DIM);
    lv_obj_add_event_cb(cancel, on_add_cancel_btn, LV_EVENT_CLICKED, NULL);
}

static void show_view(stock_view_t v)
{
    s_view = v;
    lv_obj_clean(s_root);
    s_input_lbl = NULL;
    switch (v) {
    case SV_MANAGE:
        build_manage();
        break;
    case SV_ADD:
        s_input[0] = '\0';
        build_add();
        break;
    default:
        build_quotes();
        break;
    }
}

static void refresh_input_label(void)
{
    if (!s_input_lbl) {
        return;
    }
    char buf[STOCK_SYMBOL_LEN + 2];
    snprintf(buf, sizeof(buf), "%s_", s_input);
    lv_label_set_text(s_input_lbl, buf);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void muse_stock_build(lv_obj_t *tile, int w, int h)
{
    s_w = w;
    s_h = h;
    load_watchlist();

    s_root = lv_obj_create(tile);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, w, h);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    show_view(SV_QUOTES);
    s_next_poll = 0; /* tick() schedules the first poll */
    s_was_trading = trading_hours_now();
}

void muse_stock_show(void)
{
    muse_ui_show_stocks();
    /* Refresh the list each time the tile is opened (picks up NVS edits). */
    load_watchlist();
    show_view(SV_QUOTES);
}

void muse_stock_select(int idx)
{
    if (idx < -1 || idx >= s_count) {
        return;
    }
    s_selected = idx;
    if (s_view == SV_QUOTES) {
        show_view(SV_QUOTES);
    }
}

void muse_stock_manage(void)
{
    show_view(SV_MANAGE);
}

void muse_stock_add_begin(void)
{
    show_view(SV_ADD);
}

void muse_stock_delete(int idx)
{
    delete_symbol(idx);
    show_view(s_view);
}

void muse_stock_tick(float now)
{
    if (!s_root) {
        return;
    }
    bool trading = trading_hours_now();
    if (trading != s_was_trading) {
        s_was_trading = trading;
        if (trading) {
            ESP_LOGI(TAG, "trading hours: polling every %.0fs", (double)STOCK_POLL_S);
        } else {
            ESP_LOGI(TAG, "outside trading hours: polling paused");
        }
    }
    if (!trading) {
        return; /* save power outside trading hours */
    }
    if (s_next_poll == 0) {
        s_next_poll = now + STOCK_POLL_S;
        return;
    }
    if (now >= s_next_poll) {
        s_next_poll = now + STOCK_POLL_S;
        /* A locally managed (NVS) list is never overridden by the remote. */
        if (strcmp(s_src_name, "nvs") != 0) {
            remote_watchlist_check();
        }
        quote_poll();
    }
}

/* Test/sim hook: force the trading-hours state (<0 auto, 0 off, >0 on). */
void muse_stock_set_trading_override(int v)
{
    s_trading_override = v;
}

/* Test/sim hook: scroll the quote list so row idx is at the top. */
void muse_stock_scroll_to(int idx)
{
    if (!s_quote_list || idx < 0 || idx >= s_count) {
        return;
    }
    lv_obj_scroll_to_y(s_quote_list, idx * (84 + 10), LV_ANIM_OFF);
}

bool muse_stock_text_active(void)
{
    return s_view == SV_ADD && s_root != NULL;
}

void muse_stock_text_char(char c)
{
    size_t len = strlen(s_input);
    if (len + 1 < STOCK_SYMBOL_LEN) {
        s_input[len] = c;
        s_input[len + 1] = '\0';
        refresh_input_label();
    }
}

void muse_stock_text_backspace(void)
{
    size_t len = strlen(s_input);
    if (len > 0) {
        s_input[len - 1] = '\0';
        refresh_input_label();
    }
}

void muse_stock_text_done(void)
{
    if (add_symbol(s_input)) {
        show_view(SV_MANAGE);
    } else {
        /* Invalid: shake the box by flashing it dim red once. */
        refresh_input_label();
    }
}

void muse_stock_text_cancel(void)
{
    show_view(SV_MANAGE);
}
