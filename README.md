# KnockKnock
Project for the course of "Internet of Things" at Sapienza University of Rome, A.Y. 2025/2026.

## Overview

KnockKnock is a low-cost, wireless IoT intrusion detection project focused on doors and windows in any size of building, may it be a residential or commercial one.thanks to the application of IoT techniques and paradigms, the project offers low-impact installation, battery-aware behavior, modular expansion, and centralized monitoring through a backend and dashboard.

## Our Team

| Name | GitHub Username | Matricola |
| --- | --- | --- |
| [Alessandro Coccia](https://www.linkedin.com/in/alessandro-coccia-2534b72a6) | [ErFonchio](https://github.com/ErFonchio) | 1988689 |
| [Leonardo Santucci](https://www.linkedin.com/in/leonardo-s-654847373) | [lellosant](https://github.com/lellosant) | 2282707 |
| [Antonio Turco](https://www.linkedin.com/in/antonyoturco/) | [antonyo-turco](https://github.com/antonyo-turco) | 1986183 |

## Blog
### [Knock Knock blog](https://wiz.altervista.org/iot-course-sapienza/)

## Deliveries

### First delivery

#### [First delivery presentation](https://github.com/antonyo-turco/iot-presentations/blob/9377e07d5346901678ff115f52adedeaf1f66225/1st-presentation/outputs/main.pdf)

### Second delivery
#### [Second delivery presentation](https://github.com/antonyo-turco/KnockKnock/blob/main/Second%20delivery%20presentation.pdf)
#### [Second delivery MD](https://github.com/antonyo-turco/KnockKnock/blob/main/iot-MD/2nd-delivery.MD)
#### [Demo second delivery](https://youtu.be/wBy7bFRrQwU)

### Final delivery
#### [Final delivery presentation](https://github.com/antonyo-turco/iot-presentations/blob/7c1fe8fa58019908c3fef2b911b31b3d940134ad/3rd-presentation/build/main.pdf)


### [Demo final presentation](https://youtu.be/919C2gk2Hbs)







## Getting Started

### Prerequisites

```bash
pip install -r requirements.txt
```

### 1. Network setup

Run once before building firmware, and again whenever your LAN IP changes:

```bash
python setup_network.py
```

This will:
- Auto-detect your LAN IP
- Generate TLS certificates for the MQTT broker
- Copy `ca.crt` into the firmware tree (`edge-hub/certs/`)
- Patch `CLOUD_MQTT_BROKER_IP` in `edge-hub/include/config.h`
- Print a QR code to reach the web dashboard from your phone

If the IP hasn't changed, the script exits early without regenerating certs. Use `--force` to override:

```bash
python setup_network.py --force
```

### 2. Start the cloud infrastructure

```bash
docker compose -f cloud-infrastructure/docker-compose.yml up -d
```

| Service | URL |
| --- | --- |
| Web dashboard | `http://<LAN-IP>:3000` |
| Grafana | `http://<LAN-IP>:3001` |
| InfluxDB | `http://<LAN-IP>:8086` |
| MQTT (TLS) | `mqtts://<LAN-IP>:8883` |

### 3. Build and flash the firmware

**Edge hub:**
```bash
cd edge-hub
pio run --target upload
```

**IoT sensor:**
```bash
cd iot-sensor
pio run --target upload
```

