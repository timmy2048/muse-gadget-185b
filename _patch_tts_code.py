from pathlib import Path

p = Path(r'C:\esp\muse-gadget-sdk\esp32\components\muse\muse_chat_session.cpp')
t = p.read_text(encoding='utf-8')

# includes
if 'esp_http_client.h' not in t:
    t = t.replace('#include "esp_crt_bundle.h"',
                  '#include "esp_crt_bundle.h"\n#include "esp_http_client.h"')
if 'muse_text.h' not in t:
    t = t.replace('#include "muse_settings.h"',
                  '#include "muse_settings.h"\n#include "muse_text.h"')

# TTS helpers + start_tts replacement
old_start = '''static void start_tts(void)
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

if old_start not in t:
    raise SystemExit('start_tts block not found exactly — abort')

new_block = r'''#if CONFIG_MUSE_TTS_HTTP
#define TTS_TEXT_MAX 180   /* Youdao GET URL budget; longer replies are truncated */
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
        } else if (c == ' ') {
            out[o++] = '+';
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = '\0';
    return o;
}

/* Truncate on a UTF-8 boundary so TTS URLs stay short. */
static void tts_clip_text(const char *in, char *out, size_t cap)
{
    if (!in || !cap) {
        if (cap) out[0] = '\0';
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

/* Fetch MP3 for `text` into s_turn.mp3 via tts_data(). Returns true if any audio arrived. */
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
    esp_http_client_set_header(c, "User-Agent", "MuseGadget/1.0");
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    bool ok = (err == ESP_OK && status >= 200 && status < 300 && s_turn.mp3_len > before);
    ESP_LOGI(TAG, "TTS HTTP %d err=%s got %u bytes", status, esp_err_to_name(err),
             (unsigned)(s_turn.mp3_len - before));
    return ok;
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
        }
#endif
        /*
         * Speaker off, or TTS disabled/unavailable: silence paces captions.
         * STT: voice notes already go to Muse as WAV on /chat/stream (VOICE_NOTE);
         * the VM transcribes server-side. Streaming /api/voice/dictation has no
         * ASR on the VM today (see VOICE_NOTE comment above).
         */
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

t = t.replace(old_start, new_block)
p.write_text(t, encoding='utf-8', newline='\n')
print('muse_chat_session.cpp: start_tts wired')

# forward declare tts_data before fetch uses it — tts_data is defined AFTER start_tts
# Our fetch_tts_mp3 calls tts_data which is currently defined later.
# In C++, static functions need declaration before use. Add forward decl.
if 'static void tts_data(const uint8_t *data, size_t len);' not in t:
    t = p.read_text(encoding='utf-8')
    needle = '#if CONFIG_MUSE_TTS_HTTP\n#define TTS_TEXT_MAX'
    decl = 'static void tts_data(const uint8_t *data, size_t len);\n\n#if CONFIG_MUSE_TTS_HTTP\n#define TTS_TEXT_MAX'
    if needle in t:
        t = t.replace(needle, decl, 1)
        p.write_text(t, encoding='utf-8', newline='\n')
        print('added tts_data forward decl')
    else:
        print('WARN: could not add forward decl')
else:
    print('forward decl ok')
