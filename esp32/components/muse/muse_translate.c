/*
 * Translate page: hold-BOOT STT → auto ZH⇄EN → TTS.
 * Idle: title only (no tip/coaching card). Listening + result states match mock.
 */
#include "muse_translate.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "muse_audio.h"
#include "muse_state.h"
#include "muse_text.h"
#include "muse_ui.h"
#include "muse_voice.h"
#include "sdkconfig.h"

#ifdef ESP_PLATFORM
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "minimp3.h"
#endif

#if CONFIG_MUSE_STT_HTTP
#include "muse_stt.h"
#endif

static const char *TAG = "muse_xlate";

#define COLOR_BG 0x0b0b12
#define COLOR_TEXT 0xf2efff
#define COLOR_DIM 0x8b84a8
#define COLOR_CAPTION 0xd8d2ff
#define COLOR_ACCENT 0xa77dff
#define COLOR_ROW 0x1e1b33
#define COLOR_METER 0x1d1733
#define COLOR_EN 0x6ec8ff

#define TEXT_CAP 384
#define TRANSLATE_URL "http://10.0.0.1:8787/translate?q="
#define GTX_URL_PREFIX "https://translate.googleapis.com/translate_a/single?client=gtx&dt=t&"

typedef enum {
    XLATE_IDLE = 0,
    XLATE_LISTEN,
    XLATE_BUSY,
    XLATE_RESULT,
    XLATE_ERROR,
} xlate_state_t;

static lv_obj_t *s_root;
static lv_obj_t *s_title;
static lv_obj_t *s_sub;
static lv_obj_t *s_idle_mark;
static lv_obj_t *s_listen;
static lv_obj_t *s_result;
static lv_obj_t *s_src_lab;
static lv_obj_t *s_src_tag;
static lv_obj_t *s_src_text;
static lv_obj_t *s_tgt_lab;
static lv_obj_t *s_tgt_tag;
static lv_obj_t *s_tgt_text;
static lv_obj_t *s_tts_row;
static lv_obj_t *s_pill;
static lv_obj_t *s_wave[7];
static int s_w, s_h;

static volatile xlate_state_t s_state = XLATE_IDLE;
static char s_src[TEXT_CAP];
static char s_tgt[TEXT_CAP];
static char s_src_lang[8]; /* "zh" | "en" */
static char s_tgt_lang[8];
static char s_status[64];
static bool s_dirty;
static int s_wave_phase;

#ifdef ESP_PLATFORM
static SemaphoreHandle_t s_mtx;
static TaskHandle_t s_job_task;
static int16_t *s_job_pcm;
static size_t s_job_frames;
#endif

#if CONFIG_MUSE_CJK_FONT
LV_FONT_DECLARE(muse_font_cjk_16)
static const lv_font_t *xf_font(void)
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
static const lv_font_t *xf_font_sm(void)
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
#else
static const lv_font_t *xf_font(void) { return &lv_font_montserrat_16; }
static const lv_font_t *xf_font_sm(void) { return &lv_font_montserrat_14; }
#endif

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt ? txt : "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static void set_text(lv_obj_t *l, const char *text)
{
    if (!l || !text) {
        return;
    }
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

static void pill_set(const char *text, uint32_t bg, uint32_t fg)
{
    if (!s_pill) {
        return;
    }
    lv_obj_t *lab = lv_obj_get_child(s_pill, 0);
    if (lab) {
        set_text(lab, text ? text : "");
        lv_obj_set_style_text_color(lab, lv_color_hex(fg), 0);
    }
    lv_obj_set_style_bg_color(s_pill, lv_color_hex(bg), 0);
    if (text && text[0]) {
        lv_obj_remove_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
    }
}

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

static bool text_is_cjk_dominant(const char *s)
{
    if (!s || !s[0]) {
        return false;
    }
    int cjk = 0, latin = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x80) {
            if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
                latin++;
            }
            p++;
            continue;
        }
        /* UTF-8 3-byte CJK ranges (approx) */
        if ((p[0] & 0xf0) == 0xe0 && p[1] && p[2]) {
            unsigned cp = ((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
            if ((cp >= 0x3400 && cp <= 0x9fff) || (cp >= 0xf900 && cp <= 0xfaff)) {
                cjk++;
            }
            p += 3;
            continue;
        }
        if ((*p & 0xe0) == 0xc0 && p[1]) {
            p += 2;
            continue;
        }
        if ((*p & 0xf8) == 0xf0 && p[1] && p[2] && p[3]) {
            p += 4;
            continue;
        }
        p++;
    }
    return cjk >= 1 && cjk >= latin;
}

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

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_buf_t;

