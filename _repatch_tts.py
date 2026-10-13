# -*- coding: utf-8 -*-
from pathlib import Path

p = Path(r"C:\esp\muse-gadget-sdk\esp32\components\muse\muse_chat_session.cpp")
t = p.read_text(encoding="utf-8")
assert "start_tts" in t and len(t) > 50000

# 1) includes
if "esp_http_client.h" not in t:
    t = t.replace('#include "esp_crt_bundle.h"', '#include "esp_crt_bundle.h"\n#include "esp_http_client.h"\n#include "driver/gpio.h"')
if "muse_text.h" not in t:
    t = t.replace('#include "muse_settings.h"', '#include "muse_settings.h"\n#include "muse_text.h"')

# 2) SUB_LINE_MAX
t = t.replace("#define SUB_LINE_MAX (256 * 1024)", "#define SUB_LINE_MAX (1024 * 1024)          /* PSRAM; long message_done NDJSON */")
if "SUB_LINE_MAX (1024 * 1024)" not in t and "SUB_LINE_MAX (256" in t:
    raise SystemExit("SUB_LINE_MAX replace failed")

# 3) Replace start_tts block (stock silent version)
old = '''static void start_tts(void)
{
    if (s_turn.tts_msg >= 0) {
        return;
    }
    for (int i = 0; i < s_turn.nmsgs; i++) {
        msg_t &m = s_turn.msgs[i];
        if (m.tts != TTS_QUEUED) {
            continue;
        }
        /*
         * Replies are text, shown at reading pace: silence in place of speech
         * paces the captions and ends the turn. To speak them instead, send
         * the message's text (s_turn.texts + i * TEXT_MAX, if texts was
         * allocated; up to TEXT_MAX - 1 bytes) to a TTS API of your choice and
         * play the MP3 it returns. In place of the silence below: keep
         * m.tts = TTS_ACTIVE and s_turn.tts_msg = i, set s_turn.silent = false,
         * m.pcm_start = s_turn.pcm_out, m.pcm_frames = 0, s_turn.mp3_len = 0,
         * s_turn.mp3_ended = false, s_turn.kbps = 0, s_turn.down_rate = 0 and
         * mp3dec_init(&s_turn.dec). Then, on this task, pass the MP3 to
         * tts_data() as it arrives (it buffers up to MP3_BUF and drops the
         * rest, so hold off while it's full) and set s_turn.mp3_ended at the
         * end. decode() plays it at the speaker's volume, captions following,
         * and finishes the message once it's drained.
         */
        m.pcm_start = s_turn.pcm_out;
        m.pcm_frames = (uint32_t)(m.len * MIC_RATE / TEXT_CHARS_PER_S);
        m.tts = TTS_ACTIVE;
        s_turn.tts_msg = i;
        s_turn.silent = true;
        ESP_LOGI(TAG, "showing message %s (%u chars)", m.id, (unsigned)m.len);
        show_reply_start(m);
        return;
    }
}'''
if old not in t:
    raise SystemExit("stock start_tts not found — file may already be patched differently")

