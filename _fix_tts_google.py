from pathlib import Path
import re

# 1) Fix URL encoding: use %20 not +
p = Path(r'C:\esp\muse-gadget-sdk\esp32\components\muse\muse_chat_session.cpp')
t = p.read_text(encoding='utf-8')
old = '''        } else if (c == ' ') {
            out[o++] = '+';
        } else {'''
new = '''        } else {'''
if old not in t:
    raise SystemExit('encode block not found')
t = t.replace(old, new, 1)

# Better UA + log first bytes on non-2xx
old_fetch_tail = '''    esp_http_client_set_header(c, "User-Agent", "MuseGadget/1.0");
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    bool ok = (err == ESP_OK && status >= 200 && status < 300 && s_turn.mp3_len > before);
    ESP_LOGI(TAG, "TTS HTTP %d err=%s got %u bytes", status, esp_err_to_name(err),
             (unsigned)(s_turn.mp3_len - before));
    return ok;
}'''
new_fetch_tail = '''    esp_http_client_set_header(c, "User-Agent",
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
}'''
if old_fetch_tail not in t:
    raise SystemExit('fetch tail not found')
t = t.replace(old_fetch_tail, new_fetch_tail, 1)
p.write_text(t, encoding='utf-8', newline='\n')
print('session.cpp: encode + Google-ready fetch checks')

# 2) Kconfig defaults -> Google
k = Path(r'C:\esp\muse-gadget-sdk\esp32\components\muse\Kconfig')
kt = k.read_text(encoding='utf-8')
kt2 = kt.replace(
    'default "https://dict.youdao.com/dictvoice?type=0&audio="',
    'default "https://translate.googleapis.com/translate_tts?ie=UTF-8&client=gtx&tl=zh-CN&q="'
).replace(
    'default "https://dict.youdao.com/dictvoice?type=1&audio="',
    'default "https://translate.googleapis.com/translate_tts?ie=UTF-8&client=gtx&tl=en&q="'
)
# help text tweak
kt2 = kt2.replace('default: Youdao\n            dictvoice, good for Chinese',
                  'default: Google translate_tts (works via WireGuard; Youdao often 500)')
if kt2 == kt:
    # try simpler help replace
    kt2 = kt.replace('Youdao dictvoice, good for Chinese). Change MUSE_TTS_URL_ZH/EN if needed.',
                     'Google translate_tts via WG). Change MUSE_TTS_URL_ZH/EN if needed.')
k.write_text(kt2 if kt2 != kt else kt.replace(
    'built-in TTS; this uses an external HTTP endpoint (default: Youdao\n            dictvoice, good for Chinese). Change MUSE_TTS_URL_ZH/EN if needed.',
    'built-in TTS; default Google translate_tts (reaches via WireGuard).\n            Change MUSE_TTS_URL_ZH/EN if needed.'
), encoding='utf-8', newline='\n')
print('Kconfig defaults updated')

# 3) sdkconfig values
def set_cfg(path, pairs):
    p = Path(path)
    t = p.read_text(encoding='utf-8')
    for k,v in pairs:
        line = f'{k}="{v}"' if not v.startswith('"') else f'{k}={v}'
        if v in ('y','n') or v.startswith(('y','n')) and len(v)==1:
            line = f'{k}={v}'
        pat = re.compile(r'^#?\s*' + re.escape(k) + r'.*$', re.M)
        if pat.search(t):
            t = pat.sub(line if '=' in line else f'{k}={v}', t)
        else:
            t = t.rstrip() + '\n' + (line if '=' in str(line) else f'{k}={v}') + '\n'
    p.write_text(t, encoding='utf-8', newline='\n')
    print('updated', path)

zh = 'https://translate.googleapis.com/translate_tts?ie=UTF-8&client=gtx&tl=zh-CN&q='
en = 'https://translate.googleapis.com/translate_tts?ie=UTF-8&client=gtx&tl=en&q='
for path in [
    r'C:\esp\muse-gadget-sdk\esp32\build-muse-waveshare-s3-185b\sdkconfig',
    r'C:\esp\muse-gadget-sdk\esp32\devices\sdkconfig.wg.local',
    r'C:\esp\muse-gadget-sdk\esp32\devices\sdkconfig.muse-waveshare-s3-185b',
]:
    pp = Path(path)
    if not pp.exists():
        continue
    t = pp.read_text(encoding='utf-8')
    for key, val in [('CONFIG_MUSE_TTS_URL_ZH', zh), ('CONFIG_MUSE_TTS_URL_EN', en)]:
        line = f'{key}="{val}"'
        pat = re.compile(r'^#?\s*' + re.escape(key) + r'.*$', re.M)
        if pat.search(t):
            t = pat.sub(line, t)
        else:
            t = t.rstrip() + '\n' + line + '\n'
    pp.write_text(t, encoding='utf-8', newline='\n')
    print('tts urls ->', path)

print('done')