static esp_err_t on_http(esp_http_client_event_t *evt)
{
    http_buf_t *r = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->data || evt->data_len <= 0 || !r || !r->buf) {
        return ESP_OK;
    }
    size_t n = (size_t)evt->data_len;
    if (r->len + n >= r->cap) {
        n = r->cap > r->len + 1 ? r->cap - r->len - 1 : 0;
    }
    if (n) {
        memcpy(r->buf + r->len, evt->data, n);
        r->len += n;
        r->buf[r->len] = '\0';
    }
    return ESP_OK;
}

static int http_get_body(const char *url, char *buf, size_t cap, size_t *out_len, bool https)
{
    http_buf_t hb = { .buf = buf, .len = 0, .cap = cap };
    if (cap) {
        buf[0] = '\0';
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .event_handler = on_http,
        .user_data = &hb,
        .disable_auto_redirect = true,
    };
    if (https) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return -1;
    }
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (out_len) {
        *out_len = hb.len;
    }
    if (err != ESP_OK) {
        return -1;
    }
    return status;
}

static int http_get_bin(const char *url, uint8_t *buf, size_t cap, size_t *out_len)
{
    return http_get_body(url, (char *)buf, cap, out_len, strncmp(url, "https", 5) == 0);
}

/* Parse GTX JSON array-of-arrays: [[ ["translated","src",...], ...], ...] */
static bool parse_gtx(const char *json, char *out, size_t out_cap)
{
    if (!json || !out || out_cap < 2) {
        return false;
    }
    out[0] = '\0';
    size_t o = 0;
    const char *p = json;
    /* Walk first inner arrays' first strings */
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    bool take = false;
    int str_idx_in_row = 0;
    int row_depth = -1;
    for (; *p; p++) {
        char c = *p;
        if (in_str) {
            if (esc) {
                esc = false;
                if (take && o + 1 < out_cap) {
                    out[o++] = c;
                }
                continue;
            }
            if (c == '\\') {
                esc = true;
                continue;
            }
            if (c == '"') {
                in_str = false;
                if (take) {
                    take = false;
                    /* only first string of each leaf row */
                }
                continue;
            }
            if (take && o + 1 < out_cap) {
                out[o++] = c;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
            if (depth >= 3 && str_idx_in_row == 0) {
                take = true;
            }
            if (depth >= 3) {
                str_idx_in_row++;
            }
            continue;
        }
        if (c == '[') {
            depth++;
            if (depth == 3) {
                str_idx_in_row = 0;
                row_depth = depth;
            }
            continue;
        }
        if (c == ']') {
            if (depth == row_depth) {
                str_idx_in_row = 0;
            }
            depth--;
            continue;
        }
        if (c == ',' && depth == 2) {
            /* next row */
            str_idx_in_row = 0;
        }
    }
    out[o] = '\0';
    return o > 0;
}

static bool translate_text(const char *src, const char *from_lang, const char *to_lang,
                           char *out, size_t out_cap)
{
    char enc[640];
    urlencode(src, enc, sizeof(enc));

    /* Prefer VPS /translate (Azure if configured, else gtx on VPS). */
    char url[1400];
    snprintf(url, sizeof(url), "%s%s&from=%s&to=%s", TRANSLATE_URL, enc, from_lang, to_lang);
    char *resp = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) {
        resp = malloc(2048);
    }
    if (!resp) {
        return false;
    }
    size_t len = 0;
    int st = http_get_body(url, resp, 2048, &len, false);
    if (st >= 200 && st < 300 && len > 2) {
        /* {"ok":true,"text":"..."} */
        const char *key = strstr(resp, "\"text\"");
        if (key) {
            const char *q1 = strchr(key + 6, '"');
            if (q1) {
                q1++;
                const char *q2 = q1;
                while (*q2 && *q2 != '"') {
                    if (*q2 == '\\' && q2[1]) {
                        q2 += 2;
                        continue;
                    }
                    q2++;
                }
                if (*q2 == '"' && (size_t)(q2 - q1) < out_cap) {
                    size_t n = (size_t)(q2 - q1);
                    /* crude unescape of \/ \" \\ */
                    size_t o = 0;
                    for (size_t i = 0; i < n && o + 1 < out_cap; i++) {
                        if (q1[i] == '\\' && i + 1 < n) {
                            out[o++] = q1[++i];
                        } else {
                            out[o++] = q1[i];
                        }
                    }
                    out[o] = '\0';
                    if (o > 0 && strstr(resp, "\"ok\":true")) {
                        free(resp);
                        return true;
                    }
                }
            }
        }
    }

    /* Fallback: Google gtx over WG. */
    const char *sl = (from_lang[0] == 'z') ? "zh-CN" : "en";
    const char *tl = (to_lang[0] == 'z') ? "zh-CN" : "en";
    snprintf(url, sizeof(url), "%ssl=%s&tl=%s&q=%s", GTX_URL_PREFIX, sl, tl, enc);
    st = http_get_body(url, resp, 2048, &len, true);
    bool ok = false;
    if (st >= 200 && st < 300 && len > 2) {
        ok = parse_gtx(resp, out, out_cap);
    }
    free(resp);
    return ok;
}