new = r'''static void tts_data(const uint8_t *data, size_t len);

#if CONFIG_MUSE_TTS_HTTP
#define TTS_TEXT_MAX 180
#define TTS_URL_MAX  1200

static size_t tts_urlencode(const char *in, char *out, size_t out_cap)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < out_cap; p++) {
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

static void tts_clip_text(const char *in, char *out, size_t cap)
{
    if (!in || !cap) {
        if (cap) {
            out[0] = '\0';
        }
        return;
    }
    size_t n = 0;
    while (in[n] && n + 1 < cap && n < TTS_TEXT_MAX) {
        unsigned char c = (unsigned char)in[n];
        size_t need = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        if (n + need > TTS_TEXT_MAX || n + need >= cap) {
            break;
        }
        for (size_t i = 0; i < need; i++) {
            out[n + i] = in[n + i];
        }
        n += need;
    }
    out[n] = '\0';
}

static esp_err_t tts_http_on_data(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }
    tts_data(static_cast<const uint8_t *>(evt->data), (size_t)evt->data_len);
    return ESP_OK;
}

static bool fetch_tts_url(const char *url)
{
    size_t before = s_turn.mp3_len;
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 20000;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 1024;
    cfg.event_handler = tts_http_on_data;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = false;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return false;
    }
    esp_http_client_set_header(c, "User-Agent",
                                "Mozilla/5.0 (Linux; Android 10) AppleWebKit/537.36 Chrome/120.0.0.0 Mobile Safari/537.36");
    esp_http_client_set_header(c, "Accept", "audio/mpeg,audio/*;q=0.9,*/*;q=0.8");
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    size_t got = s_turn.mp3_len - before;
    bool ok = (err == ESP_OK && status >= 200 && status < 300 && got > 64 && s_turn.mp3[before] == 0xff);
    ESP_LOGI(TAG, "TTS HTTP %d err=%s got %u bytes%s", status, esp_err_to_name(err), (unsigned)got,
             ok ? "" : " (not usable MP3)");
    if (!ok && got) {
        s_turn.mp3_len = before;
    }
    return ok;
}

static bool fetch_tts_mp3(const char *text)
{
    if (!text || !text[0] || !s_turn.mp3) {
        return false;
    }
    char clip[TTS_TEXT_MAX + 1];
    tts_clip_text(text, clip, sizeof(clip));
    if (!clip[0]) {
        return false;
    }
    char enc[TTS_URL_MAX];
    tts_urlencode(clip, enc, sizeof(enc));
    const char *prefixes[2];
    if (muse_text_has_cjk(clip)) {
        prefixes[0] = CONFIG_MUSE_TTS_URL_ZH;
        prefixes[1] = "https://translate.google.com/translate_tts?ie=UTF-8&client=tw-ob&tl=zh-CN&q=";
    } else {
        prefixes[0] = CONFIG_MUSE_TTS_URL_EN;
        prefixes[1] = "https://translate.google.com/translate_tts?ie=UTF-8&client=tw-ob&tl=en&q=";
    }
    char url[TTS_URL_MAX];
    for (int i = 0; i < 2; i++) {
        if (snprintf(url, sizeof(url), "%s%s", prefixes[i], enc) >= (int)sizeof(url)) {
            continue;
        }
        ESP_LOGI(TAG, "TTS try %d", i);
        if (fetch_tts_url(url)) {
            return true;
        }
    }
    return false;
}

static bool load_embedded_reply_mp3(void)
{
    extern const uint8_t mp3_start[] asm("_binary_test_reply_mp3_start");
    extern const uint8_t mp3_end[] asm("_binary_test_reply_mp3_end");
    size_t len = (size_t)(mp3_end - mp3_start);
    if (!len || !s_turn.mp3) {
        return false;
    }
    s_turn.mp3_len = 0;
    tts_data(mp3_start, len);
    ESP_LOGI(TAG, "TTS fallback: embedded test_reply.mp3 (%u bytes)", (unsigned)s_turn.mp3_len);
    return s_turn.mp3_len > 64 && s_turn.mp3[0] == 0xff;
}
#endif /* CONFIG_MUSE_TTS_HTTP */

static void start_tts(void)
{
    if (s_turn.tts_msg >= 0) {
        return;
    }
    for (int i = 0; i < s_turn.nmsgs; i++) {
        msg_t &m = s_turn.msgs[i];
        if (m.tts != TTS_QUEUED) {
            continue;
        }
        const char *text = (s_turn.texts && m.len) ? (s_turn.texts + i * TEXT_MAX) : nullptr;
        bool want_speak = muse_settings_speaker_on() && text && text[0];
#if CONFIG_MUSE_TTS_HTTP
        if (want_speak) {
            m.pcm_start = s_turn.pcm_out;
            m.pcm_frames = 0;
            m.tts = TTS_ACTIVE;
            s_turn.tts_msg = i;
            s_turn.silent = false;
            s_turn.mp3_len = 0;
            s_turn.mp3_ended = false;
            s_turn.kbps = 0;
            s_turn.down_rate = 0;
            mp3dec_init(&s_turn.dec);
            mark(M_TTS);
            gpio_set_direction(GPIO_NUM_9, GPIO_MODE_OUTPUT);
            gpio_set_level(GPIO_NUM_9, 1);
            ESP_LOGI(TAG, "speaking message %s (%u chars) speaker=%d vol=%d silent=0 PA9=1",
                     m.id, (unsigned)m.len, (int)muse_settings_speaker_on(), muse_settings_volume());
            show_reply_start(m);
            bool got = fetch_tts_mp3(text);
            if (!got) {
                ESP_LOGW(TAG, "TTS HTTP failed; using embedded test_reply.mp3");
                got = load_embedded_reply_mp3();
            }
            if (!got) {
                ESP_LOGW(TAG, "no MP3; silent captions");
                s_turn.silent = true;
                m.pcm_frames = (uint32_t)(m.len * MIC_RATE / TEXT_CHARS_PER_S);
                s_turn.mp3_len = 0;
                s_turn.mp3_ended = true;
            } else {
                s_turn.mp3_ended = true;
                ESP_LOGI(TAG, "TTS ready: %u mp3 bytes silent=%d", (unsigned)s_turn.mp3_len, (int)s_turn.silent);
            }
            return;
        }
#endif
        (void)want_speak;
        m.pcm_start = s_turn.pcm_out;
        m.pcm_frames = (uint32_t)(m.len * MIC_RATE / TEXT_CHARS_PER_S);
        m.tts = TTS_ACTIVE;
        s_turn.tts_msg = i;
        s_turn.silent = true;
        ESP_LOGI(TAG, "showing message %s (%u chars)", m.id, (unsigned)m.len);
        show_reply_start(m);
        return;
    }
}'''

