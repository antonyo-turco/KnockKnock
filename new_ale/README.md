# ADXL362 Serial Plotter - Heltec V4 (ESP32-S3)

Programma per leggere continuamente l'accelerometro ADXL362 sulla Heltec V4 e stampare i dati in formato CSV per Serial Plotter.

## Hardware

**Heltec V4 (ESP32-S3) Pinout:**

```
ADXL362          Heltec V4
──────────────────────────
VDD     ──→  3.3V
GND     ──→  GND
MOSI    ──→  GPIO 10
MISO    ──→  GPIO 11
SCLK    ──→  GPIO 9
CS      ──→  GPIO 8
INT1    ──→  GPIO 14 (opzionale)
```

## Setup

### 1. Installa PlatformIO
```bash
pip install platformio
```

### 2. Compila il Progetto
```bash
cd /Users/alessandrococcia/Desktop/IoT/KnockKnock/new_ale
pio run -e heltec_v4
```

### 3. Identifica la Porta USB
```bash
ls -la /dev/tty.* | grep -i usb
# Risultato: /dev/tty.SLAB_USBtoUART (o simile)
```

### 4. Flashare il Dispositivo
```bash
pio run -e heltec_v4 -t upload
```

### 5. Monitor Seriale
```bash
pio device monitor -e heltec_v4
```

O apri **Arduino IDE → Strumenti → Serial Plotter** per visualizzare i grafici.

## Output

Il programma stampa i dati in formato CSV:
```
X,Y,Z
245,98,-1002
244,97,-1001
246,99,-1003
...
```

## Troubleshooting

Se non rileva l'accelerometro, controlla:
1. Collegamento SPI (MOSI, MISO, SCK)
2. CS collegato a GPIO 8
3. VDD a 3.3V e GND correttamente collegati
4. Prova a aumentare il tempo di attesa: `vTaskDelay(pdMS_TO_TICKS(200))`

## Build Flags

Configurabili in `platformio.ini`:
- `-DBOARD_HAS_PSRAM`: Usa PSRAM se disponibile
- `-mfix-esp32-psram-cache-issue`: Fix cache PSRAM

