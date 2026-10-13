import serial, time, sys
port = "COM3"
out = r"C:\esp\muse-gadget-sdk\esp32\boot_check_keeplink.txt"
try:
    ser = serial.Serial(port, 115200, timeout=0.2)
except Exception as e:
    print("OPEN_FAIL", e)
    sys.exit(1)
# brief DTR toggle to nudge reboot once, then listen
ser.dtr = False
ser.rts = False
time.sleep(0.05)
buf = b""
end = time.time() + 16
while time.time() < end:
    chunk = ser.read(4096)
    if chunk:
        buf += chunk
    else:
        time.sleep(0.05)
ser.close()
text = buf.decode("utf-8", errors="replace")
open(out, "w", encoding="utf-8", errors="replace").write(text)
print("BYTES", len(buf))
keys = ("wifi", "WiFi", "muse", "Muse", "token", "WG", "wireguard", "SCREEN", "nap", "sleep", "keep", "Battery", "battery", "BQ", "paired", "ws", "raw", "CONFIG", "offline", "online", "TLS")
for line in text.splitlines():
    low = line.lower()
    if any(k.lower() in low for k in keys):
        print(line[:240])
