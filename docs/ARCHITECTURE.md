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
- **Notification types:**
  - ALERT with image: Two HTTP requests (text POST + image PUT)
  - ALERT text-only: Single POST (fallback when no photo)
  - STATUS: Arm/disarm notifications (low priority)
  - Heartbeat: Every 6 hours
- **GPS:** Polled every 30s via AT+CGPSINFO, included in notifications

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
| ntfy topic | antitheft-gonnie-2219 | LILYGO |
