#!/usr/bin/env python3
"""
capture.py  —  Cattura il CSV dall'ESP32 e lo salva su file

Uso:
    python capture.py --port /dev/ttyUSB0 --out baseline_01.csv --duration 300

    --port      porta seriale (Mac: /dev/cu.usbserial-*, Win: COM3, Linux: /dev/ttyUSB0)
    --out       file di output  [default: capture.csv]
    --duration  secondi di registrazione, 0 = infinito fino a Ctrl+C  [default: 60]
    --baud      baud rate  [default: 921600]
"""

import argparse
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:
    print("Installa pyserial:  pip install pyserial")
    sys.exit(1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port",     required=True)
    parser.add_argument("--out",      default="capture.csv")
    parser.add_argument("--duration", type=float, default=60)
    parser.add_argument("--baud",     type=int,   default=921600)
    args = parser.parse_args()

    out_path = Path(args.out)
    print(f"Porta  : {args.port}  @{args.baud} baud")
    print(f"Output : {out_path.resolve()}")
    print(f"Durata : {args.duration if args.duration > 0 else '∞'} secondi")
    print("Avvio — premi Ctrl+C per interrompere\n")

    t0      = time.time()
    n_lines = 0

    with serial.Serial(args.port, args.baud, timeout=1) as ser, \
         open(out_path, "w", encoding="utf-8") as fout:

        # Svuota il buffer in ingresso
        ser.reset_input_buffer()
        time.sleep(0.2)

        while True:
            if args.duration > 0 and (time.time() - t0) >= args.duration:
                break

            raw = ser.readline()
            if not raw:
                continue

            try:
                line = raw.decode("ascii", errors="replace").strip()
            except Exception:
                continue

            if not line:
                continue

            fout.write(line + "\n")
            n_lines += 1

            # Feedback ogni 4000 righe (~10s a 400Hz)
            if n_lines % 4000 == 0:
                elapsed = time.time() - t0
                print(f"  {n_lines:7d} righe  |  {elapsed:6.1f}s  |  {n_lines/elapsed:.0f} Hz effettivi")

    print(f"\nFine. {n_lines} righe salvate in {out_path}")


if __name__ == "__main__":
    main()
