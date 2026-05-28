# KnockKnock: Low-Cost IoT Intrusion Detection for Doors and Windows

### Academic Project | Sapienza University of Rome
**Course:** IoT: Algorithms and Services (A.Y. 2025/2026)  
**Authors:** Alessandro Coccia, Leonardo Santucci, Antonio Turco  

---

## Table of Contents
1. [The Challenge: Intrusion Detection Without Wires](#1-the-challenge-intrusion-detection-without-wires)
2. [System Architecture: Three-Tier Isolation](#2-system-architecture-three-tier-isolation)
3. [Hardware & Evolution: Hunting for Every Nano-Ampere](#3-hardware--evolution-hunting-for-every-nano-ampere)
4. [On-Device K-Means++ Machine Learning](#4-on-device-k-means-machine-learning)
5. [Hardware-Enforced & Cryptographic Security](#5-hardware-enforced--cryptographic-security)
6. [Performance & Battery Life Analysis](#6-performance--battery-life-analysis)

---

## 1. The Challenge: Intrusion Detection Without Wires

Home security is a paramount concern, yet standard alarm systems present substantial barriers. Traditional window sensors require complex, expensive electrical wiring—a major hurdle for renters or retrofitting older buildings. Wireless alternatives exist but suffer from rapid battery drain, requiring constant maintenance, or carry prohibitive price tags exceeding €40 per node.

**KnockKnock** was designed to break this trilemma. Our goal was to build a security sensor that could be mounted *inside* a window or door frame, operate for months (or years) on a single battery, cost less than €10 to manufacture, and accurately identify structural intrusion vibrations (like drilling, hammering, or breaking glass) while ignoring ambient noise (like heavy street traffic, footsteps, high winds, or heavy rain).

To succeed, the project had to meet a strict, uncompromising set of engineering constraints:
*   🔋 **Ultra-Low Power:** Quiescent current must remain below 100µA, targeting an average battery life exceeding 6 months on standard cells.
*   ⚡ **Real-Time Latency:** From the first physical impact on the glass to a remote MQTT notification, total alert propagation must take less than 5 seconds.
*   🛡️ **Zero False Alarms:** Falsely triggering an alarm due to wind or a passing truck is unacceptable; false positive rates must hover near 0.0%.

---

## 2. System Architecture: Three-Tier Isolation

A major architectural flaw in many modern IoT systems is direct cloud-to-device connectivity. Forcing a low-power sensor node to run a full TCP/IP stack, perform TLS handshakes, and maintain a constant Wi-Fi connection is an energy disaster. Furthermore, exposing the sensor directly to the internet dramatically increases the network's attack surface.

KnockKnock solves this through a robust **Three-Tier Architecture** that prioritizes network isolation, security, and extreme power efficiency.

![Figure 1: The three-tier security topology. The sensor operates inside an air-gapped ESP-NOW local subnet, routing to the gateway, which bridges data via UART to a secure serial-connected hub.](https://raw.githubusercontent.com/antonyo-turco/KnockKnock/main/iot-MD/images2/system_architecture_sapienza.png)

### The Three Tiers Explained:
1.  **Tier 1: The Sensor Node (ESP32-C3 + ADXL362)**  
    Mounted on the window frame. It has no IP address, no internet credentials, and does not speak Wi-Fi. It remains in deep sleep, wakes up on physical impact via a hardware interrupt, samples and classifies the vibration locally, and sends a quick, encrypted **ESP-NOW** message to the gateway.
2.  **Tier 2: The Air-Gapped Gateway (ESP32)**  
    A mains-powered local node that listens for ESP-NOW packets from registered sensors. Upon receipt, it decrypts the packet using a unique local key and forwards the alert over a wired, cryptographically secured serial interface (**UART with AES-128-GCM**) to the Hub.
3.  **Tier 3: The Hub & Cloud (Raspberry Pi / MQTT Broker)**  
    A serial-connected device (e.g., a local Raspberry Pi, server, or cloud VM) that parses the secure UART payload and publishes an authenticated alert via **MQTT with TLS** to the final user application or security dashboard.

This design guarantees **network isolation**. Even if a hacker compromised your home router, they could not access the security sensors because the sensors carry no IP stack. The gateway acts as a hard security boundary, air-gapping the sensor network from the public internet.

---

## 3. Hardware & Evolution: Hunting for Every Nano-Ampere

Achieving a multi-month battery life under a €10 budget is an exercise in ruthless hardware optimization. Our prototype went through a complete technological transformation, replacing off-the-shelf hobbyist modules with highly efficient, specialized silicon.

### Comparison 1: The Wake-Up Sensor (LM393 vs. ADXL362)
Our initial prototype used a cheap, piezo-based vibration sensor coupled with an LM393 hardware comparator. While simple, it was a power disaster. The comparator had to be always-on, drawing a massive **1.5 mA** of idle current. Additionally, it could only provide a binary output (vibration / no vibration) via a manual physical potentiometer trimmer. This meant we could not analyze the actual shape or frequency of the vibration, making false alarms from passing traffic impossible to block.

We replaced this with the **ADXL362**, an ultra-low-power 3-axis digital accelerometer. The difference was night and day:

| Metric | Before: LM393 Comparator | After: ADXL362 Digital Sensor |
| :--- | :--- | :--- |
| **Quiescent Current** | 1,500 µA (1.5mA) | **270 nA (0.27µA)** *(5500x Better)* |
| **Threshold Config** | Manual mechanical potentiometer | Dynamic software write via SPI |
| **Data Availability** | Binary trigger only (no signal metrics) | Full 3-axis waveform sampling |
| **Weather Proofing** | Poor (sensitive mechanical exposure) | Excellent (sealed internally inside frame) |

### Comparison 2: The MCU (ESP32 DOIT V1 vs. ESP32-C3 RISC-V)
A similar transformation occurred with the micro-controller. The original ESP32 dual-core dev board was oversized and had a deep-sleep leak of nearly **4 mA** due to its onboard USB-to-UART bridge and linear voltage regulators. 

By migrating to the single-core **ESP32-C3 RISC-V SoC**, we reduced the deep-sleep current to a tiny **50 µA**. The ESP32-C3 is physically smaller, much cheaper, and perfectly matches our computational needs.

![Figure 2: The physical KnockKnock prototype. An ultra-compact layout utilizing the ESP32-C3 connected to the ADXL362 breakout board, powered by a small rechargeable battery.](https://raw.githubusercontent.com/antonyo-turco/KnockKnock/main/iot-MD/images2/project_photo.jpeg)

### The State Machine Architecture: Zero-Current Default
How do we achieve an average quiescent draw of just 50µA? The system is designed around a **hardware-gated state machine** where the power-hungry MCU is completely powered OFF (in deep sleep) by default.

The ADXL362 operates as a low-power digital watchdog. Using its internal motion detection registers, it samples the environment. As long as the window is still, the MCU sleeps. The moment a shock exceeds the hardware-configured threshold (e.g. 150mg), the ADXL362 fires its **INT1 physical pin**, which acts as a GPIO wake-up interrupt for the ESP32-C3. The MCU boots in milliseconds, samples the vibration data, evaluates it, and goes back to sleep.

![Figure 3: High-resolution state machine mapping of the sensor node. By default, the system remains in a 50µA quiescent state, waking up instantly only when the physical motion threshold triggers.](https://raw.githubusercontent.com/antonyo-turco/KnockKnock/main/iot-MD/images2/state_diagram_sapienza.png)

---

## 4. On-Device K-Means++ Machine Learning

Traditional glass-break sensors are notorious for triggering false alarms. Passing heavy trucks, strong wind gusts, low-flying aircraft, or even heavy rain create structural vibrations that can easily mimic a burglary attempt. 

Solving this in the cloud is expensive and energy-intensive. Instead, KnockKnock performs **unsupervised anomaly detection directly on the ESP32-C3** using a custom, highly-optimized **K-Means++ ML pipeline**.

*   🔋 **16x Energy Saving:** Transmitting raw training data over Wi-Fi consumes **12.67 Wh**. Training locally on-device consumes just **0.79 Wh**.
*   🧠 **99% RAM Saving:** Standard models like Isolation Forest require **4.7 MB** of RAM. Our K-Means + novelty buffer pipeline takes only **42 KB**!
*   ⚙️ **Self-Calibrating:** Different window frames (single vs double pane) create distinct signals. Local training adapts perfectly to any structure.

### The Technical Pipeline: FFT to Centroid Distance
When the ADXL362 wake-up interrupt triggers, the ESP32-C3 performs the following processing pipeline:

1.  **Signal Acquisition:** Wakes and samples 512 acceleration data points at 200 Hz. This yields a 2.56-second signal acquisition window with a high spectral resolution (&Delta;*f* &approx; 0.39 Hz).
2.  **Feature Extraction (FFT):** Computes a 512-point Fast Fourier Transform (FFT). Because structural window vibrations are concentrated under 100 Hz, we extract a 51-Dimensional feature vector mapping the **40 Hz to 100 Hz band** (safely ignoring low-frequency ambient sway and the 50 Hz electrical mains interference).
3.  **Calibration Phase:** When first installed, the device enters a 24-hour unsupervised "exploration" phase. It uses **Farthest-Point Initialization** to seed *K* = 10 clusters, improving inter-centroid spread by **2.3x** and preventing mathematical collapse. It accumulates 200 outlier samples in a rolling **Novelty Buffer** (consuming just 40.8 KB) to learn the window's normal background profile.
4.  **Dynamic Thresholding:** For each cluster *k*, the device calculates a statistical boundary threshold:
    $$\theta_k = \mu_k + 3\sigma_k$$
    This represents the statistical edge of normal ambient disturbances.
5.  **Real-Time Inference:** In monitoring mode, the incoming 51-D vector's Euclidean distance *d*\* to the nearest K-Means centroid is calculated. If *d*\* &le; &theta;<sub>k</sub>, it is classified as baseline ambient noise and ignored. If *d*\* &gt; &theta;<sub>k</sub>, it is flagged as an anomaly. The inference takes less than **10 ms** and runs in 510 Multiply-Accumulate (MAC) operations!

### Machine Learning Visualizations
*   **Farthest-Point Seeding:** Using farthest-point initialization instead of random seeding prevents early cluster collapse. In urban environments, it improves the average inter-centroid distance spread from 11.94 to 27.73 (a 2.3x improvement), guaranteeing distinct clusters for traffic, wind, rain, and slamming. (Refer to remote plot: `https://raw.githubusercontent.com/antonyo-turco/KnockKnock/main/iot-MD/images2/ml_1.png`)
*   **Outlier Novelty Buffer 2D PCA:** A major constraint of the ESP32-C3 is its 400 KB RAM. We cannot store thousands of training samples. By maintaining an intelligent rolling buffer of the 200 samples with the highest centroid distances, we capture the full boundary of ambient noise, using only 40.8 KB of memory. (Refer to remote plot: `https://raw.githubusercontent.com/antonyo-turco/KnockKnock/main/iot-MD/images2/ml_2.png`)
*   **Sweeping K to Optimize False Positives:** A sweep of cluster counts (*K*) reveals that the false positive rate hits a minimum at *K* = 10 for both urban and rural installations. Increasing *K* beyond 10 yields diminishing returns, while lower values collapse different ambient signals together, raising false triggers. (Refer to remote plot: `https://raw.githubusercontent.com/antonyo-turco/KnockKnock/main/iot-MD/images2/ml_3.png`)
*   **Power Spectral Density Analysis:** Normal ambient signals (wind, rain) show low-energy sways below 40 Hz. In contrast, burglary attempts (glass cracking, drilling) produce massive high-energy spectral spikes in the 40 Hz to 100 Hz band, making our targeted 51-D FFT vector highly discriminative. (Refer to remote plot: `https://raw.githubusercontent.com/antonyo-turco/KnockKnock/main/iot-MD/images2/ml_4.png`)

### Achieving a Perfect 0% False Positive Rate via Voting
While the individual K-Means classifier is highly accurate, it still has a minute false positive rate of 0.032% in heavy urban environments (equivalent to 1 false alarm every few days).

To bring this down to absolute zero, KnockKnock implements a **majority voting strategy**. A single anomalous window event is ignored. An alarm is only dispatched if the sensor registers **MINCONSECUTIVE = 2** consecutive anomalous signals (spanning 5.12 seconds). Because random environmental spikes are isolated, this temporal voting filter reduces the false positive rate to exactly **0.0%**!

| ![Figure 4: Normal environmental vibrations (wind, raindrops). Signal peaks around 35, safely remaining within baseline boundaries and preventing false triggers.](https://raw.githubusercontent.com/antonyo-turco/KnockKnock/main/iot-MD/images2/window_energy_with_no_alarm_detected.png) | ![Figure 5: Vibration profile during an active intrusion attempt. The energy rapidly escalates past 200, breaching the Dynamic Cluster Threshold and triggering an alarm.](https://raw.githubusercontent.com/antonyo-turco/KnockKnock/main/iot-MD/images2/window_energy_with_alarm_detected.png) |
| :---: | :---: |
| **Figure 4:** Normal baseline ambient disturbances | **Figure 5:** Burglary attempt vibration profile |

---

## 5. Hardware-Enforced & Cryptographic Security

IoT devices are notorious for their lack of security. In physical intrusion systems, a compromised sensor node can act as an entry point for an attacker to disable the alarm system or compromise the broader home network.

KnockKnock implements a strict **defense-in-depth cryptographic model** written entirely in native **ESP-IDF** (avoiding the security-bloated Arduino wrapper). It enforces protection across three independent layers:

![Figure 6: Comprehensive cryptographic architecture. The system relies on hardware-enforced trust roots (eFuses), AES-XTS flash encryption, and AES-CCM encrypted local ESP-NOW transport.](https://raw.githubusercontent.com/antonyo-turco/KnockKnock/main/iot-MD/images2/security_sapienza.png)

### Layer 1: Hardware-Enforced Root of Trust (SoC Level)
Security must start in the physical silicon. During the initial provisioning of the ESP32-C3, we blow specific one-time programmable (OTP) **eFuse registers** to permanently configure the chip's security hardware.
*   **Disable JTAG Debugging:** Once blown, JTAG debugging is permanently disabled in the silicon. Even if an attacker solders microscopic probes onto the ESP32-C3 physical pins, they cannot attach a hardware debugger to dump RAM or read CPU registers in real-time.
*   **Bootloader Lock (Secure Boot V2):** A cryptographic hash of the public key is burned into the eFuses. The ROM bootloader will only execute firmware images signed by the matching private key, blocking any attempt to flash rogue firmware.
*   **eFuse Write Protection:** The registers governing the encryption keys are permanently write-locked, preventing an attacker from wiping or disabling security features.

### Layer 2: Firmware & Data at Rest Protection (Flash Level)
If an attacker physically de-soldered the external SPI NOR flash chip from the sensor node, they could normally read the raw binary file to extract the firmware code, security keys, or ML centroids.

KnockKnock prevents this via **AES-XTS Flash Encryption**:
*   A 128-bit or 256-bit AES-XTS key is generated randomly inside the SoC during first boot and stored in a read-protected eFuse.
*   The entire flash memory (bootloader, partition table, application, and data) is encrypted at rest. Decryption happens transparently on-the-fly inside the ESP32-C3's internal cache controller as instructions are fetched.
*   To protect credentials and ML centroids, we utilize a dedicated **Encrypted Non-Volatile Storage (NVS) Partition**. The NVS partition is encrypted with its own separate AES-256 key, itself nested under the primary hardware flash key, providing complete data segregation.

### Layer 3: Link & Subnet Security (ESP-NOW Level)
Because the sensor node communicates wirelessly, an attacker could attempt to eavesopen on packets, inject false alerts, or perform replay attacks. To stop this, our ESP-NOW transport uses physical-layer cryptographic framing:

```
// Conceptual representation of the ESP-NOW Secured Frame
[ 802.11 MAC Header ] [ PMK/LMK Initialized IV ] [ AES-CCM Encrypted Payload ] [ 8-Byte MIC Tag ]
```

Our network security structure enforces strict **Key Compartmentalization**:
*   **AES-CCM Encryption:** All wireless packets are encrypted and authenticated using the AES-CCM (Counter with CBC-MAC) standard. This standard provides both bulletproof payload secrecy and a cryptographically secure message integrity check (MIC) tag, making replay and tampering attacks impossible.
*   **Zero Overhead Wake-Up:** The Wi-Fi baseband hardware accelerator handles the AES-CCM decryption in less than **0.1ms**, keeping active radio time incredibly short to maximize battery savings.
*   **PMK vs. LMK Key Hierarchy:** The system utilizes a Primary Master Key (PMK) solely to initialize the security subsystem. Each registered sensor is assigned its own unique 16-byte Local Master Key (LMK) (.encrypt = true). All communication is encrypted under this peer-specific LMK.
*   **Physical Attack Isolation:** If an attacker physically steals a sensor and breaks through the hardware protection to dump the key, they only recover the LMK unique to that specific window. Because there is no single global key, the rest of the sensor subnet, the gateway, and other window nodes remain completely secure.

---

## 6. Performance & Battery Life Analysis

The ultimate verification of KnockKnock lies in its performance numbers. We conducted exhaustive physical testing to verify both the battery life projection and end-to-end alert latency.

### Battery Life vs. Duty Cycle (3500 mAh Cell)
The battery lifespan of the sensor node is directly tied to its active duty cycle (*D*), defined as the percentage of time the MCU is awake and processing.

Under normal operating conditions, the MCU is awake for approximately 36 seconds per hour (*D* = 1%), mostly for routine heartbeat telemetry and K-Means calibration. The ADXL362 activity threshold gates all physical wake events, ensuring the MCU stays asleep.

| Duty Cycle (D) | Active Time / Hour | Average Current (I_mean) | Theoretical Battery Life (3500 mAh) |
| :---: | :---: | :---: | :---: |
| 0.01 % | 0.4 seconds | 52 µA | **7.7 Years** |
| 0.10 % | 3.6 seconds | 70 µA | **5.7 Years** |
| 0.50 % | 18.0 seconds | 150 µA | **2.7 Years** |
| **1.00 % (Typical)** | **36.0 seconds** | **250 µA** | **1.6 Years (19 Months)** |
| 5.00 % | 3.0 minutes | 1.05 mA | 4.6 Months |
| 10.00 % | 6.0 minutes | 2.05 mA | 2.3 Months |
| 20.00 % | 12.0 minutes | 4.04 mA | 1.2 Months |
| 50.00 % | 30.0 minutes | 10.00 mA | 14.5 Days |

**Worst-Case Radio Deviation:** In environments with severe radio interference, the ESP-NOW transmission may fail, triggering multiple active retries. In this worst-case scenario, the active time per alarm event rises to 3.0 seconds (consisting of 2.5s sampling/FFT + 0.5s prolonged radio retries), and the average active current spikes to 50 mA. At a 1% duty cycle, this drops the battery life from 1.6 years to **8.7 months**. Even under these extreme conditions, the system still comfortably exceeds our original 6-month battery requirement!

### End-to-End Latency Breakdown
We measured the time elapsed from the physical impact on the window frame to the delivery of the MQTT security alert. The total latency is just **2.74 seconds**, easily satisfying our 5-second real-time latency target.

*   **ADXL362 Physical Trigger (< 1 ms):** Physical vibration exceeds the hardware threshold. The ADXL362 fires its INT1 hardware pin.
*   **ESP32-C3 Wake & Boot (~ 10 ms):** The micro-controller wakes from deep sleep and loads the signed bootloader via Secure Boot V2.
*   **Vibration Signal Acquisition (2560 ms):** The primary bottleneck. Gathers 512 points at 200 Hz to guarantee the high spectral resolution required for K-Means classification.
*   **FFT & Feature Extraction (< 50 ms):** Computes the Fast Fourier Transform and isolates the 51-D feature vector mapping the 40–100 Hz band.
*   **K-Means Inference (< 10 ms):** Measures Euclidean distance to the 10 centroids. Compares with cluster threshold &theta;<sub>k</sub>.
*   **ESP-NOW Secure Transmission (~ 1.5 ms):** Transmits the LMK-encrypted alert frame to the local gateway using hardware AES-CCM.
*   **UART AES-128-GCM Bridge (< 5 ms):** The gateway bridges the alert over the secure, wired serial connection to the Hub.
*   **Cloud MQTT Publication (< 100 ms):** The Hub receives, decrypts, and publishes the secure TLS MQTT packet to the alarm dashboard.

**Total Alert Latency: ~ 2.74 Seconds**

---

#UltraLowPower #OnDeviceML #ESP32C3 #ADXL362 #ESP-NOW #KMeans #EdgeComputing #Cybersecurity
