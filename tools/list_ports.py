import serial.tools.list_ports

ports = serial.tools.list_ports.comports()

if not ports:
    print("No serial ports found.")
else:
    for p in ports:
        print("DEVICE:", p.device)
        print("NAME:", p.name)
        print("DESCRIPTION:", p.description)
        print("HWID:", p.hwid)
        print("-" * 40)
