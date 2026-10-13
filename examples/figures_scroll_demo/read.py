"""Reset the board by RTS and print its log: read.py PORT SECONDS [STOP], STOP being the start of the line to end at."""
import sys, time, serial
port, seconds = sys.argv[1], float(sys.argv[2])
s = serial.Serial(port, 115200, timeout=0.2)
s.dtr = False
s.rts = True
time.sleep(0.15)
s.rts = False
end = time.time() + seconds
buf = b''
while time.time() < end:
    buf += s.read(4096)
    while b'\n' in buf:
        line, buf = buf.split(b'\n', 1)
        text = line.decode('utf-8', 'replace').rstrip()
        print(text, flush=True)
        if text.startswith(sys.argv[3] if len(sys.argv) > 3 else '\0'):
            sys.exit(0)
