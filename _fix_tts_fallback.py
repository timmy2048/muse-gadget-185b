from pathlib import Path

p = Path(r'C:\esp\muse-gadget-sdk\esp32\components\muse\muse_chat_session.cpp')
t = p.read_text(encoding='utf-8')

# Add driver/gpio.h if missing
if '#include "driver/gpio.h"' not in t:
    t = t.replace('#include "esp_http_client.h"', '#include "esp_http_client.h"\n#include "driver/gpio.h"')

# Replace fetch_tts_mp3 and start_tts speak block with stronger version
# Find and replace fetch_tts_mp3 function entirely through start_tts speak success path

old_fetch = '''/* Fetch MP3 for `text` into s_turn.mp3 via tts_data(). Returns true if any audio arrived. */
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
    const char *prefix = muse_text_has_cjk(clip) ? CONFIG_MUSE_TTS_URL_ZH : CONFIG_MUSE_TTS_URL_EN;
    char enc[TTS_URL_MAX];
    char url[TTS_URL_MAX];
    tts_urlencode(clip, enc, sizeof(enc));
    if (snprintf(url, sizeof(url), "%s%s", prefix, enc) >= (int)sizeof(url)) {
        ESP_LOGW(TAG, "TTS URL too long, skipping speak");
        return false;
    }
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
    bool ok = (err == ESP_OK && status >= 200 && status < 300 && got > 64 &&
               s_turn.mp3[before] == 0xff); /* MPEG sync */
    ESP_LOGI(TAG, "TTS HTTP %d err=%s got %u bytes%s", status, esp_err_to_name(err), (unsigned)got,
             ok ? "" : " (not usable MP3)");
    if (!ok && got) {
        /* Drop error body so silent fallback does not try to decode it. */
        s_turn.mp3_len = before;
    }
    return ok;
}
#endif /* CONFIG_MUSE_TTS_HTTP */'''

new_fetch = '''/* One HTTP GET of MP3 into tts_data(). */
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
    bool ok = (err == ESP_OK && status >= 200 && status < 300 && got > 64 &&
               s_turn.mp3[before] == 0xff);
    ESP_LOGI(TAG, "TTS HTTP %d err=%s got %u bytes%s", status, esp_err_to_name(err), (unsigned)got,
             ok ? "" : " (not usable MP3)");
    if (!ok && got) {
        s_turn.mp3_len = before;
    }
    return ok;
}

/* Fetch MP3 for `text`. Tries primary URL then Google tw-ob alternate. */
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
        ESP_LOGI(TAG, "TTS try %d: %s...", i, prefixes[i]);
        if (fetch_tts_url(url)) {
            return true;
        }
    }
    return false;
}

/* Last-resort: embedded test_reply.mp3 so the speaker path still plays. */
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
#endif /* CONFIG_MUSE_TTS_HTTP */'''

if old_fetch not in t:
    raise SystemExit('fetch block not found')
t = t.replace(old_fetch, new_fetch, 1)

# Update start_tts speak section for logging + PA + fallback
old_speak = '''        if (want_speak) {
            /*
             * Speak via HTTP TTS (default Youdao dictvoice). Muse/Hatch has no
             * built-in TTS; MP3 is decoded by the existing minimp3 path.
             */
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
            ESP_LOGI(TAG, "speaking message %s (%u chars)", m.id, (unsigned)m.len);
            show_reply_start(m);
            if (!fetch_tts_mp3(text)) {
                ESP_LOGW(TAG, "TTS fetch failed; falling back to silent captions");
                s_turn.silent = true;
                m.pcm_frames = (uint32_t)(m.len * MIC_RATE / TEXT_CHARS_PER_S);
                s_turn.mp3_len = 0;
                s_turn.mp3_ended = true;
            } else {
                s_turn.mp3_ended = true;   /* whole MP3 buffered; decode() drains it */
            }
            return;
        }'''

new_speak = '''        if (want_speak) {
            /*
             * Speak via HTTP TTS (Google translate_tts). Muse/Hatch has no
             * built-in TTS; MP3 goes through minimp3. On HTTP failure, play the
             * embedded test_reply.mp3 so the PA/speaker path still audibly works.
             */
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
            /* Waveshare 1.85B: PA_CTRL = GPIO9, active high (docs). */
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
                ESP_LOGW(TAG, "no MP3 at all; silent captions");
                s_turn.silent = true;
                m.pcm_frames = (uint32_t)(m.len * MIC_RATE / TEXT_CHARS_PER_S);
                s_turn.mp3_len = 0;
                s_turn.mp3_ended = true;
            } else {
                s_turn.mp3_ended = true;
                ESP_LOGI(TAG, "TTS ready: %u mp3 bytes, silent=%d", (unsigned)s_turn.mp3_len,
                         (int)s_turn.silent);
            }
            return;
        }'''

if old_speak not in t:
    raise SystemExit('speak block not found')
t = t.replace(old_speak, new_speak, 1)

p.write_text(t, encoding='utf-8', newline='\n')
print('patched OK')
# sanity
assert 'load_embedded_reply_mp3' in t
assert 'TTS try' in t
assert 'PA9=1' in t
print('markers ok')
