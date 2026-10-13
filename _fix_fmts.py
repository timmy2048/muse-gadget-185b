from pathlib import Path
p = Path(r"C:\esp\muse-gadget-sdk\esp32\components\muse\muse_chat_session.cpp")
lines = p.read_text(encoding="utf-8").splitlines()
# Fix lines that have broken ""msg" patterns
for i, l in enumerate(lines):
    if 'muse_hatch_console("final"' in l and '""msg"' in l:
        lines[i] = '            muse_hatch_console("final", final_text, "\\"msg\\":%d", i);'
        print("fixed final", i+1)
    if 'muse_hatch_console("message_done"' in l and '""msg"' in l:
        lines[i] = '        muse_hatch_console("message_done", nullptr, "\\"msg\\":%d,\\"bytes\\":%u", i, (unsigned)n);'
        print("fixed message_done", i+1)
p.write_text("\n".join(lines) + "\n", encoding="utf-8")
# verify
for i,l in enumerate(lines):
    if i+1 in (1387,1389,1473):
        print(i+1, l)
