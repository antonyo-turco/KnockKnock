# ADXL362 + Server Docker

## Setup del server con Docker

### 1. Avvia il server

```bash
cd /Users/alessandrococcia/Desktop/IoT/KnockKnock/server
docker-compose up --build
```

Il server partirà su `http://localhost:5000`

### 2. Configura l'ESP32

Modifica `new_ale/src/main.cpp`:
- Sostituisci `YOUR_SSID` con il tuo WiFi SSID
- Sostituisci `YOUR_PASSWORD` con la tua password WiFi
- Sostituisci `YOUR_SERVER_IP` con l'indirizzo IP del tuo computer (es: `192.168.1.100`)

**Come trovare l'IP del computer:**
```bash
ifconfig | grep "inet " | grep -v 127.0.0.1
```

### 3. Compila e flashiamo l'ESP32

```bash
cd /Users/alessandrococcia/Desktop/IoT/KnockKnock/new_ale
pio run -e heltec_v4 -t upload
```

### 4. API Endpoints

**POST /api/data** - Invia un nuovo dato
```bash
curl -X POST http://localhost:5000/api/data \
  -H "Content-Type: application/json" \
  -d '{"x": 10.5, "y": -5.2, "z": 1000.1}'
```

**GET /api/data** - Leggi gli ultimi dati
```bash
curl http://localhost:5000/api/data?limit=10
```

### 5. Il database SQLite

I dati vengono salvati in `/data/accelerometer.db`. Accedi con:
```bash
docker exec -it server_accelerometer-server_1 sqlite3 /data/accelerometer.db
sqlite> SELECT * FROM measurements LIMIT 10;
```

## Struttura

```
server/
├── app.py              # Server Flask
├── requirements.txt    # Dipendenze Python
├── Dockerfile         # Container config
└── docker-compose.yml # Orchestrazione
```

## Cosa succede

1. ESP32 legge l'accelerometro a 100 Hz (ogni 10ms)
2. Invia i dati via HTTP POST al server
3. Server salva X, Y, Z con timestamp nel database SQLite
4. Puoi leggere i dati via GET /api/data
