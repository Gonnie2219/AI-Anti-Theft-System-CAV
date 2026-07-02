# Anti-Theft System Architecture

## Overview
Three-board ESP32 system that detects intrusion (vibration/door sensor), captures a photo, and sends an alert with image to ntfy.sh over cellular.

## Boards

### Main ESP32 (`ESP32Main.txt`)
- **Role:** Central coordinator — reads sensors, controls LEDs, buffers photo, forwards data to LILYGO
- **Core 1 (loop):** RF remote arm/disarm, sensor polling, LED state, drain LILYGO responses
- **Core 0 (alarmTask):** Requests photo from CAM first (siren sounds during capture), retries once on CAM failure, then sends ALERT (with `Cam: <status>` diagnostic) + photo to LILYGO
- **UARTs:**
  - UART0 (Serial) = USB debug
  - UART2 (Serial2, TX=17 RX=16) = ESP32-CAM
  - UART1 (SerialLilyGO, TX=27 RX=26) = LILYGO
- **Concurrency:** `lilygoMutex` (FreeRTOS semaphore) protects all SerialLilyGO access across cores. `alarmInProgress` flag prevents sensor re-triggering during alarm processing. `statusPending` flag defers STATUS sends that collide with the alarm task's UART usage — flushed after alarm completes.
- **Image buffer:** 64KB heap allocation, filled from CAM then forwarded to LILYGO in 512-byte chunks
- **Immobilizer:** AEDIKO relay on GPIO23, wired through the **NC** contact (`RELAY_WIRING_NC 1`) — coil energized only while immobilized (GPIO23 HIGH = immobilized, LOW = ignition OK). Saves ~70 mA continuous and fails safe to ignition-OK on power loss. State persists in NVS and is re-asserted at boot.

### ESP32-CAM (`ESP32CAM.txt`)
- **Role:** Photo capture + SD card storage
- **Protocol on PHOTO command:**
  1. Takes 2 warm-up frames (AE/AWB settle), then 1 keeper frame (saved to SD)
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
- **Network:** Hologram SIM, APN "hologram", IPv4 PDP. Connects to ntfy.sh via SIM7600 built-in HTTP stack (AT+HTTPINIT/HTTPPARA/HTTPDATA/HTTPACTION)
- **Network recovery:** `ensureNetwork()` checks AT+CREG? (local AT command, no data cost) before every outbound HTTP request and re-attaches (CFUN=1 → CGATT=1 → CGACT=1,1) if cellular registration dropped. Logs "NETWORK: recovered" on success.
- **Persistent HTTP session:** `USE_PERSISTENT_HTTP 1` keeps the modem HTTP(S) service initialized across requests (TCP+TLS handshake reused). Session is termed + re-inited only on transport error/timeout (one retry, logs "[HTTP] session reset, retrying") or when the URL scheme flips http↔https. Per-request timing logged: "[HTTP] OK (NNNN ms)".
- **Command polling:** Polls the Worker KV command queue (`/commands/poll` over HTTPS, Bearer `DEVICE_POLL_TOKEN`) — **adaptive cadence:** every 5 s while armed or within 10 min of boot / last command / last alert / last status change; every 30 s when idle. Logs "POLL: fast/slow" on transitions. Commands: ARM, DISARM, STATUS, PHOTO, GPS, IMMOBILIZE, RESTORE, HELP. Forwards ARM/DISARM/STATUS/PHOTO/IMMOBILIZE/RESTORE to Main ESP32 via `SMS_CMD:` UART message; handles GPS/HELP locally.
- **SMS channel:** Native AT+CMGS path disabled (`USE_NATIVE_SMS=0`). Inbound SMS commands arrive via Twilio webhook → Worker → KV command queue; outbound replies are posted to ntfy ("Command Reply") for the Worker to forward.
- **Notification types:**
  - ALERT with image: Two HTTP requests (text POST + image PUT) — Title: "Anti-Theft ALERT", Priority: urgent, Tags: rotating_light
  - Requested photo: Two HTTP requests — Title: "Requested Photo", Priority: default, Tags: camera
  - ALERT text-only: Single POST (fallback when no photo)
  - STATUS: Arm/disarm notifications (low priority)
  - Command ack: Title: "Command Acknowledged", Priority: low, Tags: white_check_mark
  - GPS response: Title: "GPS Location", Priority: low, Tags: round_pushpin
  - Heartbeat: Every 5 minutes; includes battery level from AT+CBC as "Battery: NN% (V.VVV)"
  - SMS reply: Posted to ntfy with Title "Command Reply"; Worker forwards as SMS (native outbound SMS disabled)
