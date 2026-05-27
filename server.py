#!/usr/bin/env python3
"""
server.py  —  Server TCP per raccolta dati ADXL362 via WiFi

Riceve il CSV dall'ESP32 e lo salva su file, mostrando statistiche live.

Uso:
    python server.py                          # porta 9876, file auto-nominato
    python server.py --port 9876 --out dati.csv
    python server.py --duration 21600         # ferma dopo 6h

Trova il tuo IP locale:
    Mac/Linux:  ifconfig | grep "inet "
    Windows:    ipconfig  (cerca "IPv4")
Poi mettilo in SERVER_IP nello sketch.
"""

import argparse
import socket
import sys
import time
from datetime import datetime
from pathlib import Path

def format_duration(seconds: float) -> str:
    h = int(seconds) // 3600
    m = (int(seconds) % 3600) // 60
    s = int(seconds) % 60
    return f"{h:02d}:{m:02d}:{s:02d}"


def format_rate(n: int, elapsed: float) -> str:
    if elapsed < 0.1:
        return "---"
    return f"{n / elapsed:.1f}"


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port",     type=int,   default=9876,
                        help="Porta TCP in ascolto (default: 9876)")
    parser.add_argument("--out",      default="",
                        help="File CSV di output (default: capture_YYYYMMDD_HHMMSS.csv)")
    parser.add_argument("--duration", type=float, default=0,
                        help="Durata massima in secondi (default: 0 = infinito)")
    args = parser.parse_args()

    out_path = Path(args.out) if args.out else \
               Path(f"capture_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv")

    # ── Trova IP locale e stampalo ────────────────────────────────────────────
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        local_ip = s.getsockname()[0]
        s.close()
    except Exception:
        local_ip = "127.0.0.1"

    print("╔══════════════════════════════════════════════════════╗")
    print("║        ADXL362 WiFi Data Collector — Server         ║")
    print("╠══════════════════════════════════════════════════════╣")
    print(f"║  IP locale  : {local_ip:<38}║")
    print(f"║  Porta      : {args.port:<38}║")
    print(f"║  Output     : {str(out_path):<38}║")
    dur_str = format_duration(args.duration) if args.duration > 0 else "infinita"
    print(f"║  Durata     : {dur_str:<38}║")
    print("╠══════════════════════════════════════════════════════╣")
    print(f"║  → Metti SERVER_IP \"{local_ip}\" nello sketch     ║")
    print("╚══════════════════════════════════════════════════════╝")
    print()

    # ── Apri server TCP ───────────────────────────────────────────────────────
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(1)
    print(f"In ascolto su 0.0.0.0:{args.port} — accendo il dispositivo ora...")

    conn, addr = srv.accept()
    conn.settimeout(5.0)
    print(f"Connesso: {addr[0]}:{addr[1]}")
    print()

    # ── Ricezione e salvataggio ───────────────────────────────────────────────
    t_start      = time.time()
    t_last_print = t_start
    t_last_hz    = t_start
    n_total      = 0
    n_since_hz   = 0
    hz_eff       = 0.0
    leftover     = ""          # byte parziali tra un recv e l'altro

    PRINT_INTERVAL_S = 2.0     # aggiorna la riga di stato ogni 2s

    with open(out_path, "w", encoding="utf-8") as fout:
        try:
            while True:
                # ── controllo durata ─────────────────────────────────────────
                elapsed = time.time() - t_start
                if args.duration > 0 and elapsed >= args.duration:
                    print(f"\n  Durata raggiunta ({format_duration(elapsed)}) — stop.")
                    break

                # ── ricezione dati ────────────────────────────────────────────
                try:
                    chunk = conn.recv(4096)
                except socket.timeout:
                    print("  Timeout ricezione — dispositivo fermo?", flush=True)
                    continue
                except ConnectionResetError:
                    print("\n  Connessione chiusa dal dispositivo.")
                    break

                if not chunk:
                    print("\n  Connessione chiusa dal dispositivo.")
                    break

                text = leftover + chunk.decode("ascii", errors="replace")
                lines = text.split("\n")
                leftover = lines[-1]    # ultima riga potenzialmente incompleta

                for line in lines[:-1]:
                    line = line.strip()
                    if not line:
                        continue
                    fout.write(line + "\n")
                    n_total    += 1
                    n_since_hz += 1

                # ── calcolo Hz ogni secondo ───────────────────────────────────
                now = time.time()
                dt_hz = now - t_last_hz
                if dt_hz >= 1.0:
                    hz_eff     = n_since_hz / dt_hz
                    n_since_hz = 0
                    t_last_hz  = now

                # ── stampa stato ogni PRINT_INTERVAL_S ───────────────────────
                if now - t_last_print >= PRINT_INTERVAL_S:
                    elapsed = now - t_start
                    size_kb = out_path.stat().st_size / 1024 if out_path.exists() else 0
                    line_out = (
                        f"\r  [{format_duration(elapsed)}]"
                        f"  campioni: {n_total:>9,}"
                        f"  Hz: {hz_eff:>6.1f}"
                        f"  file: {size_kb:>7.0f} KB"
                    )
                    if args.duration > 0:
                        pct = min(100.0, elapsed / args.duration * 100)
                        bar = "█" * int(pct / 5) + "░" * (20 - int(pct / 5))
                        line_out += f"  [{bar}] {pct:>5.1f}%"
                    print(line_out, end="", flush=True)
                    t_last_print = now

        except KeyboardInterrupt:
            print("\n  Interrotto dall'utente.")

    conn.close()
    srv.close()

    elapsed = time.time() - t_start
    size_kb = out_path.stat().st_size / 1024 if out_path.exists() else 0
    print(f"\n\n╔══════════════════════════════════════════════════════╗")
    print(f"║  RIEPILOGO FINALE                                    ║")
    print(f"╠══════════════════════════════════════════════════════╣")
    print(f"║  Campioni   : {n_total:<38,}║")
    print(f"║  Durata     : {format_duration(elapsed):<38}║")
    print(f"║  Hz medi    : {n_total/elapsed:<38.1f}║")
    print(f"║  File       : {str(out_path):<38}║")
    print(f"║  Dimensione : {size_kb:<38.0f} KB║")
    print(f"╚══════════════════════════════════════════════════════╝")

tail -f baseline_6h_rocca.csv
if __name__ == "__main__":
    main()
