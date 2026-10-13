from pathlib import Path
import re

k = Path(r'C:\esp\muse-gadget-sdk\esp32\components\muse\Kconfig')
kt = k.read_text(encoding='utf-8')
if 'MUSE_TTS_HTTP' not in kt:
    insert = '''
    config MUSE_TTS_HTTP
        bool "Speak replies via HTTP TTS (MP3)"
        depends on MUSE_HATCH
        default y
        help
            When the speaker is on, start_tts fetches an MP3 from the TTS URL
            (reply text URL-encoded) and plays it through the existing minimp3
            path. Speaker off keeps caption silence pacing. Muse/Hatch has no
            built-in TTS; this uses an external HTTP endpoint (default: Youdao
            dictvoice, good for Chinese). Change MUSE_TTS_URL_ZH/EN if needed.

    config MUSE_TTS_URL_ZH
        string "TTS URL prefix for Chinese text"
        depends on MUSE_TTS_HTTP
        default "https://dict.youdao.com/dictvoice?type=0&audio="
        help
            Prefix only; the URL-encoded reply text is appended. Must return MP3.

    config MUSE_TTS_URL_EN
        string "TTS URL prefix for non-CJK text"
        depends on MUSE_TTS_HTTP
        default "https://dict.youdao.com/dictvoice?type=1&audio="
        help
            Used when the reply has no CJK. Must return MP3.

'''
    idx = kt.rfind('\nendmenu')
    if idx < 0:
        raise SystemExit('endmenu not found')
    kt = kt[:idx] + '\n' + insert + kt[idx:]
    k.write_text(kt, encoding='utf-8', newline='\n')
    print('Kconfig: added MUSE_TTS_*')
else:
    print('Kconfig: already has MUSE_TTS_HTTP')

def ensure_lines(path, lines_to_add):
    p = Path(path)
    if not p.exists():
        print('skip missing', path)
        return
    t = p.read_text(encoding='utf-8')
    changed = False
    for line in lines_to_add:
        key = line.split('=')[0]
        if not key.startswith('CONFIG_'):
            continue
        pat = re.compile(r'^#?\s*' + re.escape(key) + r'.*$', re.M)
        if pat.search(t):
            newt = pat.sub(line, t)
            if newt != t:
                t = newt
                changed = True
        else:
            t = t.rstrip() + '\n' + line + '\n'
            changed = True
    if changed:
        p.write_text(t, encoding='utf-8', newline='\n')
        print('updated', path)
    else:
        print('ok', path)

cjk_tts = [
    'CONFIG_MUSE_CJK_FONT=y',
    'CONFIG_MUSE_TTS_HTTP=y',
]
ensure_lines(r'C:\esp\muse-gadget-sdk\esp32\devices\sdkconfig.muse-waveshare-s3-185b', cjk_tts)
ensure_lines(r'C:\esp\muse-gadget-sdk\esp32\devices\sdkconfig.wg.local', cjk_tts)
ensure_lines(r'C:\esp\muse-gadget-sdk\esp32\build-muse-waveshare-s3-185b\sdkconfig', [
    'CONFIG_MUSE_CJK_FONT=y',
    'CONFIG_MUSE_TTS_HTTP=y',
    'CONFIG_MUSE_TTS_URL_ZH="https://dict.youdao.com/dictvoice?type=0&audio="',
    'CONFIG_MUSE_TTS_URL_EN="https://dict.youdao.com/dictvoice?type=1&audio="',
])
print('config patches done')
