#!/usr/bin/env python3
"""Muse Gadget WireGuard proxy: Azure TTS (GET /tts) + Azure STT (POST /stt).

Binds 10.0.0.1 only (WG). Secrets from muse-tts.env.
  TTS: http://10.0.0.1:8787/tts?q=
  STT: http://10.0.0.1:8788/stt  (POST audio/wav or audio/pcm; returns JSON)

Voice: zh-CN-XiaoxiaoNeural style=friendly (Mainland female with StyleList).
Prosody + punctuation breaks still applied; express-as wraps when STYLE set.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
from urllib.request import Request, urlopen
from xml.sax.saxutils import escape
import json
import os
import re
import sys
import threading

HOST = os.environ.get("MUSE_TTS_HOST", "10.0.0.1")
TTS_PORT = int(os.environ.get("MUSE_TTS_PORT", "8787"))
STT_PORT = int(os.environ.get("MUSE_STT_PORT", "8788"))
REGION = os.environ.get("AZURE_SPEECH_REGION", "eastus").strip()
KEY = os.environ.get("AZURE_SPEECH_KEY", "").strip()
VOICE = os.environ.get("AZURE_SPEECH_VOICE", "zh-CN-XiaoxiaoNeural").strip()
# Default STYLE=friendly for Xiaoxiao. express-as wrapped when STYLE non-empty.
STYLE = os.environ.get("AZURE_SPEECH_STYLE", "friendly").strip()
STYLE_DEGREE = os.environ.get("AZURE_SPEECH_STYLE_DEGREE", "1.2").strip()
PROSODY_RATE = os.environ.get("AZURE_SPEECH_RATE", "+8%").strip()
PROSODY_PITCH = os.environ.get("AZURE_SPEECH_PITCH", "+4%").strip()
OUTPUT_FORMAT = os.environ.get(
    "AZURE_SPEECH_FORMAT", "audio-16khz-32kbitrate-mono-mp3"
).strip()
MAX_CHARS = int(os.environ.get("MUSE_TTS_MAX_CHARS", "180"))
STT_LANG = os.environ.get("AZURE_STT_LANGUAGE", "zh-CN").strip()
STT_MAX_BYTES = int(os.environ.get("MUSE_STT_MAX_BYTES", str(2 * 1024 * 1024)))


def _punct_to_ssml(text: str) -> str:
    """Escape text and insert short breaks at CJK / Latin sentence punctuation."""
    parts = []
    buf = []
    for ch in text:
        if ch in "。！？!?；;":
            buf.append(ch)
            parts.append(escape("".join(buf)))
            parts.append("<break time='280ms'/>")
            buf = []
        elif ch in "，、,":
            buf.append(ch)
            parts.append(escape("".join(buf)))
            parts.append("<break time='120ms'/>")
            buf = []
        else:
            buf.append(ch)
    if buf:
        parts.append(escape("".join(buf)))
    return "".join(parts)


def build_ssml(text: str) -> str:
    body = _punct_to_ssml(text)
    voice_name = escape(VOICE, {"'": "&apos;"})
    inner = (
        "<prosody rate='%s' pitch='%s'>%s</prosody>"
        % (escape(PROSODY_RATE, {"'": "&apos;"}), escape(PROSODY_PITCH, {"'": "&apos;"}), body)
    )
    if STYLE:
        inner = (
            "<mstts:express-as style='%s' styledegree='%s'>%s</mstts:express-as>"
            % (escape(STYLE, {"'": "&apos;"}), escape(STYLE_DEGREE, {"'": "&apos;"}), inner)
        )
    return (
        "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' "
        "xmlns:mstts='https://www.w3.org/2001/mstts' xml:lang='zh-CN'>"
        "<voice xml:lang='zh-CN' name='%s'>%s</voice>"
        "</speak>"
    ) % (voice_name, inner)


def fetch_mp3(text: str) -> bytes:
    text = (text or "").strip()[:MAX_CHARS]
    if not text:
        return b""
    if not KEY:
        print("missing AZURE_SPEECH_KEY", file=sys.stderr)
        return b""
    ssml = build_ssml(text)
    url = "https://%s.tts.speech.microsoft.com/cognitiveservices/v1" % REGION
    req = Request(
        url,
        data=ssml.encode("utf-8"),
        method="POST",
        headers={
            "Ocp-Apim-Subscription-Key": KEY,
            "Content-Type": "application/ssml+xml",
            "X-Microsoft-OutputFormat": OUTPUT_FORMAT,
            "User-Agent": "muse-tts-proxy",
        },
    )
    try:
        with urlopen(req, timeout=20) as r:
            data = r.read()
    except Exception as e:
        print("azure tts fail", e, file=sys.stderr)
        return b""
    if len(data) > 64 and (data[0] == 0xFF or data[:3] == b"ID3"):
        return data
    print("azure tts bad payload len=%d head=%r" % (len(data), data[:16]), file=sys.stderr)
    return b""


def recognize_wav(audio: bytes, content_type: str) -> dict:
    if not KEY:
        return {"ok": False, "error": "missing_key"}
    if not audio:
        return {"ok": False, "error": "empty_audio"}
    lang = STT_LANG
    url = (
        "https://%s.stt.speech.microsoft.com/speech/recognition/conversation/"
        "cognitiveservices/v1?language=%s&format=detailed"
        % (REGION, lang)
    )
    ct = content_type or "audio/wav"
    # Accept raw PCM as 16k mono s16le by wrapping is caller's job; we pass through.
    req = Request(
        url,
        data=audio,
        method="POST",
        headers={
            "Ocp-Apim-Subscription-Key": KEY,
            "Content-Type": ct,
            "Accept": "application/json",
            "User-Agent": "muse-stt-proxy",
        },
    )
    try:
        with urlopen(req, timeout=30) as r:
            raw = r.read().decode("utf-8", "replace")
            status = r.status
    except Exception as e:
        print("azure stt fail", e, file=sys.stderr)
        return {"ok": False, "error": "upstream", "detail": str(e)}
    try:
        doc = json.loads(raw) if raw else {}
    except Exception:
        return {"ok": False, "error": "bad_json", "http": status, "raw": raw[:200]}
    text = (doc.get("DisplayText") or "").strip()
    # detailed format may put text in NBest
    if not text and isinstance(doc.get("NBest"), list) and doc["NBest"]:
        text = (doc["NBest"][0].get("Display") or doc["NBest"][0].get("Lexical") or "").strip()
    recognition = doc.get("RecognitionStatus") or ""
    ok = bool(text) and recognition in ("Success", "SuccessWithLimitedDetail", "")
    if recognition == "Success" and text:
        ok = True
    return {
        "ok": ok,
        "text": text,
        "status": recognition or ("ok" if text else "empty"),
        "language": lang,
        "http": status,
    }


class TtsHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        path = urlparse(self.path).path
        if path in ("/health", "/"):
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(
                json.dumps(
                    {
                        "service": "muse-tts",
                        "voice": VOICE,
                        "style": STYLE or None,
                        "rate": PROSODY_RATE,
                        "pitch": PROSODY_PITCH,
                        "stt_port": STT_PORT,
                    }
                ).encode()
            )
            return
        if path not in ("/tts", "/tts/"):
            self.send_response(404)
            self.end_headers()
            self.wfile.write(b"not found")
            return
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
        sys.stderr.write("tts %s - %s\n" % (self.address_string(), fmt % args))


class SttHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        path = urlparse(self.path).path
        if path in ("/health", "/", "/stt"):
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(
                json.dumps(
                    {"service": "muse-stt", "language": STT_LANG, "region": REGION}
                ).encode()
            )
            return
        self.send_response(404)
        self.end_headers()

    def do_POST(self):
        path = urlparse(self.path).path
        if path not in ("/stt", "/stt/"):
            self.send_response(404)
            self.end_headers()
            self.wfile.write(b"not found")
            return
        try:
            length = int(self.headers.get("Content-Length") or "0")
        except ValueError:
            length = 0
        if length <= 0 or length > STT_MAX_BYTES:
            self.send_response(400)
            self.end_headers()
            self.wfile.write(b"bad content-length")
            return
        audio = self.rfile.read(length)
        ct = self.headers.get("Content-Type") or "audio/wav"
        # strip charset etc
        ct = ct.split(";")[0].strip()
        result = recognize_wav(audio, ct)
        body = json.dumps(result, ensure_ascii=False).encode("utf-8")
        code = 200 if result.get("ok") else 502
        if result.get("error") == "empty_audio":
            code = 400
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        sys.stderr.write("stt %s - %s\n" % (self.address_string(), fmt % args))


def main():
    if not KEY:
        print("WARNING: AZURE_SPEECH_KEY empty", file=sys.stderr)
    print(
        "Muse TTS Azure voice=%s style=%r rate=%s pitch=%s on http://%s:%d/tts?q="
        % (VOICE, STYLE or None, PROSODY_RATE, PROSODY_PITCH, HOST, TTS_PORT)
    )
    print(
        "Muse STT Azure lang=%s region=%s on http://%s:%d/stt"
        % (STT_LANG, REGION, HOST, STT_PORT)
    )
    tts_srv = ThreadingHTTPServer((HOST, TTS_PORT), TtsHandler)
    stt_srv = ThreadingHTTPServer((HOST, STT_PORT), SttHandler)
    t = threading.Thread(target=stt_srv.serve_forever, name="stt", daemon=True)
    t.start()
    try:
        tts_srv.serve_forever()
    finally:
        stt_srv.shutdown()


if __name__ == "__main__":
    main()
