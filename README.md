# AI-Enabled Anti-Theft System for Connected and Autonomous Vehicles

**Wayne State University — Undergraduate Research Opportunity Program (UROP) Winter 2026**

An embedded anti-theft system that uses a three-board ESP32 architecture to detect unauthorized vehicle access, capture photographic evidence, and deliver real-time alerts with GPS location over cellular networks.

## System Architecture

The system uses three interconnected ESP32 boards communicating over UART at 115200 baud:

```
┌─────────────────────┐    UART2     ┌──────────────────┐
│   ESP32-CAM         │◄────────────►│   Main ESP32     │
│   (AI Thinker)      │  TX=17 RX=16 │   (Dev Module)   │
│                     │              │                  │
│ - Camera capture    │              │ - RF remote      │
│ - 3-photo burst     │              │ - Vibration      │
│ - SD card storage   │              │   sensor         │
└─────────────────────┘              │ - Reed switch    │
                                     │ - Status LEDs    │
                                     │ - Dual-core      │
                                     │   (FreeRTOS)     │
                                     └───────┬──────────┘
                                             │ UART1
                                             │ TX=27 RX=26
                                     ┌───────▼──────────┐
                                     │   LILYGO         │
                                     │   T-SIM7600G-H   │
                                     │                  │
                                     │ - Cellular modem │
                                     │ - GPS tracking   │
                                     │ - ntfy.sh alerts │
                                     │ - Hologram SIM   │
                                     └──────────────────┘
```

**Core 1 (Main Loop):** RF remote monitoring, sensor polling, LED control — always responsive, never blocked by alarm processing.

**Core 0 (Alarm Task):** Photo capture request, image buffering, UART forwarding to LILYGO — runs as a FreeRTOS background task.

## Hardware Components

| Component | Model | Role |
|-----------|-------|------|
| Main ESP32 | ESP32 Dev Module | Central coordinator |
| Camera | AI Thinker ESP32-CAM | Photo capture + SD storage |
| Cellular | LILYGO T-SIM7600G-H | Cellular gateway + GPS |
| RF Remote | 433MHz (RCSwitch) | Arm/disarm |
| Vibration Sensor | SW-420 | Tamper detection |
| Reed Switch | Magnetic | Door open detection |
| SIM Card | Hologram IoT SIM | Cellular data |
| LEDs | Green + Red | Armed/disarmed status |

## Pin Mapping

### Main ESP32

| Pin | Function |
|-----|----------|
| GPIO 13 | Vibration sensor |
| GPIO 14 | Reed switch |
| GPIO 15 | RF receiver (433MHz) |
| GPIO 17 | UART TX to ESP32-CAM |
| GPIO 16 | UART RX from ESP32-CAM |
| GPIO 27 | UART TX to LILYGO |
| GPIO 26 | UART RX from LILYGO |
| GPIO 32 | Green LED (armed) |
| GPIO 33 | Red LED (disarmed) |

### ESP32-CAM (AI Thinker)

Standard AI Thinker pin definitions. Key pins:

| Pin | Function |
|-----|----------|
| GPIO 4 | Flash LED |
| GPIO 25 | VSYNC (camera) |
| UART0 | Connected to Main ESP32 Serial2 |

### LILYGO T-SIM7600G-H

| Pin | Function |
|-----|----------|
| GPIO 27 | Modem TX |
| GPIO 26 | Modem RX |
| GPIO 4 | Modem PWRKEY |
| GPIO 32 | Modem DTR |
| GPIO 25 | Modem FLIGHT mode |
| GPIO 34 | Modem STATUS |
| GPIO 21 | UART RX from Main ESP32 |
| GPIO 19 | UART TX to Main ESP32 |
| GPIO 12 | Status LED |

## Wiring Diagram

```
Main ESP32 GPIO 27 (TX) ──────► LILYGO GPIO 21 (RX)
Main ESP32 GPIO 26 (RX) ◄────── LILYGO GPIO 19 (TX)
Main ESP32 GND ─────────────────── LILYGO GND

Main ESP32 GPIO 17 (TX) ──────► ESP32-CAM UART0 RX
Main ESP32 GPIO 16 (RX) ◄────── ESP32-CAM UART0 TX
Main ESP32 GND ─────────────────── ESP32-CAM GND
```

## UART Communication Protocol

All UART communication runs at **115200 baud, 8N1**.

### Main ESP32 → LILYGO

| Message | Meaning |
|---------|---------|
| `STATUS:ARMED` | System armed via RF remote |
| `STATUS:DISARMED` | System disarmed via RF remote |
| `ALERT:<reason>` | Alarm triggered (e.g., "Vibration", "Door Opened") |
| `IMG:<size>` + raw bytes + `IMG_END` | Photo data transfer |
| `NOIMG` | No photo available |

### LILYGO → Main ESP32

| Message | Meaning |
|---------|---------|
| `LILYGO_READY` | Boot complete, modem + network initialized |
| `LILYGO_OK: ...` | Notification sent successfully |
| `LILYGO_ERROR` | Notification failed |

### Main ESP32 → ESP32-CAM

| Message | Meaning |
|---------|---------|
| `PHOTO` | Request photo capture |

### ESP32-CAM → Main ESP32

