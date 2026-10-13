import serial, time, sys, os
port, baud, out, secs = sys.argv[1], int(sys.argv[2]), sys.argv[3], float(sys.argv[4])
errlog = out + '.err'
try:
    ser = serial.Serial(port, baud, timeout=0.2)
except Exception as e:
    open(errlog,'w').write(repr(e)); print('OPEN FAIL', e); sys.exit(1)
ser.reset_input_buffer()
t0 = time.time()
n=0
with open(out, 'wb') as f:
    f.write(b'=== CAPTURE START ===\n'); f.flush()
    while time.time() - t0 < secs:
        w = ser.in_waiting
        if w:
            b = ser.read(w); f.write(b); f.flush(); n += len(b)
        else:
            time.sleep(0.05)
    f.write(b'\n=== CAPTURE END ===\n')
ser.close()
print('wrote', out, 'bytes', n, 'file', os.path.getsize(out))
