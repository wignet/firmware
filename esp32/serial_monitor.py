#!/usr/bin/env python3
import serial
import time

try:
    ser = serial.Serial('/dev/cu.usbserial-1410', 115200, timeout=1)
    print("Connecté au port série. Appuyez sur Ctrl+C pour quitter.\n")
    time.sleep(2)

    while True:
        if ser.in_waiting > 0:
            line = ser.readline().decode('utf-8', errors='ignore').rstrip()
            print(line)
        time.sleep(0.1)
except KeyboardInterrupt:
    print("\nArrêt du moniteur série")
except Exception as e:
    print(f"Erreur: {e}")
finally:
    if 'ser' in locals():
        ser.close()
