# Anti-theft TinyML Firmware

Sistema antifurto per finestre basato su ESP32-C3 e accelerometro ADXL362.
Il dispositivo impara autonomamente il comportamento vibrazionale normale della finestra
su cui è installato e attiva un allarme quando rileva pattern anomali.

---

## Idea generale

Il sistema non usa un modello pre-addestrato. Invece, nelle prime 48 ore dalla
installazione, il dispositivo raccoglie dati dal sensore e costruisce da solo
un modello del comportamento normale della finestra: vibrazioni da traffico,
vento, treni, porte che sbattono. Tutto quello che accade durante questa fase
viene considerato "normale" e non farà scattare l'allarme in futuro.

Dopo le 48 ore il dispositivo entra in modalità inferenza e inizia a sorvegliare.
Se rileva un pattern vibrazionale significativamente diverso da quelli imparati,
attiva il LED di allarme.

---

## Hardware richiesto

| Componente | Note |
|---|---|
| ESP32-C3 (Heltec V4) | Microcontrollore principale |
| ADXL362 | Accelerometro 3 assi, interfaccia SPI |
| OLED SSD1306 128×64 | Display I2C per stato del sistema |
| LED | Usa `LED_BUILTIN` della scheda |

### Cablaggio SPI (ADXL362)

| Segnale | GPIO |
|---|---|
| CS | 4 |
| MISO | 3 |
| MOSI | 2 |
| SCK | 1 |

Il display OLED usa i pin `SDA_OLED`, `SCL_OLED`, `RST_OLED` definiti dalla
board Heltec V4. L'alimentazione OLED è gestita tramite `Vext`.

---

## Flusso operativo

Il firmware attraversa tre fasi in sequenza. Una volta completate le prime due,
il modello viene salvato nella NVS (memoria flash non volatile) dell'ESP32 e
viene ricaricato automaticamente ad ogni riavvio, saltando direttamente alla
fase di inferenza.

```
┌─────────────────────────────────────────────────────────┐
│  FASE 1 — EXPLORING  (default: 24h)                     │
│                                                         │
│  Il sensore campiona continuamente.                     │
│  Per ogni finestra di dati vengono calcolate 45 feature.│
│  La normalizzazione statistica viene aggiornata online  │
│  con l'algoritmo di Welford (nessun dato salvato).      │
│                                                         │
│  Un "novelty buffer" raccoglie fino a 200 campioni      │
│  massimamente diversi tra loro. Ogni nuovo campione     │
│  entra nel buffer solo se è sufficientemente diverso    │
│  da tutti quelli già presenti. Questo garantisce che    │
│  eventi rari (treni, raffiche di vento) siano           │
│  rappresentati anche se si verificano una volta sola.   │
└────────────────────────────┬────────────────────────────┘
                             │ fine periodo
                             ▼
┌─────────────────────────────────────────────────────────┐
│  TRANSIZIONE EXPLORING → TRAINING                       │
│                                                         │
│  Viene eseguito K-Means++ sul novelty buffer.           │
│  I 4 centroidi vengono posizionati nei punti dello      │
│  spazio delle feature che massimizzano la distanza      │
│  reciproca. Questo garantisce che ogni tipo di          │
│  comportamento visto in 24h abbia un centroide          │
│  dedicato, inclusi gli outlier rari.                    │
│                                                         │
│  La normalizzazione viene congelata con i parametri     │
│  finali di Welford e non viene più modificata.          │
└────────────────────────────┬────────────────────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────┐
│  FASE 2 — TRAINING  (default: 24h)                      │
│                                                         │
│  I centroidi sono già posizionati. Per ogni finestra    │
│  il campione normalizzato viene assegnato al centroide  │
│  più vicino, che viene raffinato con la media online.   │
│                                                         │
│  Per ogni cluster vengono calcolate le statistiche di   │
│  distanza: media, deviazione standard e massimo.        │
│  Queste servono a calcolare la soglia di allarme.       │
└────────────────────────────┬────────────────────────────┘
                             │ fine periodo
                             ▼
┌─────────────────────────────────────────────────────────┐
│  TRANSIZIONE TRAINING → INFERENCE                       │
│                                                         │
│  Per ogni cluster viene calcolata la soglia finale:     │
│  threshold = max(mean_dist + 3σ,  dist_max × 1.1)       │
│                                                         │
│  Il primo termine copre la distribuzione tipica.        │
│  Il secondo garantisce che ogni evento visto durante    │
│  il training non diventi un falso positivo.             │
│                                                         │
│  Il modello viene salvato nella NVS dell'ESP32.         │
└────────────────────────────┬────────────────────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────┐
│  FASE 3 — INFERENCE  (permanente)                       │
│                                                         │
│  Per ogni finestra vengono calcolate le 45 feature,     │
│  normalizzate e confrontate con i 4 centroidi.          │
│  Se la distanza dal centroide più vicino supera la      │
│  soglia di quel cluster → ALLARME.                      │
└─────────────────────────────────────────────────────────┘
```

---

## Feature estratte (45 totali)