static void play_tts(const char *text, const char *lang)
{
    if (!text || !text[0]) {
        return;
    }
    char enc[640];
    urlencode(text, enc, sizeof(enc));
    char url[1400];
    const char *prefix;
    bool en = lang && lang[0] == 'e';
#if defined(CONFIG_MUSE_TTS_URL_EN) && defined(CONFIG_MUSE_TTS_URL_ZH)
    prefix = en ? CONFIG_MUSE_TTS_URL_EN : CONFIG_MUSE_TTS_URL_ZH;
    if (!prefix || !prefix[0]) {
        prefix = "http://10.0.0.1:8787/tts?q=";
    }
#else
    prefix = "http://10.0.0.1:8787/tts?q=";
#endif
    /* Append lang= so updated VPS picks Jenny vs Xiaoxiao. */
    if (snprintf(url, sizeof(url), "%s%s&lang=%s", prefix, enc, en ? "en" : "zh") >= (int)sizeof(url)) {
        return;
    }

    const size_t cap = 96 * 1024;
    uint8_t *mp3 = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mp3) {
        mp3 = malloc(cap);
    }
    if (!mp3) {
        return;
    }
    size_t len = 0;
    int st = http_get_bin(url, mp3, cap, &len);
    if (st < 200 || st >= 300 || len < 64) {
        ESP_LOGW(TAG, "tts fetch fail st=%d len=%u", st, (unsigned)len);
        free(mp3);
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
            int out_n = 0;
            for (int i = 0; i + 2 < nmono; i += 3) {
                mono[out_n++] = mono[i];
                mono[out_n++] = mono[i + 1];
            }
            muse_audio_write(mono, (size_t)out_n);
        } else {
            muse_audio_write(mono, (size_t)nmono);
        }
    }
    free(mp3);
}

