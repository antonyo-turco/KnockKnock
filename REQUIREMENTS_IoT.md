# Requirements & Industry Standards

Analisi dei requirement del sistema antifurto TinyML per finestre (ESP32-C3 + ADXL362),
confrontati con standard industriali e prodotti commerciali di riferimento.

---

## REQ-1 — Autonomia batteria ≥ 6 mesi con 2800 mAh

### Standard di riferimento

| Standard | Descrizione |
|---|---|
| [IEC 62133](https://webstore.iec.ch/en/publication/6462) | Sicurezza per batterie portatili ricaricabili |
| [ETSI EN 300 328](https://www.etsi.org/deliver/etsi_en/300300_300399/300328/02.02.02_60/en_300328v020202p.pdf) | Power management per dispositivi radio a 2.4 GHz |

### Prodotti commerciali di riferimento

| Prodotto | Batteria | Autonomia dichiarata | Protocollo |
|---|---|---|---|
| [Aqara Door & Window Sensor](https://www.aqara.com/us/product/door-and-window-sensor/) | CR2450 | **2 anni** | Zigbee 3.0 |
| [Aqara Door & Window Sensor P2](https://us.aqara.com/products/door-and-window-sensor-p2) | CR123A | variabile | Thread / BLE |
| [Ring Alarm Contact Sensor (2nd Gen)](https://ring.com/products/alarm-window-door-contact-sensor-v2) | 2× CR2032 | **3 anni** | Z-Wave |

I prodotti commerciali raggiungono 2-3 anni di autonomia perché usano protocolli a bassissimo
consumo (Zigbee, Z-Wave, Thread) e campionano solo su evento (apertura/chiusura), senza
elaborazione continua.

### Analisi per il progetto

Il firmware attuale campiona a **100 Hz continuamente** per alimentare la pipeline FFT.
Con ESP32-C3 in active mode il consumo stimato è 80 mA, che su 2800 mAh dà circa 35 ore —
incompatibile con 6 mesi.

Le opzioni percorribili sono:

- **Alimentazione da rete** (nessun vincolo batteria) — scelta più semplice per un sensore fisso
- **ULP coprocessor** dell'ESP32-C3 per campionamento a basso consumo (~150 µA), con
  wake del CPU principale solo al superamento di una soglia di ampiezza
- Ridurre il sampling rate o usare un accelerometro con interrupt hardware
  (ADXL362 supporta interrupt su soglia di attività, consumo in standby: 270 nA)

Riferimento tecnico ESP32 sleep modes:
- [ESP32 Deep Sleep — Last Minute Engineers](https://lastminuteengineers.com/esp32-sleep-modes-power-consumption/)
- [Battery life analysis for IoT nodes — Power Electronic Tips](https://www.powerelectronictips.com/battery-life-analysis-and-maximization-for-wireless-iot-sensor-nodes-and-wearables/)

---

## REQ-2 — Allarme entro 5 secondi da un tentativo di furto

### Standard di riferimento

| Standard | Descrizione |
|---|---|
| [EN 50131-1](https://www.alertsystems.co.uk/about/industry-standards/bs-en-intruder-alarm/en-50131/) | Requisiti generali per sistemi antifurto europei |
| [EN 50131-2-6](https://sygma.co.uk/blog/british-standards-for-intruder-alarms/) | Requisiti per detector (sensitivity, false alarm immunity) |
| [BS EN 50131 / PD 6662](https://www.houndsecurity.co.uk/post/bs-en-50131-intruder-alarm-compliance) | Schema di conformità UK — gradi di rischio 1-4 |

EN 50131 classifica i sistemi in 4 gradi di rischio. Per uso residenziale (Grade 2)
il sistema deve rilevare e segnalare l'intrusione in modo affidabile tramite un
Alarm Receiving Centre (ARC). La trasmissione end-to-end ai centri di monitoraggio
certificati avviene tipicamente entro 20-30 secondi.

Il requirement di **5 secondi per l'allarme locale** è più stringente dello standard
europeo ed è realistico con questo firmware: la finestra di campionamento è 2.56 secondi,
quindi il rilevamento avviene entro la finestra successiva all'evento.

### Prodotti commerciali di riferimento

- [Ring Alarm](https://ring.com/products/alarm-window-door-contact-sensor-v2) — notifica istantanea su apertura (event-driven, non campionamento continuo)
- [Aqara Vibration Sensor](https://www.aqara.com/us/product/vibration-sensor/) — rileva vibrazioni e cadute, notifica entro 1-2 secondi

---

## REQ-3 — Nessun falso positivo per voci, animali, veicoli, eventi meteo

### Standard di riferimento

| Standard | Descrizione |
|---|---|
| [EN 50131-5-3](https://elecsec.co.uk/bs-en-50131-our-guide-to-the-british-standard-for-intruder-alarms/) | Motion detector: pattern di rilevamento, sensibilità, immunità ai falsi allarmi |
| [EN 50131-1 §8](https://www.home-security-solutions.com/knowledge-centre/understanding-security-alarm-grades) | Requisiti operativi di affidabilità e riduzione falsi allarmi |

I falsi allarmi hanno un costo operativo reale: nei sistemi professionali UK possono
portare al ritiro della risposta polizia (URN — Unique Reference Number).
Il benchmark industriale per sistemi professionali è **< 1 falso allarme per sistema per anno**.
Per sistemi consumer il target è tipicamente **< 1 per mese**.

Il requirement attuale è qualitativo. Si raccomanda di quantificarlo, ad esempio:

> "Il tasso di falsi positivi deve essere inferiore all'1% delle finestre processate
> in condizioni ambientali normali (traffico, vento, voci)."

---

## REQ-4 — Dashboard con classificazione eventi, granularità 5 secondi per 30 secondi

### Standard di riferimento

| Standard | Descrizione |
|---|---|
| [IEC 61850](https://webstore.iec.ch/en/publication/6028) | Comunicazione per sistemi di automazione — refresh rate eventi critici 1-5 s |
| [EN 50136](https://www.en-standard.eu/bs-en-50136-1-2012-alarm-systems-alarm-transmission-systems-and-equipment-general-requirements-for-alarm-transmission-systems/) | Alarm Transmission Systems — requisiti per la trasmissione degli allarmi |

Non esiste uno standard specifico per dashboard di sicurezza consumer con granularità
temporale. IEC 61850 (sistemi SCADA industriali) prevede refresh rate di 1-5 secondi
per eventi critici — il target di 5 secondi del requirement è allineato con le best
practice industriali.

La granularità di 5 secondi è direttamente legata alla dimensione della finestra:

```
SAMPLE_COUNT / SAMPLING_RATE_HZ = 256 / 100 = 2.56 s per finestra
```

Due finestre consecutive coprono ~5 secondi, quindi il requirement è soddisfatto
by design dalla pipeline esistente.

---

## REQ-5 — Modello ML segreto anche in caso di furto del dispositivo

### Standard di riferimento

| Standard | Descrizione |
|---|---|
| [ETSI EN 303 645](https://www.etsi.org/technologies/consumer-iot-security) | Cybersecurity baseline per IoT consumer — 33 requisiti obbligatori |
| [ETSI TS 103 701](https://www.cclab.com/news/how-to-meet-iot-cybersecurity-standards-using-the-etsi-en-303-645-guide) | Schema di test per EN 303 645 |

ETSI EN 303 645 è lo standard globale di riferimento per la sicurezza IoT consumer
(base della EU Cyber Resilience Act). Richiede esplicitamente che le credenziali e
i dati sensibili memorizzati sui dispositivi siano protetti, e che l'integrità del
software sia verificata tramite secure boot.

### Implementazione su ESP32-C3

L'ESP32-C3 supporta nativamente due meccanismi complementari:

| Meccanismo | Descrizione | Riferimento |
|---|---|---|
| **Flash Encryption** (AES-256 XTS) | Cifra tutto il contenuto della flash — il modello salvato in NVS è illeggibile senza la chiave hardware bruciata in eFuse | [Espressif Flash Encryption docs](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/security/flash-encryption.html) |
| **Secure Boot** (RSA-3072) | Solo firmware firmato con chiave privata trusted può girare sul dispositivo | [Espressif Secure Boot docs](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/security/flash-encryption.html) |

Una volta abilitati, sono irreversibili — la chiave AES è bruciata in eFuse e
non accessibile nemmeno via software. Questo allinea il progetto con le provisioni
di ETSI EN 303 645 sulla protezione dei dati sensibili a riposo.

Riferimento tecnico: [ESP32 Secure Boot & Flash Encryption — Zbotic](https://zbotic.in/esp32-secure-boot-and-flash-encryption-protect-your-device/)

---

## REQ-6 — Anti-spoofing delle comunicazioni tra dispositivo e hub

### Standard di riferimento

| Standard | Descrizione |
|---|---|
| [IEC 62443-3-3](https://securityboulevard.com/2025/05/securing-iiot-with-iec-62443-a-technical-guide-to-breach-proof-architectures/) | Requisiti di sicurezza per sistemi industriali — SL1/SL2/SL3 |
| [ETSI EN 303 645 §5.8](https://www.etsi.org/technologies/consumer-iot-security) | "IoT devices shall use best practice cryptography" |
| [RFC 8446 — TLS 1.3](https://www.rfc-editor.org/rfc/rfc8446) | Transport Layer Security 1.3 |

IEC 62443 richiede comunicazioni cifrate con TLS 1.3 e autenticazione tramite
certificati X.509 per prevenire spoofing, replay attack e man-in-the-middle.

Lo standard de facto per IoT security è **MQTT over TLS 1.3** con autenticazione
client-side. Il modello di minaccia da considerare (STRIDE) include:

- **Spoofing** → mitigato da autenticazione mutua TLS con certificati per dispositivo
- **Tampering** → mitigato da firma HMAC dei messaggi
- **Repudiation** → mitigato da logging con timestamp firmati
- **Information Disclosure** → mitigato da cifratura AES in transito
- **DoS** → vedi REQ-7

Riferimento: [Mapping IoT to IEC 62443 — PMC](https://pmc.ncbi.nlm.nih.gov/articles/PMC11820253/)

---

## REQ-7 — Protezione DoS verso hub centrale e cloud

### Standard di riferimento

| Standard | Descrizione |
|---|---|
| [OWASP IoT Top 10](https://owasp.org/www-project-internet-of-things/) | Top 10 vulnerabilità IoT — include DoS come rischio primario |
| [IEC 62443-2-1](https://realtimelogic.com/articles/IEC-62443-Security-Guide-for-IoT-and-Embedded-Web-Servers) | Best practice operative per ambienti IIoT |
| [IETF RFC MUD](https://arxiv.org/pdf/2305.02186) | Manufacturer Usage Description — rate limiting per dispositivo IoT |

OWASP IoT Top 10 identifica i servizi di rete insicuri come vettore primario per
attacchi DoS/DDoS. I dispositivi IoT compromessi vengono usati come botnet
(caso emblematico: attacco Mirai, che ha compromesso migliaia di dispositivi con
credenziali di default).

### Livelli di protezione (IEC 62443)

| Security Level | Protegge da |
|---|---|
| SL1 | Errori casuali e attacchi opportunistici automatizzati |
| SL2 | Hacker generici, malware automatizzato |
| SL3 | Attaccanti specializzati con conoscenza del sistema |

Per un sistema residenziale il target è **SL1-SL2**. Le mitigazioni standard sono:

- Rate limiting per IP sull'hub (max N richieste/secondo per sorgente)
- Autenticazione mutua TLS — un dispositivo compromesso non può impersonare altri
- Separazione della rete IoT dalla rete principale (VLAN dedicata)
- Watchdog sul broker MQTT per rilevare flood anomali

Il requirement dovrebbe specificare il livello target, ad esempio:

> "Il sistema deve resistere a un attacco DoS di tipo flood da un singolo
> dispositivo compromesso (SL1), con degradazione graceful del servizio
> e recovery automatico entro 60 secondi."

---

## Riepilogo standard per requirement

| Requirement | Standard primario | Standard secondario |
|---|---|---|
| REQ-1 Batteria | IEC 62133 | ETSI EN 300 328 |
| REQ-2 Tempo risposta | EN 50131-1 | EN 50131-2-6 |
| REQ-3 Falsi positivi | EN 50131-5-3 | EN 50131-1 §8 |
| REQ-4 Dashboard | IEC 61850 | EN 50136 |
| REQ-5 Modello segreto | ETSI EN 303 645 | ESP32 Flash Encryption (AES-256) |
| REQ-6 Anti-spoofing | IEC 62443-3-3 | RFC 8446 (TLS 1.3) |
| REQ-7 Anti-DoS | OWASP IoT Top 10 | IEC 62443-2-1 |
