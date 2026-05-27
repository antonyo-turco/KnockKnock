# ADXL362 + Server WiFi - Setup Guida

## 📋 Configurazione

### 1. Modifica il file `.env`

Apri `new_ale/.env` e inserisci i tuoi dati:

```bash
WIFI_SSID=TuoSSID
WIFI_PASSWORD=TuaPassword
SERVER_IP=192.168.1.100  # IP del tuo computer
SERVER_PORT=5010
TIMEZONE=UTC+2           # Modifica secondo il tuo fuso orario
```

### 2. Configura `config.h`

Apri `new_ale/src/config.h` e aggiorna i valori da `.env`:

```cpp
#define WIFI_SSID "TuoSSID"
#define WIFI_PASSWORD "TuaPassword"
#define SERVER_IP "192.168.1.100"
#define SERVER_PORT 5010
#define TZ_OFFSET 2  // UTC+2
```

### 3. Avvia il server Docker

```bash
cd /Users/alessandrococcia/Desktop/IoT/KnockKnock/server
docker-compose down
docker-compose up --build
```

### 4. Compila e flashiamo l'ESP32

```bash
cd /Users/alessandrococcia/Desktop/IoT/KnockKnock/new_ale
pio run -e heltec_v4 -t upload
```

### 5. Serial Monitor

```bash
pio device monitor -p /dev/cu.usbmodem1101 -b 115200
```

Dovresti vedere:
```
✓ WiFi connected!
✓ Time synced: ...
Ready to accumulate data...

=== Sending batch ===
✓ Sent 6000 samples (code: 201)
=== Ready for next batch ===
```

## 📊 API Endpoints

### Leggi ultimi dati (ordine cronologico)
```bash
curl "http://localhost:5010/api/data?limit=100&order=asc"
```

### Leggi in ordine inverso
```bash
curl "http://localhost:5010/api/data?limit=100&order=desc"
```

### Stats del database
```bash
curl http://localhost:5010/api/stats
```

Ritorna:
```json
{
  "total_measurements": 6000,
  "first_measurement": "2026-05-09T12:00:00+00:00",
  "last_measurement": "2026-05-09T12:01:00+00:00"
}
```

### Health check
```bash
curl http://localhost:5010/health
```

## 🗄️ Database

I dati sono salvati in `/data/accelerometer.db` con:
- **timestamp**: Data/ora sincronizzata via NTP
- **x, y, z**: Valori accelerometro

I dati sono ordinati cronologicamente per timestamp.

## 🔧 Troubleshooting

### Server non raggiungibile
- Verifica che ESP32 e computer siano sulla stessa rete WiFi
- Usa `ifconfig` per trovare l'IP del computer
- Testa con `curl http://IP:5010/health`

### NTP non sincronizza
- Controlla che l'ESP32 abbia connessione WiFi
- Verifica il fuso orario in `config.h`
- Controlla la connettività NTP dal tuo network

### Porta 5010 già occupata
- Modifica `docker-compose.yml`: cambia `5010:5010` con una porta libera
- Aggiorna `config.h` con la nuova porta
