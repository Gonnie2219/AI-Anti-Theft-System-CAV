# Anti-Theft System Architecture

## Overview
Three-board ESP32 system that detects intrusion (vibration/door sensor), captures a photo, and sends an alert with image to ntfy.sh over cellular.

## Boards

### Main ESP32 (`ESP32Main.txt`)
- **Role:** Central coordinator — reads sensors, controls LEDs, buffers photo, forwards data to LILYGO
- **Core 1 (loop):** RF remote arm/disarm, sensor polling, LED state, drain LILYGO responses
- **Core 0 (alarmTask):** Captures photo from CAM via UART, forwards photo + ALERT to LILYGO
- **UARTs:**
  - UART0 (Serial) = USB debug
  - UART2 (Serial2, TX=17 RX=16) = ESP32-CAM
  - UART1 (SerialLilyGO, TX=27 RX=26) = LILYGO
- **Concurrency:** `lilygoMutex` (FreeRTOS semaphore) protects all SerialLilyGO access across cores. `alarmInProgress` flag prevents sensor re-triggering during alarm processing.
- **Image buffer:** 50KB heap allocation, filled from CAM then forwarded to LILYGO in 512-byte chunks

### ESP32-CAM (`ESP32CAM.txt`)
- **Role:** Photo capture + SD card storage
- **Protocol on PHOTO command:**
  1. Discards stale frame, takes 3-photo burst (saves all to SD)
  2. Sends `IMG:<size>\n` header
  3. Sends raw JPEG bytes in 512-byte chunks
  4. Sends `IMG_END\n`
- **UART:** Serial (UART0) connected to Main ESP32's Serial2

### LILYGO T-SIM7600G-H (`ESP32Lilygo.txt`)
- **Role:** Cellular gateway — receives alerts from Main ESP32, uploads to ntfy.sh via SIM7600 modem
- **UARTs:**
  - UART0 (Serial) = USB debug
  - UART1 (Serial1) = SIM7600 modem (TX=27 RX=26)
  - UART2 (Serial2, RX=21 TX=19) = Main ESP32
- **Network:** Hologram SIM, APN "hologram", connects to ntfy.sh via raw TCP (HTTP over AT+CIPOPEN/CIPSEND)
- **Network recovery:** `ensureNetwork()` checks AT+CREG? before each TCP connection and re-registers if cellular dropped
- **Notification transport:** ntfy.sh via raw TCP HTTP to `159.203.148.75:80` (port 80 is used because the SIM7600 firmware `LE20B05` lacks SNI support, which prevents HTTPS to Cloudflare-fronted hosts).
- **Command polling:** Every 5s, polls `antitheft-gonnie-2219-cmd` topic for commands (ARM, DISARM, GPS, PHOTO). Sends `REMOTE_ARM`/`REMOTE_DISARM` to Main ESP32 or handles GPS/photo requests directly.
- **Notification types:**
  - ALERT with image: ntfy text POST → ntfy image PUT (Title: "Anti-Theft ALERT", Priority: urgent, Tags: rotating_light)
  - Requested photo: ntfy text POST → ntfy image PUT (Title: "Requested Photo", Priority: default, Tags: camera)
  - ALERT text-only: ntfy POST
  - STATUS (arm/disarm): ntfy POST
  - Command ack: ntfy POST (Title: "Command Acknowledged", Priority: low, Tags: white_check_mark)
  - GPS response: ntfy POST (Title: "GPS Location", Priority: low, Tags: round_pushpin)
  - Heartbeat: ntfy POST (every 6 hours)
- **GPS:** Polled every 30s via AT+CGPSINFO, included in ntfy bodies

## Inter-Board Protocol (UART, 115200 baud)

### Main -> LILYGO
| Message | Meaning |
|---------|---------|
| `STATUS:ARMED` / `STATUS:DISARMED` | Arm/disarm state change |
| `ALERT:<reason>` | Alarm triggered |
| `IMG:<size>` + raw bytes + `IMG_END` | Photo data |
| `NOIMG` | No photo available |

### LILYGO -> Main
| Message | Meaning |
|---------|---------|
| `LILYGO_READY` | Boot complete |
| `LILYGO_OK: ...` | Notification sent successfully |
| `LILYGO_ERROR` | Notification failed |
| `REMOTE_ARM` | Remote arm command (from web dashboard via ntfy command topic) |
| `REMOTE_DISARM` | Remote disarm command (from web dashboard via ntfy command topic) |
| `REQUEST_PHOTO` | Photo request (from web dashboard via ntfy command topic) |

### Main -> CAM
| Message | Meaning |
|---------|---------|
| `PHOTO` | Request photo capture |

### CAM -> Main
| Message | Meaning |
|---------|---------|
| `CAM_READY` | Boot complete |
| `CAM_OK: ...` | Photo saved to SD (x3) |
| `IMG:<size>` + raw bytes + `IMG_END` | Photo data |
| `CAM_ERROR: ...` | Error |

## Web Dashboard (`webapp/public/index.html`)
- **Role:** Browser-based control panel — arm/disarm, request GPS/photo, view live alerts
- **Live URL:** https://webapp-seven-livid-86.vercel.app
- **Deployment:** Vercel static site — `webapp/` is the project root, `public/` is the output directory
- **Auth:** Client-side SHA-256 hash of username:password
- **Commands:** POSTs command text (ARM, DISARM, GPS, PHOTO) to `antitheft-gonnie-2219-cmd` ntfy topic. Buttons show visual feedback (spinner/checkmark) and a toast notification on success.
- **Live feed:** SSE subscription to `antitheft-gonnie-2219` alert topic for real-time notifications. Feed is capped at 50 items to prevent memory bloat. SSE reconnect indicator appears when the connection drops.
- **Email alerts:** On intrusion alerts (title "Anti-Theft ALERT" or priority >= 5), POSTs to ntfy.sh with `Email:` header to forward alert to email. Uses `email-forwarded` tag to prevent re-triggering the email forward on the same notification. Shows "Email sent" badge on the feed item and a toast notification.
- **Map:** Leaflet + OpenStreetMap, updates marker from GPS coordinates in notification bodies
- **Photo display:** Inline images from ntfy attachment URLs
- **External deps:** Leaflet 1.9.4 (CDN), OpenStreetMap tiles

## Key Parameters
| Parameter | Value | Location |
|-----------|-------|----------|
| Sensor debounce | 5000ms | ESP32Main |
| Image buffer | 50KB | ESP32Main + LILYGO |
| Alarm task stack | 16KB | ESP32Main |
| CAM serial RX buffer | 2048 bytes | ESP32Main |
| Image receive timeout | 30s (outer), 15s (inner) | LILYGO |
| LILYGO response timeout | 90s | ESP32Main |
| Heartbeat interval | 6 hours | LILYGO |
| Command poll interval | 5000ms | LILYGO |
| ntfy alert topic | antitheft-gonnie-2219 | LILYGO |
| ntfy command topic | antitheft-gonnie-2219-cmd | LILYGO + Web Dashboard |