- **GPS:** Gated on armed state — `AT+CGPS=1` on STATUS:ARMED, `AT+CGPS=0` on STATUS:DISARMED. IMMOBILIZED/IGNITION_OK don't change GPS (stays on for theft tracking). `handleAlert()` turns GPS on if off (boot-while-armed edge case). Polled every 30s via AT+CGPSINFO when active; included in notifications.

## Inter-Board Protocol (UART, 115200 baud)

### Main -> LILYGO
| Message | Meaning |
|---------|---------|
| `STATUS:ARMED` / `STATUS:DISARMED` | Arm/disarm state change |
| `SMS_REPLY:<text>` | Response text to send back via SMS |
| `ALERT:<reason>[; Cam: <status>]` | Alarm triggered, sent after the photo phase. Cam status: `PHOTO_OK <n>`, `CAM_NO_RESPONSE`, `CAM_PARTIAL <got>/<exp>`, `CAM_GARBAGE`; `,RETRY_OK <n>` / `,RETRY_FAIL` appended after the single retry. Omitted for "Photo Requested" (LILYGO exact-matches it) |
| `IMG:<size>` + raw bytes + `IMG_END` | Photo data |
| `NOIMG` | No photo available |

### LILYGO -> Main
| Message | Meaning |
|---------|---------|
| `LILYGO_READY` | Boot complete |
| `LILYGO_OK: ...` | Notification sent successfully |
| `LILYGO_ERROR` | Notification failed |
| `REMOTE_ARM` | Remote arm command (from web dashboard via Worker KV command queue) |
| `REMOTE_DISARM` | Remote disarm command (from web dashboard via Worker KV command queue) |
| `REQUEST_PHOTO` | Photo request (from web dashboard via Worker KV command queue) |
| `SMS_CMD:ARM` | Arm command (from inbound SMS) |
| `SMS_CMD:DISARM` | Disarm command (from inbound SMS) |
| `SMS_CMD:STATUS` | Status request (from inbound SMS) |
| `SMS_CMD:PHOTO` | Photo request (from inbound SMS) |

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
- **Role:** Browser-based control panel — arm/disarm, request GPS/photo, view live alerts with AI threat analysis
- **Live URL:** https://webapp-seven-livid-86.vercel.app
- **Deployment:** Vercel — `webapp/` is the project root, `public/` is the static output directory, `api/` contains serverless functions
- **Auth:** Client-side SHA-256 hash of username:password
- **Commands:** POSTs commands (ARM, DISARM, GPS, PHOTO, IMMOBILIZE, RESTORE) to the Worker `/commands/dispatch` endpoint (CMD_SECRET auth), which queues them in KV for the LILYGO to poll. Buttons have Unicode icons and show toast notification on success.
- **Live feed:** SSE subscription to `antitheft-gonnie-2219` alert topic for real-time notifications. Feed is capped at 50 items. Slide-in animation on new items. SSE reconnect indicator on connection drop.
- **Battery info:** No dedicated widget — heartbeat notifications carry a "Battery: NN% (V.VVV)" line (AT+CBC on LILYGO) that displays as plain text in the live feed.
- **Online/offline detection:** Tracks `lastMessageTime`. After 3 minutes of silence, shows "SYSTEM OFFLINE" with last-known status and minutes since last seen. Checked every 30 seconds.
- **localStorage persistence:** Saves and restores: armed/disarmed status, battery level, GPS coordinates, and last message timestamp across page refreshes.
- **AI threat classification:** When a photo attachment arrives (title contains "Photo Evidence" or filename is `alert.jpg`), the dashboard POSTs the image URL to `/api/analyze` (Vercel serverless function). The function uses the Anthropic Claude Vision API to classify the image as HIGH/MEDIUM/LOW threat with a text verdict. Result is displayed as a color-coded badge on the feed item.
- **Photo interaction:** Images are wrapped in a `.photo-wrap` container with a download button overlay. Clicking an image opens a full-screen lightbox overlay (click to dismiss).
- **Markdown rendering:** `formatBody()` converts `**bold**` → `<strong>`, `[text](url)` → clickable links, and bare GPS coordinate pairs → Google Maps links.
- **Email alerts:** On intrusion alerts (title "Anti-Theft ALERT" or priority >= 5), POSTs to ntfy.sh with `Email:` header. Uses `email-forwarded` tag to prevent loops. Shows "Email sent" badge.
- **Map:** Leaflet + OpenStreetMap, auto-updates marker from GPS coordinates in notification bodies.
- **External deps:** Leaflet 1.9.4 (CDN), OpenStreetMap tiles, @anthropic-ai/sdk (server-side)

