import time
import serial

PORT = "COM3"
BAUD = 115200

print(f"Opening {PORT} ...")
ser = serial.Serial(PORT, BAUD, timeout=0.5)

# Το Arduino συνήθως κάνει reset μόλις ανοίξει η serial
time.sleep(2.5)

try:
    ser.reset_input_buffer()
except Exception:
    pass

cmd = "PING\n"
print("Sending:", cmd.strip())
ser.write(cmd.encode("utf-8"))
ser.flush()

t0 = time.time()
while time.time() - t0 < 8:
    line = ser.readline()
    if line:
        text = line.decode("utf-8", errors="ignore").strip()
        print("RECV:", text)

ser.close()
print("Closed.")