t = t.replace(old, new, 1)

# 4) typed-turn message_done: queue TTS + keep console
old_md = '''    if (s_turn.text) {
        /* The whole text if the pieces didn't add up to it (a line skipped, say): the reader uses it instead. */
        size_t n = m.len;
        if (final_text && final_text[0] && strlen(final_text) != m.len) {
            n = strlen(final_text);
            muse_hatch_console("final", final_text, "\\"msg\\":%d", i);
        }
        muse_hatch_console("message_done", nullptr, "\\"msg\\":%d,\\"bytes\\":%u", i, (unsigned)n);
        ESP_LOGI(TAG, "message %s done (%u chars)", m.id, (unsigned)n);
        return;
    }'''
new_md = '''    if (s_turn.text) {
        /* Console still gets the reply; also queue TTS when speaker is on. */
        size_t n = m.len;
        if (final_text && final_text[0] && strlen(final_text) != m.len) {
            n = strlen(final_text);
            muse_hatch_console("final", final_text, "\\"msg\\":%d", i);
        }
        muse_hatch_console("message_done", nullptr, "\\"msg\\":%d,\\"bytes\\":%u", i, (unsigned)n);
        ESP_LOGI(TAG, "message %s done (%u chars)", m.id, (unsigned)n);
        if (!m.len && final_text && final_text[0]) {
            append_text(m, final_text);
        }
        if (m.len && m.tts == TTS_NONE && muse_settings_speaker_on()) {
            m.tts = TTS_QUEUED;
        }
        return;
    }'''
if old_md not in t:
    raise SystemExit("message_done text block not found")
t = t.replace(old_md, new_md, 1)

# 5) typed deltas fill texts[]
old_d = '''        if (text && text[0] && s_turn.text) {
            mark(M_TEXT);
            m.len += strlen(text);
            muse_hatch_console("text", text, "\\"msg\\":%d", i);
        }'''
new_d = '''        if (text && text[0] && s_turn.text) {
            mark(M_TEXT);
            append_text(m, text);   /* fill texts[] so start_tts can speak */
            muse_hatch_console("text", text, "\\"msg\\":%d", i);
        }'''
if old_d not in t:
    raise SystemExit("typed delta block not found")
t = t.replace(old_d, new_d, 1)

# Avoid duplicate tts_data definition: stock file has tts_data after start_tts.
# Our new block forwards-declares and the original definition must remain.
# Check we didn't leave two "static void tts_data" definitions with bodies.
p.write_text(t, encoding="utf-8")
print("OK size", p.stat().st_size)
print("SUB", "1024 * 1024" in t)
print("TTS HTTP", "fetch_tts_mp3" in t)
print("fallback", "load_embedded_reply_mp3" in t)
print("tts_data defs", t.count("static void tts_data"))
