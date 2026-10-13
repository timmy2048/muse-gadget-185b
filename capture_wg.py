import serial, time, sys
port = "COM3"
baud = 115200
out = r"C:\esp\muse-gadget-sdk\wg_serial.log"
deadline = time.time() + 50
keywords = ("muse.wg", "peer up", "tunnel default", "handshake", "SNTP", "got IP", "api.muse", "fetch_vms", "wg=", "WireGuard", "esp_wireguard", "TLS", "timeout", "error")
ser = serial.Serial(port, baud, timeout=0.2)
# toggle DTR/RTS for reset
ser.dtr = False; ser.rts = True; time.sleep(0.1)
ser.rts = False; ser.dtr = True; time.sleep(0.1)
buf = b""
lines = []
interesting = []
with open(out, "wb") as f:
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            f.write(chunk); f.flush()
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                try:
                    s = line.decode("utf-8", "replace").rstrip()
                except Exception:
                    s = repr(line)
                lines.append(s)
                low = s.lower()
                if any(k.lower() in low for k in keywords):
                    interesting.append(s)
ser.close()
print(f"lines={len(lines)} interesting={len(interesting)}")
for s in interesting[-80:]:
    # redact private key if any
    if "PRIVATE_KEY" in s or "PrivateKey" in s:
        continue
    print(s)