| Message | Meaning |
|---------|---------|
| `CAM_READY` | Camera + SD initialized |
| `CAM_OK: Photo saved /photo_N.jpg (X KB)` | Photo saved to SD (x3 burst) |
| `IMG:<size>` + raw bytes + `IMG_END` | Last photo data transfer |
| `CAM_ERROR: ...` | Error during capture |

## Flash Instructions

### Arduino IDE Board Settings

| Setting | Main ESP32 | ESP32-CAM | LILYGO |
|---------|-----------|-----------|--------|
| Board | ESP32 Dev Module | AI Thinker ESP32-CAM | ESP32 Dev Module |
| Upload Speed | 921600 | 921600 | 921600 |
| CPU Frequency | 240MHz | 240MHz | 240MHz |
| Flash Frequency | 80MHz | 80MHz | 80MHz |
| Flash Size | 4MB | 4MB | 4MB (or 16MB if available) |
| Partition Scheme | Default 4MB | Huge APP (3MB No OTA) | Default 4MB |
| PSRAM | Disabled | Enabled | Disabled |

### Required Libraries

- **RCSwitch** — RF remote control (Main ESP32)
- **esp_camera** — Camera driver (ESP32-CAM, included with ESP32 board package)
- **SD_MMC** — SD card access (ESP32-CAM, included with ESP32 board package)

### Flash Order

1. **ESP32-CAM** — Flash first, insert SD card, verify `CAM_READY` on serial monitor
2. **LILYGO** — Flash second, insert Hologram SIM, verify `Ready! Waiting for ALERT...` on serial monitor
3. **Main ESP32** — Flash last, connect wiring to both boards, verify `Status: DISARMED` on serial monitor

## Notification Setup

### ntfy.sh

The system sends push notifications via [ntfy.sh](https://ntfy.sh):

1. Install the ntfy app on your phone ([Android](https://play.google.com/store/apps/details?id=io.heckel.ntfy) / [iOS](https://apps.apple.com/us/app/ntfy/id1625396347))
2. Subscribe to topic: `antitheft-gonnie-2219` (or change `NTFY_TOPIC` in the LILYGO firmware)
3. Notifications include:
   - **ALERT** (urgent priority) — Alarm triggered with reason, timestamp, GPS location, Google Maps link, and photo
   - **STATUS** (low priority) — Arm/disarm state changes
   - **Heartbeat** (min priority) — System health check every 6 hours

### Hologram SIM

The LILYGO board uses a [Hologram](https://hologram.io) IoT SIM card:

- APN: `hologram`
- The SIM provides data connectivity for HTTP requests and GPS assistance
- No SMS is used — all communication goes through ntfy.sh HTTP API

## Known Issues

| Issue | Details | Workaround |
|-------|---------|------------|
| **GPIO 25 dead on LILYGO** | GPIO 25 is documented as MODEM_FLIGHT for the T-SIM7600G-H. On some units this pin may not toggle correctly. | Set `MODEM_FLIGHT` HIGH in setup and leave it; do not toggle at runtime. |
| **No DNS on SIM7600** | The SIM7600 modem's `AT+CIPOPEN` command does not reliably resolve hostnames. | `NTFY_IP` is hardcoded to `159.203.148.75` (ntfy.sh). If ntfy.sh changes IP, update this constant. |
| **ntfy email delivery requires auth** | Sending notifications via ntfy.sh email forwarding requires an authenticated ntfy account. | Use phone push notifications (no auth required) instead of email forwarding. |
| **50KB image buffer limit** | Images larger than 50KB are rejected. VGA quality 10 JPEG typically produces 15-40KB images. | If images are consistently too large, reduce `FRAMESIZE_VGA` to `FRAMESIZE_CIF` or increase `jpeg_quality` number (lower quality). |
| **Single alarm at a time** | The `alarmInProgress` flag prevents concurrent alarms. A second sensor trigger during an active alarm is ignored. | By design — prevents resource contention and duplicate notifications. The 5-second debounce also helps. |

## Future Work

- **AI image classification** — On-device or cloud-based image analysis to reduce false positives (e.g., distinguish a person from wind vibration)
- **Two-way control** — Send commands back to the system via ntfy.sh (e.g., remote arm/disarm, request photo on demand)
- **OTA firmware updates** — Enable over-the-air updates via cellular connection
- **Battery monitoring** — Track and report vehicle battery voltage to detect disconnection attacks
- **Multi-zone sensors** — Add additional vibration sensors or break-wire sensors for window/trunk coverage
- **Encrypted communication** — Add HTTPS/TLS support when using a modem with TLS capability

## Project Structure

```
AI-Anti-Theft-System-CAV/
├── firmware/
│   ├── AntiTheftSystemMainESP.ino      # Main ESP32 coordinator
│   ├── AntiTheftSystemESP32CAM.ino     # ESP32-CAM photo capture
│   └── AntiTheftSystemLilygo.ino       # LILYGO cellular gateway
├── docs/
│   └── ARCHITECTURE.md                 # Detailed system architecture
├── .gitignore
├── LICENSE                             # MIT License
└── README.md
```

## Author

**Gonnie Ben-Tal** ([@Gonnie2219](https://github.com/Gonnie2219))
Wayne State University — HP0787
Undergraduate Research Opportunity Program (UROP), Winter 2026

**Faculty Advisor:** Dr. Lubna Alazzawi — Wayne State University, Department of Electrical and Computer Engineering

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.