#if CONFIG_MUSE_STT_HTTP
static char *stt_auto(const int16_t *pcm, size_t frames, char *lang_out, size_t lang_cap)
{
    char *text = muse_stt_recognize_pcm_auto(pcm, frames, lang_out, lang_cap);
    if (!text) {
        return NULL;
    }
    if (lang_out && lang_cap && !lang_out[0]) {
        snprintf(lang_out, lang_cap, "%s", text_is_cjk_dominant(text) ? "zh" : "en");
    }
    return text;
}
#else
static char *stt_auto(const int16_t *pcm, size_t frames, char *lang_out, size_t lang_cap)
{
    (void)pcm;
    (void)frames;
    if (lang_out && lang_cap) {
        lang_out[0] = '\0';
    }
    return NULL;
}
#endif

static void set_status_locked(xlate_state_t st, const char *msg)
{
    s_state = st;
    if (msg) {
        snprintf(s_status, sizeof(s_status), "%s", msg);
    }
    s_dirty = true;
}

static void job_task(void *arg)
{
    (void)arg;
    int16_t *pcm = s_job_pcm;
    size_t frames = s_job_frames;
    s_job_pcm = NULL;
    s_job_frames = 0;

    lock();
    set_status_locked(XLATE_BUSY, "识别中…");
    unlock();

    char lang[8] = { 0 };
    char *text = stt_auto(pcm, frames, lang, sizeof(lang));
    free(pcm);

    if (!text || !text[0]) {
        lock();
        s_src[0] = s_tgt[0] = '\0';
        set_status_locked(XLATE_ERROR, "未识别到语音");
        unlock();
        s_job_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (!lang[0]) {
        snprintf(lang, sizeof(lang), "%s", text_is_cjk_dominant(text) ? "zh" : "en");
    }
    const char *to = (lang[0] == 'z') ? "en" : "zh";

    lock();
    snprintf(s_src, sizeof(s_src), "%s", text);
    snprintf(s_src_lang, sizeof(s_src_lang), "%s", lang);
    snprintf(s_tgt_lang, sizeof(s_tgt_lang), "%s", to);
    set_status_locked(XLATE_BUSY, lang[0] == 'z' ? "翻译成英文…" : "翻译成中文…");
    unlock();

    char translated[TEXT_CAP];
    translated[0] = '\0';
    bool ok = translate_text(text, lang, to, translated, sizeof(translated));
    free(text);

    if (!ok || !translated[0]) {
        lock();
        s_tgt[0] = '\0';
        set_status_locked(XLATE_ERROR, "翻译失败");
        unlock();
        s_job_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    lock();
    snprintf(s_tgt, sizeof(s_tgt), "%s", translated);
    set_status_locked(XLATE_RESULT, to[0] == 'e' ? "检测到中文 · → 英文" : "检测到英文 · → 中文");
    unlock();

    muse_voice_request_chirp();
    vTaskDelay(pdMS_TO_TICKS(150));
    play_tts(translated, to);

    lock();
    /* keep RESULT; refresh tts row */
    s_dirty = true;
    unlock();

    s_job_task = NULL;
    vTaskDelete(NULL);
}
#endif /* ESP_PLATFORM */

bool muse_translate_page_active(void)
{
    return muse_ui_page_is_translate();
}

void muse_translate_on_listen(void)
{
    lock();
    s_src[0] = s_tgt[0] = '\0';
    set_status_locked(XLATE_LISTEN, "正在聆听 · 自动语种");
    unlock();
    muse_state_poke();
}

bool muse_translate_submit_pcm(const int16_t *pcm, size_t frames)
{
#ifndef ESP_PLATFORM
    (void)pcm;
    (void)frames;
    return false;
#else
    if (!pcm || frames < 3200) { /* <0.2s @16k */
        lock();
        set_status_locked(XLATE_ERROR, "请说久一点");
        unlock();
        return true;
    }
    if (s_job_task) {
        ESP_LOGW(TAG, "translate busy");
        return true;
    }
    size_t bytes = frames * sizeof(int16_t);
    int16_t *copy = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
        copy = malloc(bytes);
    }
    if (!copy) {
        lock();
        set_status_locked(XLATE_ERROR, "内存不足");
        unlock();
        return true;
    }
    memcpy(copy, pcm, bytes);
    s_job_pcm = copy;
    s_job_frames = frames;
    lock();
    set_status_locked(XLATE_BUSY, "识别中…");
    unlock();

    /* Room for minimp3 + HTTP; stack in SPIRAM. */
    BaseType_t ok = xTaskCreateWithCaps(job_task, "muse_xlate", 16384, NULL, 4, &s_job_task,
                                        MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ok = xTaskCreate(job_task, "muse_xlate", 12288, NULL, 4, &s_job_task);
    }
    if (ok != pdPASS) {
        free(copy);
        s_job_pcm = NULL;
        s_job_task = NULL;
        lock();
        set_status_locked(XLATE_ERROR, "任务启动失败");
        unlock();
    }
    return true;
#endif
}

static void show_layer(lv_obj_t *which)
{
    if (s_idle_mark) {
        lv_obj_set_flag(s_idle_mark, LV_OBJ_FLAG_HIDDEN, which != s_idle_mark);
    }
    if (s_listen) {
        lv_obj_set_flag(s_listen, LV_OBJ_FLAG_HIDDEN, which != s_listen);
    }
    if (s_result) {
        lv_obj_set_flag(s_result, LV_OBJ_FLAG_HIDDEN, which != s_result);
    }
}

static void apply_ui(void)
{
    if (!s_root) {
        return;
    }
    xlate_state_t st;
    char src[TEXT_CAP], tgt[TEXT_CAP], status[64], sl[8], tl[8];
    lock();
    st = s_state;
    snprintf(src, sizeof(src), "%s", s_src);
    snprintf(tgt, sizeof(tgt), "%s", s_tgt);
    snprintf(status, sizeof(status), "%s", s_status);
    snprintf(sl, sizeof(sl), "%s", s_src_lang);
    snprintf(tl, sizeof(tl), "%s", s_tgt_lang);
    s_dirty = false;
    unlock();

    if (st == XLATE_LISTEN) {
        show_layer(s_listen);
        set_text(s_sub, status[0] ? status : "正在聆听 · 自动语种");
        pill_set("按住中", COLOR_ACCENT, 0xffffff);
    } else if (st == XLATE_BUSY) {
        show_layer(s_listen);
        set_text(s_sub, status[0] ? status : "处理中…");
        pill_set("处理中", COLOR_METER, COLOR_CAPTION);
    } else if (st == XLATE_RESULT || (st == XLATE_ERROR && src[0])) {
        show_layer(s_result);
        set_text(s_sub, status[0] ? status : "");
        bool src_zh = sl[0] == 'z' || text_is_cjk_dominant(src);
        bool tgt_en = tl[0] == 'e' || (!text_is_cjk_dominant(tgt) && tgt[0]);
        set_text(s_src_tag, src_zh ? "中文" : "EN");
        set_text(s_tgt_tag, tgt_en ? "EN · TTS" : "中文 · TTS");
        lv_obj_set_style_text_color(s_tgt_tag, lv_color_hex(tgt_en ? COLOR_EN : COLOR_ACCENT), 0);
        set_text(s_src_text, src);
        set_text(s_tgt_text, tgt[0] ? tgt : (st == XLATE_ERROR ? status : ""));
        set_text(s_tts_row, tgt_en ? "正在播报英文" : "正在播报中文");
        pill_set("再按住说话", COLOR_METER, COLOR_CAPTION);
    } else if (st == XLATE_ERROR) {
        show_layer(s_idle_mark);
        set_text(s_sub, status[0] ? status : "出错了");
        pill_set("再试一次", COLOR_METER, COLOR_CAPTION);
    } else {
        /* Idle: title only — no tip/coaching card text. */
        show_layer(s_idle_mark);
        set_text(s_sub, "");
        pill_set("", COLOR_METER, COLOR_CAPTION);
        return;
    }
}

void muse_translate_tick(float now)
{
    (void)now;
    if (!s_root) {
        return;
    }
    if (s_dirty) {
        apply_ui();
    }
    if (s_state == XLATE_LISTEN && s_listen && !lv_obj_has_flag(s_listen, LV_OBJ_FLAG_HIDDEN)) {
        s_wave_phase = (s_wave_phase + 1) % 7;
        static const int heights[] = { 10, 22, 14, 26, 12, 20, 8 };
        for (int i = 0; i < 7; i++) {
            if (!s_wave[i]) {
                continue;
            }
            int h = heights[(i + s_wave_phase) % 7];
            lv_obj_set_height(s_wave[i], h);
        }
    }
}

void muse_translate_build(lv_obj_t *tile, int w, int h)
{
    s_w = w;
    s_h = h;
#ifdef ESP_PLATFORM
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
    }
#endif
    s_root = lv_obj_create(tile);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, w, h);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    s_title = label(s_root, &lv_font_montserrat_20, COLOR_TEXT, "翻译");
