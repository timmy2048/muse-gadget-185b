# -*- coding: utf-8 -*-
from pathlib import Path
import re

# --- Firmware: TTS URL order + ensure ID3 path ---
p = Path(r"C:\esp\muse-gadget-sdk\esp32\components\muse\muse_chat_session.cpp")
t = p.read_text(encoding="utf-8")
assert "mp3_looks_valid" in t and "load_embedded_reply_mp3" in t

# Replace fetch_tts_mp3 prefix list: WG-local optional, Baidu, Youdao, Google
old = '''    const char *prefixes[2];
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
}'''

new = '''    /* Prefer WG-local proxy (10.0.0.1), then China CDN, then Google, then embedded. */
    const char *prefixes[4];
    int npref;
    if (muse_text_has_cjk(clip)) {
        prefixes[0] = CONFIG_MUSE_TTS_URL_ZH;
        prefixes[1] = "https://fanyi.baidu.com/gettts?lan=zh&spd=5&source=web&text=";
        prefixes[2] = "https://dict.youdao.com/dictvoice?type=0&audio=";
        prefixes[3] = "https://translate.googleapis.com/translate_tts?ie=UTF-8&client=gtx&tl=zh-CN&q=";
        npref = 4;
    } else {
        prefixes[0] = CONFIG_MUSE_TTS_URL_EN;
        prefixes[1] = "https://fanyi.baidu.com/gettts?lan=en&spd=5&source=web&text=";
        prefixes[2] = "https://dict.youdao.com/dictvoice?type=1&audio=";
        prefixes[3] = "https://translate.googleapis.com/translate_tts?ie=UTF-8&client=gtx&tl=en&q=";
        npref = 4;
    }
    char url[TTS_URL_MAX];
    for (int i = 0; i < npref; i++) {
        if (!prefixes[i] || !prefixes[i][0]) {
            continue;
        }
        if (snprintf(url, sizeof(url), "%s%s", prefixes[i], enc) >= (int)sizeof(url)) {
            continue;
        }
        ESP_LOGI(TAG, "TTS try %d", i);
        if (fetch_tts_url(url)) {
            return true;
        }
    }
    return false;
}'''

if old not in t:
    raise SystemExit("fetch_tts_mp3 prefix block not found")
t = t.replace(old, new, 1)
p.write_text(t, encoding="utf-8")
print("session.cpp TTS order updated")

# Kconfig defaults: WG-local proxy first as CONFIG_MUSE_TTS_URL_*
k = Path(r"C:\esp\muse-gadget-sdk\esp32\components\muse\Kconfig")
kt = k.read_text(encoding="utf-8")
kt = kt.replace(
    'default "https://translate.googleapis.com/translate_tts?ie=UTF-8&client=gtx&tl=zh-CN&q="',
    'default "http://10.0.0.1:8787/tts?q="'
).replace(
    'default "https://translate.googleapis.com/translate_tts?ie=UTF-8&client=gtx&tl=en&q="',
    'default "http://10.0.0.1:8787/tts?q="'
)
# help note
if "10.0.0.1:8787" not in kt:
    pass
k.write_text(kt, encoding="utf-8")
print("Kconfig defaults -> WG 10.0.0.1:8787")

zh = 'http://10.0.0.1:8787/tts?q='
en = zh
for path in [
    r"C:\esp\muse-gadget-sdk\esp32\build-muse-waveshare-s3-185b\sdkconfig",
    r"C:\esp\muse-gadget-sdk\esp32\devices\sdkconfig.wg.local",
    r"C:\esp\muse-gadget-sdk\esp32\devices\sdkconfig.muse-waveshare-s3-185b",
]:
    pp = Path(path)
    if not pp.exists():
        continue
    s = pp.read_text(encoding="utf-8")
    for key, val in [("CONFIG_MUSE_TTS_URL_ZH", zh), ("CONFIG_MUSE_TTS_URL_EN", en)]:
        line = f'{key}="{val}"'
        pat = re.compile(r"^#?\s*" + re.escape(key) + r".*$", re.M)
        if pat.search(s):
            s = pat.sub(line, s)
        else:
            s = s.rstrip() + "\n" + line + "\n"
    pp.write_text(s, encoding="utf-8")
    print("updated", path)

# Minimal TTS proxy for VPS (WireGuard 10.0.0.1) — Baidu gettts backend
proxy = Path(r"C:\esp\muse-gadget-sdk\tools\tts_wg_proxy.py")
proxy.parent.mkdir(parents=True, exist_ok=True)
proxy.write_text(r'''#!/usr/bin/env python3
"""TTS MP3 proxy for Muse Gadget over WireGuard.

Run on the WG server (usually 10.0.0.1):
  python3 tts_wg_proxy.py
Gadget CONFIG_MUSE_TTS_URL_* = http://10.0.0.1:8787/tts?q=

Fetches Baidu gettts (China-friendly) and returns audio/mpeg.
"""
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs, quote
from urllib.request import Request, urlopen
import sys

HOST, PORT = "0.0.0.0", 8787

def fetch_mp3(text: str) -> bytes:
    text = text.strip()[:180]
    if not text:
        return b""
    # Prefer Baidu; fall back to Google gtx
    urls = [
        "https://fanyi.baidu.com/gettts?lan=zh&spd=5&source=web&text=" + quote(text),
        "https://translate.googleapis.com/translate_tts?ie=UTF-8&client=gtx&tl=zh-CN&q=" + quote(text),
    ]
    # crude lang pick: ASCII-heavy -> en
    if all(ord(c) < 128 for c in text):
        urls = [
            "https://fanyi.baidu.com/gettts?lan=en&spd=5&source=web&text=" + quote(text),
            "https://translate.googleapis.com/translate_tts?ie=UTF-8&client=gtx&tl=en&q=" + quote(text),
        ]
    for u in urls:
        try:
            req = Request(u, headers={"User-Agent": "Mozilla/5.0"})
            with urlopen(req, timeout=15) as r:
                data = r.read()
            if len(data) > 64 and (data[0] == 0xFF or data[:3] == b"ID3"):
                return data
        except Exception as e:
            print("upstream fail", e, file=sys.stderr)
    return b""

class H(BaseHTTPRequestHandler):
    def do_GET(self):
        q = parse_qs(urlparse(self.path).query)
        text = (q.get("q") or q.get("text") or [""])[0]
        data = fetch_mp3(text)
        if not data:
            self.send_response(502)
            self.end_headers()
            self.wfile.write(b"tts upstream failed")
            return
        self.send_response(200)
        self.send_header("Content-Type", "audio/mpeg")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

if __name__ == "__main__":
    print("Muse TTS proxy on http://0.0.0.0:%d/tts?q=  (use http://10.0.0.1:%d/tts?q= from gadget)" % (PORT, PORT))
    HTTPServer((HOST, PORT), H).serve_forever()
''', encoding="utf-8")
print("wrote", proxy)
