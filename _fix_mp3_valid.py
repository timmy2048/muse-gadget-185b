from pathlib import Path
p = Path(r"C:\esp\muse-gadget-sdk\esp32\components\muse\muse_chat_session.cpp")
t = p.read_text(encoding="utf-8")

helper = '''
static bool mp3_looks_valid(const uint8_t *data, size_t len)
{
    if (!data || len < 4) {
        return false;
    }
    /* MPEG frame sync or ID3v2 tag (test_reply.mp3 and many TTS bodies). */
    if (data[0] == 0xff && (data[1] & 0xe0) == 0xe0) {
        return true;
    }
    if (len >= 3 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
        return true;
    }
    /* Scan a little for a sync word after a short preamble. */
    size_t lim = len < 2048 ? len - 1 : 2047;
    for (size_t i = 0; i < lim; i++) {
        if (data[i] == 0xff && (data[i + 1] & 0xe0) == 0xe0) {
            return true;
        }
    }
    return false;
}

'''

if "mp3_looks_valid" not in t:
    t = t.replace("static bool fetch_tts_url(const char *url)", helper + "static bool fetch_tts_url(const char *url)")

# Replace both validation sites
t = t.replace(
    "bool ok = (err == ESP_OK && status >= 200 && status < 300 && got > 64 && s_turn.mp3[before] == 0xff);",
    "bool ok = (err == ESP_OK && status >= 200 && status < 300 && got > 64 &&\n"
    "               mp3_looks_valid(s_turn.mp3 + before, got));"
)
t = t.replace(
    "return s_turn.mp3_len > 64 && s_turn.mp3[0] == 0xff;",
    "return s_turn.mp3_len > 64 && mp3_looks_valid(s_turn.mp3, s_turn.mp3_len);"
)
p.write_text(t, encoding="utf-8")
print("patched mp3_looks_valid", "mp3_looks_valid" in t)