#if CONFIG_MUSE_CJK_FONT
    lv_obj_set_style_text_font(s_title, xf_font(), 0);
    lv_label_set_text(s_title, "翻译");
#endif
    lv_obj_set_style_text_letter_space(s_title, 3, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 40);

    s_sub = label(s_root, xf_font_sm(), COLOR_DIM, "");
    lv_obj_align(s_sub, LV_ALIGN_TOP_MID, 0, 66);

    /* Idle mark: quiet center (no tip card). */
    s_idle_mark = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_idle_mark);
    lv_obj_set_size(s_idle_mark, w - 80, 120);
    lv_obj_align(s_idle_mark, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *globe = label(s_idle_mark, &lv_font_montserrat_28, COLOR_ACCENT, LV_SYMBOL_REFRESH);
    lv_obj_center(globe);

    /* Listening */
    s_listen = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_listen);
    lv_obj_set_size(s_listen, w, h - 140);
    lv_obj_align(s_listen, LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_add_flag(s_listen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(s_listen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_listen, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_listen, 10, 0);

    lv_obj_t *mic = lv_obj_create(s_listen);
    lv_obj_remove_style_all(mic);
    lv_obj_set_size(mic, 88, 88);
    lv_obj_set_style_radius(mic, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(mic, 3, 0);
    lv_obj_set_style_border_color(mic, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_bg_color(mic, lv_color_hex(0x16122a), 0);
    lv_obj_set_style_bg_opa(mic, LV_OPA_COVER, 0);
    lv_obj_t *mic_icon = label(mic, &lv_font_montserrat_28, COLOR_CAPTION, LV_SYMBOL_AUDIO);
    lv_obj_center(mic_icon);

    lv_obj_t *wave_row = lv_obj_create(s_listen);
    lv_obj_remove_style_all(wave_row);
    lv_obj_set_size(wave_row, 80, 28);
    lv_obj_set_flex_flow(wave_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wave_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(wave_row, 4, 0);
    static const int wh[] = { 10, 22, 14, 26, 12, 20, 8 };
    for (int i = 0; i < 7; i++) {
        s_wave[i] = lv_obj_create(wave_row);
        lv_obj_remove_style_all(s_wave[i]);
        lv_obj_set_size(s_wave[i], 4, wh[i]);
        lv_obj_set_style_radius(s_wave[i], 2, 0);
        lv_obj_set_style_bg_color(s_wave[i], lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_set_style_bg_opa(s_wave[i], LV_OPA_COVER, 0);
    }
    label(s_listen, xf_font(), COLOR_CAPTION, "聆听中…");
    label(s_listen, xf_font_sm(), COLOR_DIM, "松开 BOOT 发送");

    /* Result */
    s_result = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_result);
    lv_obj_set_size(s_result, w - 80, h - 160);
    lv_obj_align(s_result, LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_add_flag(s_result, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(s_result, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_result, 8, 0);
    lv_obj_set_scrollbar_mode(s_result, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_result, LV_DIR_VER);

    lv_obj_t *src_b = lv_obj_create(s_result);
    lv_obj_remove_style_all(src_b);
    lv_obj_set_width(src_b, lv_pct(100));
    lv_obj_set_height(src_b, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(src_b, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_bg_opa(src_b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(src_b, 14, 0);
    lv_obj_set_style_pad_all(src_b, 10, 0);
    lv_obj_set_style_border_width(src_b, 0, 0);
    lv_obj_set_style_pad_left(src_b, 12, 0);
    lv_obj_set_style_outline_width(src_b, 0, 0);
    /* left bar via border */
    lv_obj_set_style_border_side(src_b, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(src_b, 3, 0);
    lv_obj_set_style_border_color(src_b, lv_color_hex(COLOR_DIM), 0);

    lv_obj_t *src_head = lv_obj_create(src_b);
    lv_obj_remove_style_all(src_head);
    lv_obj_set_size(src_head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(src_head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(src_head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_src_lab = label(src_head, xf_font_sm(), COLOR_DIM, "原文");
    s_src_tag = label(src_head, xf_font_sm(), COLOR_CAPTION, "中文");
    s_src_text = label(src_b, xf_font(), COLOR_TEXT, "");
    lv_label_set_long_mode(s_src_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_src_text, lv_pct(100));

    lv_obj_t *tgt_b = lv_obj_create(s_result);
    lv_obj_remove_style_all(tgt_b);
    lv_obj_set_width(tgt_b, lv_pct(100));
    lv_obj_set_height(tgt_b, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(tgt_b, lv_color_hex(0x221c3a), 0);
    lv_obj_set_style_bg_opa(tgt_b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tgt_b, 14, 0);
    lv_obj_set_style_pad_all(tgt_b, 10, 0);
    lv_obj_set_style_border_side(tgt_b, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(tgt_b, 3, 0);
    lv_obj_set_style_border_color(tgt_b, lv_color_hex(COLOR_ACCENT), 0);

    lv_obj_t *tgt_head = lv_obj_create(tgt_b);
    lv_obj_remove_style_all(tgt_head);
    lv_obj_set_size(tgt_head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tgt_head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tgt_head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_tgt_lab = label(tgt_head, xf_font_sm(), COLOR_DIM, "译文");
    s_tgt_tag = label(tgt_head, xf_font_sm(), COLOR_EN, "EN · TTS");
    s_tgt_text = label(tgt_b, xf_font(), COLOR_TEXT, "");
    lv_label_set_long_mode(s_tgt_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_tgt_text, lv_pct(100));
    s_tts_row = label(tgt_b, xf_font_sm(), COLOR_DIM, "");

    /* Footer pill */
    s_pill = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_pill);
    lv_obj_set_style_bg_color(s_pill, lv_color_hex(COLOR_METER), 0);
    lv_obj_set_style_bg_opa(s_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_pill, 20, 0);
    lv_obj_set_style_pad_hor(s_pill, 18, 0);
    lv_obj_set_style_pad_ver(s_pill, 7, 0);
    lv_obj_align(s_pill, LV_ALIGN_BOTTOM_MID, 0, -36);
    label(s_pill, xf_font_sm(), COLOR_CAPTION, "");
    lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN);

    s_state = XLATE_IDLE;
    s_dirty = true;
    (void)s_src_lab;
    (void)s_tgt_lab;
    apply_ui();
}

void muse_translate_show(void)
{
    muse_ui_show_translate();
    s_dirty = true;
    apply_ui();
}