### AI Analysis Endpoint (`webapp/api/analyze.js`)
- **Role:** Vercel serverless function that classifies photos using Anthropic Claude Vision API
- **Input:** POST with JSON body `{ imageUrl: "..." }`
- **Process:** Fetches the image, sends it to Claude with a security-focused prompt asking for threat classification
- **Output:** JSON `{ threatLevel: "HIGH"|"MEDIUM"|"LOW", verdict: "..." }`
- **Auth:** Requires `X-CMD-Secret` header matching the `CMD_SECRET` environment variable (shared secret with Worker). Also requires `ANTHROPIC_API_KEY` environment variable (set in Vercel project settings).

### PWA + Web Push Notifications
- **Installable PWA:** `manifest.json` (name "Anti-Theft Dashboard", `display: standalone`, dark theme) + `icon-192/512.png` + `apple-touch-icon.png`. On iPhone: Safari Share → Add to Home Screen (iOS 16.4+ required for push).
- **Service worker (`webapp/public/sw.js`):** push-only — `push` shows the notification, `notificationclick` focuses/opens the dashboard. Deliberately NO caching/offline/fetch interception, so SSE and future deploys are unaffected.
- **Subscribe flow:** after login a 🔔 bell appears when the Push API exists (on iOS that's only inside the installed home-screen app; the Expo WebView never shows it). Tap → `Notification.requestPermission()` → `pushManager.subscribe()` with the VAPID public key → POST subscription to Worker `/push/subscribe`. All `/push/*` endpoints require CMD_SECRET auth (alerts contain GPS, so subscribe is not open). "Send test" hits `/push/test`; `/push/unsubscribe` removes a subscription.
- **Worker fan-out (`worker/worker.js`):** at both alert trigger points — the `/ingest` fast path and the ntfy cron loop — the Worker calls `sendWebPushToAll()`: a pure-WebCrypto implementation of VAPID ES256 JWT (RFC 8292) + aes128gcm payload encryption (RFC 8291), because npm `web-push` does not run on Cloudflare Workers. Subscriptions are stored in KV as `push_sub:<sha256(endpoint)>`; a 404/410 from the push service prunes the entry. TTL 3600. A `push` flag in the existing per-alert dedup JSON prevents double sends between ingest and cron.
- **Keys:** VAPID public key is a constant in `worker.js` and `index.html`; the private key is the `VAPID_PRIVATE_KEY` Wrangler secret (subject `mailto:gonnie2219@gmail.com`).
- Web Push is an ADDITIONAL user-facing channel — ntfy stays the hardware→cloud transport, and WhatsApp delivery is unchanged.

## Key Parameters
| Parameter | Value | Location |
|-----------|-------|----------|
| Sensor debounce | 5000ms | ESP32Main |
| Image buffer | 64KB | ESP32Main + LILYGO |
| Alarm task stack | 16KB | ESP32Main |
| CAM serial RX buffer | 2048 bytes | ESP32Main |
| CAM photo receive | 15 s timeout, 1 retry after 2 s flush | ESP32Main |
| Image receive timeout | 30s (outer), 15s (inner) | LILYGO |
| LILYGO response timeout | 90s | ESP32Main |
| Heartbeat interval | 5 minutes | LILYGO |
| Command poll interval | adaptive: 5 s (armed/active) / 30 s (idle), 10 min activity window | LILYGO |
| ntfy alert topic | antitheft-gonnie-2219 | LILYGO |
| Command queue | Worker KV via `/commands/poll` (Bearer DEVICE_POLL_TOKEN) | LILYGO + Worker |
