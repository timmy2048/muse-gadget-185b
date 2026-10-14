#!/usr/bin/env python3
"""Muse Gadget WireGuard proxy: Azure TTS + STT + translate.

Binds 10.0.0.1 only (WG). Secrets from muse-tts.env.
  TTS:       GET  http://10.0.0.1:8787/tts?q=&lang=zh|en
  Translate: GET  http://10.0.0.1:8787/translate?q=&from=&to=
             POST http://10.0.0.1:8787/translate  JSON {q,from,to}
  STT:       POST http://10.0.0.1:8788/stt?lang=zh-CN|en-US|auto

Speech key stays on VPS. Translator: Azure Translator if AZURE_TRANSLATOR_KEY
(or multi-service key) works; else Google gtx fallback (ZH/EN).
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs, quote
from urllib.request import Request, urlopen
from xml.sax.saxutils import escape
import json
import os
import re
import sys
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed

HOST = os.environ.get("MUSE_TTS_HOST", "10.0.0.1")
TTS_PORT = int(os.environ.get("MUSE_TTS_PORT", "8787"))
STT_PORT = int(os.environ.get("MUSE_STT_PORT", "8788"))
REGION = os.environ.get("AZURE_SPEECH_REGION", "eastus").strip()
KEY = os.environ.get("AZURE_SPEECH_KEY", "").strip()
TRANSLATOR_KEY = os.environ.get("AZURE_TRANSLATOR_KEY", KEY).strip()
TRANSLATOR_REGION = os.environ.get("AZURE_TRANSLATOR_REGION", REGION).strip()
VOICE_ZH = os.environ.get("AZURE_SPEECH_VOICE", "zh-CN-XiaoxiaoNeural").strip()
VOICE_EN = os.environ.get("AZURE_SPEECH_VOICE_EN", "en-US-JennyNeural").strip()
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


def build_ssml(text: str, lang: str = "zh") -> str:
    body = _punct_to_ssml(text)
    is_en = lang.lower().startswith("en")
    voice = VOICE_EN if is_en else VOICE_ZH
    xml_lang = "en-US" if is_en else "zh-CN"
    voice_name = escape(voice, {"'": "&apos;"})
    inner = (
        "<prosody rate='%s' pitch='%s'>%s</prosody>"
        % (escape(PROSODY_RATE, {"'": "&apos;"}), escape(PROSODY_PITCH, {"'": "&apos;"}), body)
    )
    if STYLE and not is_en:
        inner = (
            "<mstts:express-as style='%s' styledegree='%s'>%s</mstts:express-as>"
            % (escape(STYLE, {"'": "&apos;"}), escape(STYLE_DEGREE, {"'": "&apos;"}), inner)
        )
    return (
        "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' "
        "xmlns:mstts='https://www.w3.org/2001/mstts' xml:lang='%s'>"
        "<voice xml:lang='%s' name='%s'>%s</voice>"
        "</speak>"
    ) % (xml_lang, xml_lang, voice_name, inner)


def fetch_mp3(text: str, lang: str = "zh") -> bytes:
    text = (text or "").strip()[:MAX_CHARS]
    if not text:
        return b""
    if not KEY:
        print("missing AZURE_SPEECH_KEY", file=sys.stderr)
        return b""
    ssml = build_ssml(text, lang)
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


def recognize_wav(audio: bytes, content_type: str, lang: str) -> dict:
    if not KEY:
        return {"ok": False, "error": "missing_key"}
    if not audio:
        return {"ok": False, "error": "empty_audio"}
    url = (
        "https://%s.stt.speech.microsoft.com/speech/recognition/conversation/"
        "cognitiveservices/v1?language=%s&format=detailed"
        % (REGION, lang)
    )
    ct = (content_type or "audio/wav").split(";")[0].strip()
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
        return {"ok": False, "error": "upstream", "detail": str(e), "language": lang}
    try:
        doc = json.loads(raw) if raw else {}
    except Exception:
        return {"ok": False, "error": "bad_json", "http": status, "raw": raw[:200], "language": lang}
    text = (doc.get("DisplayText") or "").strip()
    conf = 0.0
    if isinstance(doc.get("NBest"), list) and doc["NBest"]:
        best = doc["NBest"][0]
        if not text:
            text = (best.get("Display") or best.get("Lexical") or "").strip()
        try:
            conf = float(best.get("Confidence") or 0.0)
        except (TypeError, ValueError):
            conf = 0.0
    recognition = doc.get("RecognitionStatus") or ""
    ok = bool(text) and recognition in ("Success", "SuccessWithLimitedDetail", "")
    if recognition == "Success" and text:
        ok = True
    return {
        "ok": ok,
        "text": text,
        "status": recognition or ("ok" if text else "empty"),
        "language": lang,
        "confidence": conf,
        "http": status,
    }


_CJK_RE = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff]")


def looks_cjk(text: str) -> bool:
    if not text:
        return False
    cjk = len(_CJK_RE.findall(text))
    latin = len(re.findall(r"[A-Za-z]", text))
    return cjk >= max(1, latin)


def recognize_auto(audio: bytes, content_type: str) -> dict:
    """Run zh-CN + en-US in parallel; pick higher confidence, tie-break by script."""
    results = {}
    with ThreadPoolExecutor(max_workers=2) as pool:
        futs = {
            pool.submit(recognize_wav, audio, content_type, lang): lang
            for lang in ("zh-CN", "en-US")
        }
        for fut in as_completed(futs):
            results[futs[fut]] = fut.result()
    zh = results.get("zh-CN") or {"ok": False, "confidence": 0, "text": ""}
    en = results.get("en-US") or {"ok": False, "confidence": 0, "text": ""}
    candidates = [r for r in (zh, en) if r.get("ok") and r.get("text")]
    if not candidates:
        # return whichever has more detail
        pick = zh if (zh.get("text") or "") else en
        pick = dict(pick)
        pick["ok"] = False
        pick["detected"] = "auto"
        return pick
    # Prefer matching script when confidences are close
    def score(r):
        conf = float(r.get("confidence") or 0)
        text = r.get("text") or ""
        lang = r.get("language") or ""
        bonus = 0.15 if (lang.startswith("zh") and looks_cjk(text)) else 0
        bonus += 0.15 if (lang.startswith("en") and not looks_cjk(text)) else 0
        return conf + bonus

    pick = max(candidates, key=score)
    out = dict(pick)
    out["detected"] = "zh" if str(out.get("language", "")).startswith("zh") else "en"
    out["ok"] = True
    return out


def azure_translate(text: str, frm: str, to: str) -> str | None:
    if not TRANSLATOR_KEY or not text:
        return None
    # Normalize Azure Translator language tags
    def norm(code: str) -> str:
        c = (code or "").lower()
        if c.startswith("zh"):
            return "zh-Hans"
        if c.startswith("en"):
            return "en"
        return code or "en"

    body = json.dumps([{"Text": text[:500]}]).encode("utf-8")
    url = (
        "https://api.cognitive.microsofttranslator.com/translate"
        "?api-version=3.0&from=%s&to=%s" % (norm(frm), norm(to))
    )
    req = Request(
        url,
        data=body,
        method="POST",
        headers={
            "Ocp-Apim-Subscription-Key": TRANSLATOR_KEY,
            "Ocp-Apim-Subscription-Region": TRANSLATOR_REGION,
            "Content-Type": "application/json",
            "User-Agent": "muse-translate-proxy",
        },
    )
    try:
        with urlopen(req, timeout=15) as r:
            doc = json.loads(r.read().decode("utf-8", "replace"))
        return doc[0]["translations"][0]["text"]
    except Exception as e:
        print("azure translate fail", e, file=sys.stderr)
        return None


def gtx_translate(text: str, frm: str, to: str) -> str | None:
    def norm(code: str) -> str:
        c = (code or "").lower()
        if c.startswith("zh"):
            return "zh-CN"
        if c.startswith("en"):
            return "en"
        return code or "auto"

    url = (
        "https://translate.googleapis.com/translate_a/single"
        "?client=gtx&sl=%s&tl=%s&dt=t&q=%s"
        % (norm(frm), norm(to), quote(text[:500]))
    )
    try:
        with urlopen(url, timeout=12) as r:
            doc = json.loads(r.read().decode("utf-8", "replace"))
        parts = []
        for row in doc[0] or []:
            if row and row[0]:
                parts.append(row[0])
        out = "".join(parts).strip()
        return out or None
    except Exception as e:
        print("gtx translate fail", e, file=sys.stderr)
        return None


def do_translate(text: str, frm: str, to: str) -> dict:
    text = (text or "").strip()
    if not text:
        return {"ok": False, "error": "empty"}
    if not frm or frm == "auto":
        frm = "zh-Hans" if looks_cjk(text) else "en"
    if not to:
        to = "en" if str(frm).startswith("zh") else "zh-Hans"
    translated = azure_translate(text, frm, to)
    engine = "azure"
    if not translated:
        translated = gtx_translate(text, frm, to)
        engine = "gtx"
    if not translated:
        return {"ok": False, "error": "upstream", "from": frm, "to": to}
    return {
        "ok": True,
        "text": translated,
        "source": text,
        "from": frm,
        "to": to,
        "engine": engine,
    }


class TtsHandler(BaseHTTPRequestHandler):
    def _json(self, code: int, obj: dict):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        q = parse_qs(parsed.query)
        if path in ("/health", "/"):
            self._json(
                200,
                {
                    "service": "muse-tts",
                    "voice_zh": VOICE_ZH,
                    "voice_en": VOICE_EN,
                    "style": STYLE or None,
                    "stt_port": STT_PORT,
                    "translate": True,
                },
            )
            return
        if path in ("/translate", "/translate/"):
            text = (q.get("q") or q.get("text") or [""])[0]
            frm = (q.get("from") or q.get("sl") or ["auto"])[0]
            to = (q.get("to") or q.get("tl") or [""])[0]
            result = do_translate(text, frm, to)
            self._json(200 if result.get("ok") else 502, result)
            return
        if path not in ("/tts", "/tts/"):
            self.send_response(404)
            self.end_headers()
            self.wfile.write(b"not found")
            return
        text = (q.get("q") or q.get("text") or [""])[0]
        lang = (q.get("lang") or q.get("voice") or ["zh"])[0]
        data = fetch_mp3(text, lang)
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

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path
        if path not in ("/translate", "/translate/"):
            self.send_response(404)
            self.end_headers()
            return
        try:
            length = int(self.headers.get("Content-Length") or "0")
        except ValueError:
            length = 0
        raw = self.rfile.read(max(0, min(length, 8192))) if length > 0 else b"{}"
        try:
            doc = json.loads(raw.decode("utf-8", "replace") or "{}")
        except Exception:
            self._json(400, {"ok": False, "error": "bad_json"})
            return
        result = do_translate(
            doc.get("q") or doc.get("text") or "",
            doc.get("from") or doc.get("sl") or "auto",
            doc.get("to") or doc.get("tl") or "",
        )
        self._json(200 if result.get("ok") else 502, result)

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
                    {
                        "service": "muse-stt",
                        "language": STT_LANG,
                        "region": REGION,
                        "auto": True,
                    }
                ).encode()
            )
            return
        self.send_response(404)
        self.end_headers()

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path
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
        q = parse_qs(parsed.query)
        lang = (q.get("lang") or q.get("language") or [STT_LANG])[0].strip() or STT_LANG
        if lang.lower() in ("auto", "detect", "zh,en", "en,zh"):
            result = recognize_auto(audio, ct)
        else:
            result = recognize_wav(audio, ct, lang)
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
        "Muse TTS Azure voice_zh=%s voice_en=%s style=%r on http://%s:%d/tts?q="
        % (VOICE_ZH, VOICE_EN, STYLE or None, HOST, TTS_PORT)
    )
    print(
        "Muse translate on http://%s:%d/translate" % (HOST, TTS_PORT)
    )
    print(
        "Muse STT Azure default=%s region=%s auto on http://%s:%d/stt?lang=auto"
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
