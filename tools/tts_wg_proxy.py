#!/usr/bin/env python3
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
