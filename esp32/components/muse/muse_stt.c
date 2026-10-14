/*
 * HTTP client for Muse STT proxy (Azure Speech short-audio behind WG).
 */
#include "sdkconfig.h"

#if CONFIG_MUSE_STT_HTTP

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "muse_audio.h"
#include "muse_stt.h"

static const char *TAG = "muse_stt";

#define RESP_MAX 2048

typedef struct {
    char buf[RESP_MAX];
    size_t len;
} resp_t;

static esp_err_t on_http(esp_http_client_event_t *evt)
{
    resp_t *r = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }
    size_t n = evt->data_len;
    if (r->len + n >= sizeof(r->buf)) {
        n = sizeof(r->buf) - 1 - r->len;
    }
    if (n) {
        memcpy(r->buf + r->len, evt->data, n);
        r->len += n;
        r->buf[r->len] = '\0';
    }
    return ESP_OK;
}

static void write_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void write_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

/* Build a minimal 16-bit mono WAV in PSRAM (header + PCM). Caller frees. */
static uint8_t *make_wav(const int16_t *pcm, size_t frames, size_t *out_len)
{
    size_t data_bytes = frames * sizeof(int16_t);
    size_t total = 44 + data_bytes;
    uint8_t *wav = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) {
        wav = malloc(total);
    }
    if (!wav) {
        return NULL;
    }
    memcpy(wav, "RIFF", 4);
    write_u32(wav + 4, (uint32_t)(36 + data_bytes));
    memcpy(wav + 8, "WAVEfmt ", 8);
    write_u32(wav + 16, 16);
    write_u16(wav + 20, 1);   /* PCM */
    write_u16(wav + 22, 1);   /* mono */
    write_u32(wav + 24, (uint32_t)MUSE_AUDIO_RATE);
    write_u32(wav + 28, (uint32_t)(MUSE_AUDIO_RATE * 2));
    write_u16(wav + 32, 2);   /* block align */
    write_u16(wav + 34, 16);  /* bits */
    memcpy(wav + 36, "data", 4);
    write_u32(wav + 40, (uint32_t)data_bytes);
    memcpy(wav + 44, pcm, data_bytes);
    *out_len = total;
    return wav;
}


static char *recognize_url(const int16_t *pcm, size_t frames, const char *url, char *lang_out, size_t lang_cap)
{
    if (!pcm || frames < MUSE_AUDIO_RATE / 5) {   /* <0.2s useless */
        return NULL;
    }
    size_t wav_len = 0;
    uint8_t *wav = make_wav(pcm, frames, &wav_len);
    if (!wav) {
        ESP_LOGE(TAG, "no mem for wav (%u frames)", (unsigned)frames);
        return NULL;
    }

    resp_t resp = { 0 };
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 25000,
        .buffer_size = 2048,
        .buffer_size_tx = 4096,
        .event_handler = on_http,
        .user_data = &resp,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        free(wav);
        return NULL;
    }
    esp_http_client_set_header(c, "Content-Type", "audio/wav");
    esp_http_client_set_header(c, "Accept", "application/json");
    esp_http_client_set_header(c, "User-Agent", "muse-gadget-stt");
    esp_http_client_set_post_field(c, (const char *)wav, (int)wav_len);

    ESP_LOGI(TAG, "POST %s (%u bytes, %.2fs)", url, (unsigned)wav_len,
             (double)frames / MUSE_AUDIO_RATE);
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    free(wav);

    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG, "stt http fail err=%s status=%d body=%.*s", esp_err_to_name(err), status,
                 (int)resp.len, resp.buf);
        return NULL;
    }

    cJSON *root = cJSON_ParseWithLength(resp.buf, resp.len);
    if (!root) {
        ESP_LOGW(TAG, "stt bad json: %.*s", (int)resp.len, resp.buf);
        return NULL;
    }
    const cJSON *ok = cJSON_GetObjectItem(root, "ok");
    const cJSON *text = cJSON_GetObjectItem(root, "text");
    const cJSON *language = cJSON_GetObjectItem(root, "language");
    const cJSON *detected = cJSON_GetObjectItem(root, "detected");
    char *out = NULL;
    if (cJSON_IsTrue(ok) && cJSON_IsString(text) && text->valuestring && text->valuestring[0]) {
        out = strdup(text->valuestring);
        ESP_LOGI(TAG, "recognized: %s", out);
        if (lang_out && lang_cap) {
            lang_out[0] = '\0';
            const char *lang = NULL;
            if (cJSON_IsString(detected) && detected->valuestring && detected->valuestring[0]) {
                lang = detected->valuestring;
            } else if (cJSON_IsString(language) && language->valuestring) {
                lang = language->valuestring;
            }
            if (lang) {
                if (lang[0] == 'z' || lang[0] == 'Z') {
                    snprintf(lang_out, lang_cap, "zh");
                } else if (lang[0] == 'e' || lang[0] == 'E') {
                    snprintf(lang_out, lang_cap, "en");
                } else {
                    snprintf(lang_out, lang_cap, "%s", lang);
                }
            }
        }
    } else {
        ESP_LOGW(TAG, "stt empty/fail: %.*s", (int)resp.len, resp.buf);
    }
    cJSON_Delete(root);
    return out;
}

char *muse_stt_recognize_pcm(const int16_t *pcm, size_t frames)
{
    return recognize_url(pcm, frames, CONFIG_MUSE_STT_URL, NULL, 0);
}

char *muse_stt_recognize_pcm_auto(const int16_t *pcm, size_t frames, char *lang_out, size_t lang_cap)
{
    char url[256];
    /* Append ?lang=auto or &lang=auto */
    if (strchr(CONFIG_MUSE_STT_URL, '?')) {
        snprintf(url, sizeof(url), "%s&lang=auto", CONFIG_MUSE_STT_URL);
    } else {
        snprintf(url, sizeof(url), "%s?lang=auto", CONFIG_MUSE_STT_URL);
    }
    char *out = recognize_url(pcm, frames, url, lang_out, lang_cap);
    if (out) {
        return out;
    }
    /* Older VPS without auto: fall back to default STT. */
    return recognize_url(pcm, frames, CONFIG_MUSE_STT_URL, lang_out, lang_cap);
}

#endif /* CONFIG_MUSE_STT_HTTP */
