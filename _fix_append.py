from pathlib import Path
p = Path(r'C:\esp\muse-gadget-sdk\esp32\components\muse\muse_chat_session.cpp')
lines = p.read_text(encoding='utf-8').splitlines()
changed = 0
for i,l in enumerate(lines):
    if 'm.len += strlen(text);' in l and i > 0 and 's_turn.text' in lines[i-2]:
        # replace this line with append_text
        lines[i] = '            append_text(m, text);   /* fill texts[] so start_tts can speak */'
        changed += 1
        print('replaced line', i+1)
p.write_text('\n'.join(lines)+'\n', encoding='utf-8', newline='\n')
print('changed', changed)
