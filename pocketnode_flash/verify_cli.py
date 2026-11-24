import time, serial

PORT, BAUD = "COM17", 115200

def drain(ser, seconds, label):
    end = time.time() + seconds
    buf = b""
    while time.time() < end:
        n = ser.in_waiting
        if n:
            buf += ser.read(n)
        else:
            time.sleep(0.05)
    text = buf.decode("utf-8", "replace")
    print(f"===== {label} ({len(buf)} bytes) =====")
    print(text if text.strip() else "<no data>")
    return text

ser = serial.Serial()
ser.port = PORT
ser.baudrate = BAUD
ser.timeout = 0.2
ser.dsrdtr = False
ser.rtscts = False
ser.open()
# Do not hold the board in reset via the auto-reset lines
ser.dtr = False
ser.rts = False
ser.reset_input_buffer()
print(f"Opened {PORT} @ {BAUD}, waiting 2 s for boot...")
time.sleep(2.0)

boot = drain(ser, 2.0, "BOOT / STARTUP OUTPUT")

ser.write(b"\r\nhelp\r\n")
ser.flush()
drain(ser, 4.0, "RESPONSE TO: help")

ser.write(b"\r\ninfo\r\n")
ser.flush()
drain(ser, 4.0, "RESPONSE TO: info")

ser.close()