Per ogni finestra di 256 campioni (2.56 secondi) vengono calcolate:

| # | Feature | Descrizione |
|---|---|---|
| 0 | `impact_score` | Score composito in [0,1]: 40% p99 + 35% jerk + 25% banda 20-40Hz |
| 1-4 | `m/x/y/z_p99` | 99° percentile del valore assoluto del segnale |
| 5-8 | `m/x/y/z_jerk_max` | Massima variazione tra campioni consecutivi |
| 9-12 | `m/x/y/z_band_20_40` | Potenza spettrale nella banda 20-40 Hz |
| 13-16 | `m/x/y/z_band_1_5` | Potenza spettrale nella banda 1-5 Hz |
| 17-20 | `m/x/y/z_band_5_20` | Potenza spettrale nella banda 5-20 Hz |
| 21-23 | `x/y/z_zcr` | Zero-crossing rate (cambi di segno / campione) |
| 24-30 | `x_top7_freq` | Hz dei 7 bin FFT con magnitudine più alta sull'asse X |
| 31-37 | `y_top7_freq` | Idem asse Y |
| 38-44 | `z_top7_freq` | Idem asse Z |

Il prefisso `m` indica la magnitudine euclidea `sqrt(x²+y²+z²)`.
Le feature per asse permettono di distinguere la direzione dell'impatto,
cruciale per discriminare un tentativo di apertura (prevalente su Z)
dal traffico stradale (prevalente su X/Y).

---

## Algoritmo — K-Means++ online

Il K-Means usato è una variante **deterministica furthest-point** di K-Means++:

- **Seeding:** i K centroidi iniziali vengono scelti dal novelty buffer
  massimizzando la distanza reciproca. Nessun numero casuale — risultato
  completamente riproducibile.
- **Aggiornamento online:** ogni nuovo campione aggiorna il centroide più
  vicino con la media cumulativa (`c += (x - c) / n`), senza learning rate
  da scegliere.
- **Normalizzazione:** tutti i vettori sono normalizzati in z-score prima
  del calcolo delle distanze, in modo che feature con scale molto diverse
  (es. `impact_score` ≈ 0.05 vs `m_p99` ≈ 1000) contribuiscano equamente.

---

## Parametri configurabili

### `main.cpp` — durate delle fasi

```cpp
#define EXPLORING_DURATION_MS  (24UL * 3600UL * 1000UL)   // 24h produzione
#define TRAINING_DURATION_MS   (24UL * 3600UL * 1000UL)   // 24h produzione
```

Per il testing usa i valori più corti commentati nel file.
**Importante:** le due righe vanno cambiate insieme.

### `config.h` — parametri del sensore e del buffer

```cpp
SAMPLE_COUNT      = 256     // campioni per finestra (= FFT_SIZE)
FFT_SIZE          = 256     // dimensione FFT — non separare da SAMPLE_COUNT
SAMPLING_RATE_HZ  = 100.0   // frequenza di campionamento in Hz
NOVELTY_BUFFER_SIZE = 200   // max campioni nel novelty buffer
NOVELTY_THRESHOLD   = 1.5   // distanza minima (spazio norm.) per "novità"
```

### `tinyml_training.h` — parametri del modello

```cpp
KMEANS_K        = 4      // numero di cluster
SIGMA_MULT      = 3.0    // moltiplicatore σ per la soglia statistica
MAX_DIST_MARGIN = 1.1    // margine sulla distanza massima (10%)
MIN_THRESHOLD   = 0.30   // soglia minima assoluta per cluster silenzioso
```

---

## Indicatori visivi

### LED

| Stato | Pattern |
|---|---|
| EXPLORING | Doppio-blink ogni 3 secondi |
| TRAINING | Blink lento (0.8s) |
| INFERENCE — baseline | Spento |
| INFERENCE — deviazione | Blink veloce (120ms) |
| Transizione di fase | Acceso fisso (brevemente) |

### Display OLED

| Fase | Contenuto |
|---|---|
| EXPLORING | Barra avanzamento, % buffer novelty, contatore campioni |
| TRAINING | Barra avanzamento, contatore campioni, numero cluster |
| INFERENCE baseline | `dist`, `impact`, `m_p99`, numero finestra |
| INFERENCE deviazione | Schermata invertita (bianco su nero) con stesse metriche |

---

## Struttura dei file

```
config.h                 Costanti condivise (SAMPLE_COUNT, FFT_SIZE, ecc.)
feature_extraction.h     Definizione struct InferenceFeatures (45 campi)
feature_extraction.cpp   Calcolo delle 45 feature (FFT, bande, ZCR, top7)
tinyml_training.h        API del modello K-Means++
tinyml_training.cpp      Novelty buffer, seeding, training, inferenza, NVS
main.cpp                 Loop principale, gestione stati, OLED, LED, SPI
```

---

## Librerie richieste

- `ArduinoFFT` — calcolo FFT
- `Adafruit GFX Library` — grafica OLED
- `Adafruit SSD1306` — driver display
- `Preferences` — NVS (inclusa nell'SDK ESP32 Arduino)
