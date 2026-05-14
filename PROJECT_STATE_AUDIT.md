# Project State Audit — Anti-Theft System

**Date:** 2026-05-07
**Auditor:** Claude (code-reading only, no runtime verification)
**Scope:** Full codebase at commit `9fee077`

---

## 1. File Inventory

### Firmware (`firmware/`)
| File | Lines | Role |
|------|-------|------|
| `AntiTheftSystemMainESP.ino` | 421 | Central coordinator, sensors, FreeRTOS dual-core |
| `AntiTheftSystemESP32CAM.ino` | 157 | Camera capture + SD storage |
| `AntiTheftSystemLilygo.ino` | 916 | Cellular gateway, ntfy, SMS, GPS |
| `CHANGES.md` | 203 | SpeedTalk migration changelog (May 3 2026) |

### Cloudflare Worker (`worker/`)
| File | Lines | Role |
|------|-------|------|
| `worker.js` | 536 | ntfy -> WhatsApp/SMS bridge, inbound SMS webhook |
| `wrangler.toml` | 10 | Deployment config (cron every minute, KV binding) |
| `package.json` | 5 | Minimal metadata, no runtime deps |
| `README.md` | 112 | Deployment instructions |

### Web Dashboard (`webapp/`)
| File | Lines | Role |
|------|-------|------|
| `public/index.html` | 821 | Single-file dashboard (HTML + CSS + JS) |
| `api/analyze.js` | 41 | Vercel serverless: Anthropic Vision AI classification |
| `package.json` | 9 | `@anthropic-ai/sdk ^0.39.0` |
| `vercel.json` | 4 | Static output dir = `public/` |

### Docs (root)
| File | Status |
|------|--------|
| `ARCHITECTURE.md` | **Modified (unstaged)** — root copy exists alongside `docs/ARCHITECTURE.md` |
| `docs/ARCHITECTURE.md` | Stale — references SpeedTalk APN "Wholesale" and old TCP-socket CIPOPEN/NETOPEN architecture, but current firmware uses Hologram + HTTPINIT |
| `README.md` | Modified (unstaged) |
| `claude_code_sms_feature_prompt.md` | Untracked — planning doc for SMS feature, references SpeedTalk; historical, not actionable |

### Stale / Out-of-place files
| File | Issue |
|------|-------|
| `ESP32Main.txt` | **Phase 4b snapshot** — older version of the Main ESP32 firmware (no immobilize, no SMS_CMD handling, no boot wait). Gitignored by `*.txt` rule but still present on disk. |
| `ESP32CAM.txt` | **Phase 4b snapshot** — identical to current `.ino` (CAM firmware unchanged). |
| `ESP32Lilygo.txt` | **Phase 4b snapshot** — old LILYGO firmware using raw TCP sockets (CIPOPEN/NETOPEN), no HTTPINIT, no SMS, no ntfy cmd polling, 6-hour heartbeat. Completely superseded. |
| `webapp/node_modules/` | Checked into git (no `.gitignore` for `node_modules` inside `webapp/`). Not harmful but adds bloat. |

---

## 2. Firmware Audit

### 2a. AntiTheftSystemMainESP.ino

**FreeRTOS architecture:**
- **Core 1** (`loop()`): RF remote polling via RCSwitch interrupt on GPIO 15, sensor reads (vibration GPIO 13, reed GPIO 14), LED control, LILYGO UART drain + SMS_CMD/REQUEST_PHOTO dispatch. Never blocks.
- **Core 0** (`alarmTask()`): Created dynamically by `startAlarm()` via `xTaskCreatePinnedToCore`. 16 KB stack, priority 1. Requests photo from CAM, buffers it, forwards to LILYGO, waits up to 90s for LILYGO_OK/LILYGO_ERROR. Self-deletes via `vTaskDelete(NULL)`.
- **Mutex:** `lilygoMutex` (FreeRTOS binary semaphore) guards all `SerialLilyGO` access. Taken with 100ms timeout in loop, `portMAX_DELAY` in alarm task. Correct: prevents Core 0 alarm writes from colliding with Core 1 drain reads.
- **Flag:** `volatile bool alarmInProgress` prevents sensor re-triggering and LILYGO drain overlap during alarm processing.

**UART pin assignments:**
| UART | Peripheral | TX | RX | Baud |
|------|-----------|----|----|------|
| UART0 (Serial) | USB debug | default | default | 115200 |
| UART2 (Serial2) | ESP32-CAM | GPIO 17 | GPIO 16 | 115200 |
| UART1 (SerialLilyGO) | LILYGO | GPIO 27 | GPIO 26 | 115200 |

**Feature flags / defines:**
| Define | Value | Purpose |
|--------|-------|---------|
| `DEBOUNCE_MS` | 5000 | Alarm debounce |
| `IMG_BUF_SIZE` | 51200 | 50 KB image buffer |
| `CODE_ARM` | 616609 | RF remote arm code |
| `CODE_DISARM` | 616610 | RF remote disarm code |
| `IMMOBILIZE_PIN` | GPIO 23 | Relay output (active LOW = immobilized) |

**NVS persistence:** `Preferences` library stores `immobilized` bool. Restored on boot (line 93). Relay pin set accordingly (line 95).

### 2b. AntiTheftSystemESP32CAM.ino

**Architecture:** Single-core, single-task Arduino `loop()`. No FreeRTOS tasks, no mutexes. Pure UART slave.

**Protocol:** On `PHOTO` command:
1. Flash LED on (GPIO 4)
2. Discard stale frame
3. 3-photo burst with 300ms gaps, all saved to SD
4. Send `IMG:<len>\n` + raw JPEG in 512-byte chunks + `IMG_END\n` (last frame only)
5. Flash LED off

**No feature flags.** Resolution: VGA (640x480), JPEG quality 10. Falls back to CIF if no PSRAM.

### 2c. AntiTheftSystemLilygo.ino

**Architecture:** Single-core Arduino `loop()` — no FreeRTOS tasks. This means all operations (UART reads, AT commands, HTTP requests) are serialized. There is **no modem mutex** — not needed because everything runs on one core sequentially.

**UART pin assignments:**
| UART | Peripheral | TX | RX | Baud |
|------|-----------|----|----|------|
| UART0 (SerialMon) | USB debug | default | default | 115200 |
| UART1 (SerialAT) | SIM7600 modem | GPIO 27 | GPIO 26 | 115200 |
| UART2 (SerialMain) | Main ESP32 | GPIO 19 | GPIO 21 | 115200 |

**Feature flags / defines:**
| Define | Value | Purpose |
|--------|-------|---------|
| `USE_NATIVE_SMS` | **0** | Guards all AT+CMGS code paths. Currently OFF — SMS is Twilio-only via Worker. |
| `APN` | `"hologram"` | Hologram SIM |
| `NTFY_TOPIC` | `"antitheft-gonnie-2219"` | Alert topic |
| `NTFY_CMD_TOPIC` | `"antitheft-gonnie-2219-cmd"` | Command topic |
| `NTFY_CMD_POLL_MS` | 5000 (5s) | Command poll interval |
| `HEARTBEAT_MS` | 300000 (5min) | Heartbeat interval |
| `GPS_UPDATE_MS` | 300000 (5min) | GPS refresh interval |
| `IMG_BUF_SIZE` | 51200 | 50 KB photo buffer |
| `SMS_MAX_PER_HOUR` | 10 | Rate limit (only compiled when USE_NATIVE_SMS=1) |
| `SMS_POLL_MS` | 60000 (1min) | SMS safety-net poll (only compiled when USE_NATIVE_SMS=1) |
| `WORKER_INGEST_URL` | `https://...workers.dev/ingest` | Fast-path Worker POST |

**Full AT command sequence at boot (setup, lines 705-857):**
1. `AT` — modem alive check (10 retries)
2. `ATE0` — echo off
3. `AT+CMEE=2` — verbose errors
4. `AT+CFUN=0` — radio off
5. `AT+CGDCONT=1,"IP","hologram"` — set APN (IPv4)
6. `AT+CGAUTH=1,0,"",""` — clear auth
7. `AT+CFUN=1` — radio on
8. `AT+CGREG?` — wait for registration (30 retries)
9. `AT+CGATT=1` — PS attach
10. `AT+CGACT=1,1` — activate PDP
11. `AT+CSQ` — signal quality (diagnostic)
12. `AT+CGATT?` — confirm PS attach (diagnostic)
13. `AT+CGDCONT?` — PDP profile (diagnostic)
14. `AT+CGPADDR=1` — IP address (diagnostic)
15. `AT+CGCONTRDP=1` — full context profile (diagnostic)
16. `AT+CPSI?` — system info (diagnostic)
17. `AT+CEER` — last error (diagnostic)
18. `AT+CGPS=1` — enable GPS
19. (When `USE_NATIVE_SMS=1`): `AT+CMGF=1`, `AT+CSCS="GSM"`, `AT+CPMS="ME","ME","ME"`, `AT+CNMI=2,1,0,0,0`, `AT+CMGD=1,4`

**AT commands used at runtime:**
- `AT+HTTPTERM`, `AT+HTTPSSL=`, `AT+HTTPINIT`, `AT+HTTPPARA`, `AT+HTTPDATA`, `AT+HTTPACTION=0` (GET), `AT+HTTPACTION=1` (POST), `AT+HTTPREAD` — HTTP client
- `AT+CGPSINFO` — GPS position
- `AT+CCLK?` — modem clock
- (When `USE_NATIVE_SMS=1`): `AT+CMGS`, `AT+CMGL="REC UNREAD"`, `AT+CMGD=1,1`

### 2d. Full Alarm Pipeline (step-by-step across all three boards)

1. **Main ESP32 Core 1:** Sensor triggers (vibration HIGH or reed HIGH). `startAlarm("Vibration")` checks debounce, sets `alarmInProgress=true`, spawns `alarmTask` on Core 0.
2. **Main ESP32 Core 0:** Takes `lilygoMutex`, sends `ALERT:Vibration\n` to LILYGO via UART1. Releases mutex.
3. **Main ESP32 Core 0:** Sends `PHOTO\n` to ESP32-CAM via UART2.
4. **ESP32-CAM:** Receives `PHOTO`, flashes LED, discards stale frame, takes 3-photo burst (saves to SD), sends `IMG:<size>\n` + raw bytes + `IMG_END\n` back on UART0.
5. **Main ESP32 Core 0:** `receivePhotoFromCAM()` buffers the image into `imgBuffer` (15s timeout). Takes `lilygoMutex`, sends `IMG:<size>\n` + raw bytes in 512-byte chunks + `IMG_END\n` to LILYGO. Releases mutex.
6. **LILYGO:** `loop()` reads `ALERT:Vibration` from SerialMain, calls `handleAlert("Vibration")`.
7. **LILYGO `handleAlert()`:**
   - `receiveImage()` — reads IMG header + binary from SerialMain (30s outer / 20s byte timeout)
   - `updateGPS()` — AT+CGPSINFO
   - (If `USE_NATIVE_SMS=1`): `sendSMS()` to owner
   - `httpPostTextRetry()` — POST alert text to `http://ntfy.sh/antitheft-gonnie-2219` (3 attempts, exponential backoff)
   - `httpPostDirect(WORKER_INGEST_URL, body)` — fast-path POST to Cloudflare Worker for immediate WhatsApp
   - `httpPostBinaryRetry()` — POST photo as `application/octet-stream` to ntfy topic (3 attempts)
   - Sends `LILYGO_OK` or `LILYGO_ERROR` back to Main via UART2
8. **Main ESP32 Core 0:** Waits up to 90s for LILYGO_OK/LILYGO_ERROR. Sets `alarmInProgress=false`. Deletes task.
9. **Cloudflare Worker (cron, every 60s):** Polls ntfy, matches alert + photo within 30s window, runs AI analysis via Vercel `/api/analyze`, sends WhatsApp (with photo + AI verdict).
10. **Cloudflare Worker (fast path, /ingest):** Receives direct POST from LILYGO, sends text-only WhatsApp immediately, writes dedup key so cron only sends photo follow-up.
11. **Web Dashboard:** SSE connection receives ntfy events in real-time, displays in feed, triggers AI analysis for photos, sends email alert for urgent events.

---

## 3. SMS Bridge Audit

### 3a. LILYGO firmware SMS functions

**With `USE_NATIVE_SMS=0` (current state), the following code is compiled out:**
- `sendSMS()` (lines 448-491) — AT+CMGS with rate limiting
- `pollIncomingSMS()` (lines 539-568) — AT+CMGL parser
- SMS init AT commands in setup (lines 813-818)
- URC handling in loop (lines 877-883)
- SMS poll timer in loop (lines 894-899)
- Startup SMS (line 847)

**Active SMS-related functions (always compiled):**
- `handleSMSCommand()` (lines 508-536) — command dispatcher, shared between ntfy poll path and native SMS path. Forwards ARM/DISARM/STATUS/PHOTO/IMMOBILIZE/RESTORE to Main via `SMS_CMD:`, handles GPS/HELP locally.
- `pollNtfyCommands()` (lines 351-405) — polls `antitheft-gonnie-2219-cmd/json?poll=1&since=<lastCmdId>`, parses JSON, calls `handleSMSCommand()`.
- `isAuthorized()` / `last10()` (lines 498-506) — phone number whitelist (last 10 digits). Only used from `pollIncomingSMS()` which is currently compiled out.

### 3b. `pollNtfyCommands` — critical analysis

**`lastCmdId` default seed:** Loaded from NVS preferences at line 823:
```cpp
lastCmdId = preferences.getString("lastCmdId", "0");
```
On first boot (or if NVS is empty), `lastCmdId = "0"`.

**Validation guard (lines 826-829):** Resets "now" or empty string to "0". Good — "now" is invalid for poll mode.

**HTTP request produced on first boot:**
```
GET http://ntfy.sh/antitheft-gonnie-2219-cmd/json?poll=1&since=0
```

**Does `since=0` work for poll subscriptions?** Yes. Per ntfy.sh documentation, `since=0` means "all cached messages" (typically last 12 hours). On first boot this will replay up to 12 hours of commands. This is **mostly harmless** because:
- The system starts DISARMED, so replaying an old ARM command would arm it (undesirable but not dangerous).
- An old PHOTO command would trigger a photo capture unnecessarily.
- The NVS persistence (`preferences.putString("lastCmdId", lastCmdId)`) means this only happens on the very first boot ever, or after NVS wipe.

**Bug: `since=0` replays stale commands.** After first boot, if there are old messages on the ntfy topic, they will all be executed. This is a real issue — see Problem Surface.

### 3c. Cloudflare Worker audit

**Endpoints:**
| Method | Path | Purpose |
|--------|------|---------|
| POST | `/ingest` | Fast-path alert from LILYGO — immediate WhatsApp |
| POST | `/sms/inbound` | Twilio SMS webhook — inbound commands |
| GET | `/` | Health check (returns JSON with last_poll_timestamp) |
| (cron) | every 1 minute | Poll ntfy, forward alerts to WhatsApp, process photos |

**HMAC verification (lines 514-535):** Twilio HMAC-SHA1 signature verification against `X-Twilio-Signature` header. Implementation looks correct: sorts params, concatenates URL + key/value pairs, HMAC-SHA1, base64 compare. One concern: uses `===` string comparison rather than constant-time comparison — theoretically vulnerable to timing attacks, but impractical against a Cloudflare Worker.

**KV usage:**
- `last_poll_timestamp` — numeric timestamp for ntfy poll cursor
- `msg:<id>` — per-message dedup with JSON `{whatsapp, sms}` state, 24h TTL
- `body:<hash>` — content-hash dedup for `/ingest` vs cron path, 24h TTL

**Twilio API calls:**
- `sendWhatsApp()` — POST to Twilio Messages API with `whatsapp:+...` From/To, optional MediaUrl
- `sendSMS()` — POST to Twilio Messages API with plain `+1...` From/To
- `analyzePhoto()` — POST to Vercel `/api/analyze` endpoint

**`VALID_COMMANDS` (line 21):** `["ARM", "DISARM", "STATUS", "PHOTO", "GPS", "HELP"]`

**Missing from Worker `VALID_COMMANDS`: `IMMOBILIZE` and `RESTORE`.** The LILYGO firmware handles these in `handleSMSCommand()` (lines 521-524), and the webapp dashboard sends them, but the Worker's inbound SMS handler will reject them as "Unknown command" if sent via SMS. This is a gap.

**`USE_TWILIO_SMS = false` (line 24):** Outbound SMS from the Worker is disabled. The `sendSMS()` function exists but is never called with real Twilio creds. All SMS `dedup.sms` fields are immediately set to `true` to suppress retry noise.

### 3d. SIM configuration

**Current firmware expects: Hologram SIM** — `APN[] = "hologram"` at `AntiTheftSystemLilygo.ino:37`, PDP type `"IP"` (IPv4 only) at line 756.

**Native AT+CMGS active?** No. `USE_NATIVE_SMS` is `0` at line 41. All SMS-related code (`sendSMS`, `pollIncomingSMS`, SMS init AT commands, URC handling) is inside `#if USE_NATIVE_SMS` blocks and is not compiled.

**SpeedTalk path:** The `claude_code_sms_feature_prompt.md` and `CHANGES.md` document a planned SpeedTalk migration. The `docs/ARCHITECTURE.md` references SpeedTalk/Wholesale APN. But **the actual firmware on disk uses Hologram**. The SpeedTalk changes described in `CHANGES.md` were either reverted or never applied to the current `.ino` files.

---

## 4. Webapp Audit

### 4a. Framework and routing

**Framework:** None — single static HTML file (`webapp/public/index.html`). No React, no Next.js, no build step. Served directly by Vercel as static content.

**Routing:**
- `/` → `public/index.html` (static)
- `/api/analyze` → `api/analyze.js` (Vercel serverless function, Node.js runtime)

### 4b. External services called

| Service | How | Auth |
|---------|-----|------|
| ntfy.sh SSE (`/sse`) | `EventSource` subscription to alert topic | None (public topic) |
| ntfy.sh poll (`/json?poll=1&since=24h`) | `fetch` GET on page load | None |
| ntfy.sh publish (commands) | `fetch` POST to cmd topic | None |
| ntfy.sh publish (email) | `fetch` POST with JSON body + `email` field | None |
| Vercel `/api/analyze` | `fetch` POST with `{imageUrl}` | None (same-origin) |
| Anthropic API (from `analyze.js`) | `@anthropic-ai/sdk` | `ANTHROPIC_API_KEY` env var |

**Auth concern:** The ntfy topics are public. Anyone who knows the topic name can read all alerts and send arbitrary commands. Dashboard login is client-side only (SHA-256 hash at line 248) and provides zero server-side protection.

### 4c. UI elements — wired vs. UI-only

| Element | Wired to backend? | How |
|---------|-------------------|-----|
| **Login form** | Client-side only | SHA-256 hash comparison (line 248). No server auth. |
| **ARM button** | Yes | POSTs "ARM" to ntfy cmd topic (line 735). LILYGO polls and forwards `SMS_CMD:ARM` to Main. |
| **DISARM button** | Yes | Same flow as ARM. |
| **GPS button** | Yes | POSTs "GPS" to cmd topic. LILYGO handles locally (AT+CGPSINFO), replies via ntfy. |
| **PHOTO button** | Yes | POSTs "PHOTO" to cmd topic. LILYGO sends `SMS_CMD:PHOTO` to Main, triggers full alarm pipeline. |
| **IMMOBILIZE button** | Yes | POSTs "IMMOBILIZE" to cmd topic (line 808). LILYGO forwards `SMS_CMD:IMMOBILIZE`. Main sets relay. **But:** Worker rejects this command via SMS — dashboard only. |
| **RESTORE button** | Yes | Same flow as IMMOBILIZE. Same Worker gap. |
| **Status pill** | Live | Infers state from ntfy message body keywords ("armed"/"disarmed") — lines 390-396. |
| **Ignition pill** | Live | Infers from keywords ("immobilized"/"ignition restored") — lines 399-406. |
| **Last seen** | Live | Tracks `lastMessageTime` from any ntfy message. Offline after 15 minutes. |
| **Map** | Live | Leaflet map, position extracted from `Location: lat,lon` in ntfy bodies. GPS trail persisted in localStorage. |
| **Hero photo** | Live | Shows latest photo attachment from ntfy stream. |
| **Event feed** | Live | SSE subscription, capped at 10 items in DOM (line 602), dedup via `seenMsgIds` Set. |
| **AI badge** | Live | On new photo attachment, POSTs to `/api/analyze`, shows threat level badge. |
| **Email alert** | Live | Auto-fires on urgent alerts, POSTs to ntfy with `email` field. Dedup via localStorage. |
| **Offline banner** | Live | Shows after 15 minutes of no messages (line 486: `elapsed > 900000`). |
| **Confirm dialog** | Yes | Two-step confirm for IMMOBILIZE/RESTORE (lines 805-816). |
| **Lightbox** | Yes | Click any photo to open fullscreen overlay. |
| **Toast** | Yes | Shows "Command sent" / "Connected" / error messages. |
| **Optimistic UI** | Yes | Buttons show spinner, status pill updates immediately, reverts on 20s timeout (line 743). |

### 4d. AI classification flow

- **Model:** `claude-sonnet-4-20250514` (`analyze.js:19`)
- **Prompt location:** Inline in `analyze.js:26` — "You are a security camera AI. Analyze this photo and classify the threat level as HIGH, MEDIUM, or LOW..."
- **Trigger:** Dashboard JS, on any new (non-historical) photo attachment with `.jpg`/`.jpeg` filename (`index.html:430`). Also triggered by Worker cron when photo is matched to alert (`worker.js:177-178`).
- **Flow:** Dashboard `analyzePhoto()` → `fetch('/api/analyze', {imageUrl})` → Vercel function fetches image → base64 encodes → sends to Anthropic Messages API → parses `THREAT_LEVEL: HIGH|MEDIUM|LOW - <reason>` → returns `{threatLevel, verdict}` → dashboard shows badge.
- **Dual invocation:** Both the dashboard AND the Worker independently analyze photos. The Worker result goes into WhatsApp messages; the dashboard result goes into the feed UI. They could produce different verdicts for the same photo.

---

## 5. Problem Surface

### TODOs / FIXMEs / HACKs / XXXs

**None found.** Grep for `TODO|FIXME|HACK|XXX` across the entire repo returned zero matches.

### Commented-out blocks (>3 lines)

**None found.** All comments in the `.ino` files are explanatory, not commented-out code. The `#if USE_NATIVE_SMS` guards serve the same function cleanly.

### Stale files with wrong content

**`docs/ARCHITECTURE.md` (lines 34, 36, 121-126):** References SpeedTalk, APN "Wholesale", SMS channel, 2-minute heartbeat, 5-second command poll, SMS rate limit, SMS safety-net poll. The actual firmware uses Hologram, no native SMS, 5-minute heartbeat/GPS, 5-second command poll.

**`ARCHITECTURE.md` (root, lines 36, 47, 120-124):** Also says command poll is 15s (actual: 5s), heartbeat is 6 hours (actual: 5 min). References "Battery monitoring" widget and "AT+CBC voltage-based estimation" — but no AT+CBC command exists anywhere in the current firmware. Battery monitoring is described in docs but **not implemented**.

**`firmware/CHANGES.md`:** Describes SpeedTalk migration changes that are not reflected in the current firmware. The firmware was either reverted to Hologram or this doc describes a planned future state.

---

## 6. Final Report

### Working (verified by code reading)

- **Sensor detection pipeline (Main ESP32):** Vibration (GPIO 13) and reed switch (GPIO 14) trigger `startAlarm()` when armed, with 5s debounce. `AntiTheftSystemMainESP.ino:188-196`.
- **RF remote arm/disarm:** RCSwitch on GPIO 15, codes 616609/616610. `AntiTheftSystemMainESP.ino:140-161`.
- **FreeRTOS dual-core alarm processing:** Core 0 alarm task with `lilygoMutex` protecting UART. `AntiTheftSystemMainESP.ino:199-292`. Clean task lifecycle (create, run, delete).
- **ESP32-CAM burst capture + SD save + UART stream:** 3-photo burst, stale frame discard, VGA JPEG, 512-byte chunked UART transfer. `AntiTheftSystemESP32CAM.ino:96-156`.
- **Main-to-LILYGO photo forwarding:** 50 KB buffer, 512-byte chunks, IMG/IMG_END framing. `AntiTheftSystemMainESP.ino:243-261`.
- **LILYGO modem boot sequence:** Full AT setup chain — echo off, CMEE=2, CFUN cycle, APN config, registration wait, PS attach, PDP activate, diagnostics, GPS on. `AntiTheftSystemLilygo.ino:705-857`.
- **ntfy.sh text POST via HTTPINIT:** `httpPostText()` with retry wrapper (3 attempts, exponential backoff, skip on 4xx). `AntiTheftSystemLilygo.ino:187-209, 248-263`.
- **ntfy.sh binary POST (photo upload):** `httpPostBinary()` with 1 KB chunks, retry wrapper. `AntiTheftSystemLilygo.ino:212-243, 265-280`.
- **ntfy command polling:** Every 5s, GET `?poll=1&since=<lastCmdId>`, JSON parse, command dispatch. NVS persistence of cursor. `AntiTheftSystemLilygo.ino:351-405`.
- **Command dispatch (ARM/DISARM/STATUS/PHOTO/GPS/HELP/IMMOBILIZE/RESTORE):** All 8 commands handled. ARM/DISARM/STATUS/PHOTO/IMMOBILIZE/RESTORE forwarded to Main via `SMS_CMD:`. GPS/HELP handled locally. `AntiTheftSystemLilygo.ino:508-536`.
- **Main ESP32 SMS command handler:** Processes all 6 forwarded commands, sends status updates and SMS_REPLY back. `AntiTheftSystemMainESP.ino:356-420`.
- **Immobilization relay:** GPIO 23, active LOW, NVS-persisted across reboots. Controlled via IMMOBILIZE/RESTORE commands. `AntiTheftSystemMainESP.ino:91-96, 397-419`.
- **Worker fast-path /ingest:** Receives direct POST from LILYGO, sends text-only WhatsApp immediately, writes dedup key. `worker.js:76-109`.
- **Worker cron safety net:** Every 60s polls ntfy, matches alerts+photos, AI analysis, WhatsApp with photo, dual-dedup with /ingest. `worker.js:111-276`.
- **Worker inbound SMS webhook:** Twilio HMAC-SHA1 verification, sender whitelist, command validation, posts to ntfy cmd topic, TwiML response. `worker.js:454-501`.
- **Dashboard SSE live feed:** EventSource to ntfy, dedup via Set, status/ignition inference from keywords, GPS map updates, photo display. `index.html:278-338`.
- **Dashboard commands (ARM/DISARM/GPS/PHOTO/IMMOBILIZE/RESTORE):** All 6 buttons wired, optimistic UI with spinner + 20s timeout + revert on failure. `index.html:716-773`.
- **Dashboard AI classification:** POSTs to `/api/analyze`, shows badge. `index.html:661-700`.
- **Vercel AI endpoint:** Fetches image, base64 encodes, Claude Sonnet 4 analysis, threat level parsing. `analyze.js:1-41`.
- **GPS tracking:** AT+CGPSINFO every 5 min, NMEA parsing, Google Maps link generation. `AntiTheftSystemLilygo.ino:410-433`.
- **Heartbeat:** Every 5 min, ntfy POST with timestamp + GPS. `AntiTheftSystemLilygo.ino:690-700`.
- **Dashboard history load:** On page load, fetches `?poll=1&since=24h` for recent history. `index.html:340-356`.
- **Dashboard offline detection:** 15-minute timeout, shows offline banner with last seen time and last GPS. `index.html:483-522`.
- **Dashboard email alerts:** Auto-sends email via ntfy on urgent alerts, dedup via localStorage. `index.html:624-659`.
- **Dashboard lightbox:** Click photo for fullscreen overlay. `index.html:609-615`.
- **Dashboard GPS trail:** Polyline on Leaflet map, last 50 points, persisted in localStorage. `index.html:524-537`.

### Partially working / known bugs

- **`since=0` replays stale commands on first boot.** On very first boot (no NVS), `lastCmdId="0"` causes `pollNtfyCommands()` to fetch all cached messages (up to 12h). Old ARM/DISARM/PHOTO commands will execute. `AntiTheftSystemLilygo.ino:823`. **Root cause:** Using `"0"` as initial seed fetches all history; should use `"all"` (return nothing) or current timestamp.
  ```diff
  - lastCmdId = preferences.getString("lastCmdId", "0");
  + lastCmdId = preferences.getString("lastCmdId", "all");
  ```
  **Correction:** ntfy `since=all` returns all messages too. The correct value to return nothing in poll mode is a current unix timestamp. But the LILYGO doesn't know the current timestamp before talking to the modem. A pragmatic fix: after modem boot, before enabling the poll loop, set `lastCmdId` to a value from `AT+CCLK?` converted to unix timestamp. Alternatively, skip command processing for the first poll after a fresh NVS init.

- **Worker rejects IMMOBILIZE/RESTORE via SMS.** `VALID_COMMANDS` at `worker.js:21` is `["ARM", "DISARM", "STATUS", "PHOTO", "GPS", "HELP"]`. Texting "IMMOBILIZE" or "RESTORE" to the Twilio number returns "Unknown command." Dashboard sends these fine because it POSTs directly to ntfy. `worker.js:21`.
  ```diff
  - const VALID_COMMANDS = ["ARM", "DISARM", "STATUS", "PHOTO", "GPS", "HELP"];
  + const VALID_COMMANDS = ["ARM", "DISARM", "STATUS", "PHOTO", "GPS", "HELP", "IMMOBILIZE", "RESTORE"];
  ```

- **ARCHITECTURE.md / docs/ out of sync with firmware.** Multiple parameter mismatches:
  - `docs/ARCHITECTURE.md:34` says "Speedtalk SIM, APN Wholesale" — firmware says Hologram.
  - `docs/ARCHITECTURE.md:36` says "Every 5s, polls...sends REMOTE_ARM/REMOTE_DISARM" — firmware sends `SMS_CMD:ARM`. The `REMOTE_ARM`/`REMOTE_DISARM` protocol messages are documented in both ARCHITECTURE files but **never sent by the LILYGO firmware**. Only `REQUEST_PHOTO` is handled by the Main ESP32 (`AntiTheftSystemMainESP.ino:171`).
  - `docs/ARCHITECTURE.md:121-122` says heartbeat=2min, command poll=5s — firmware has heartbeat=5min, poll=5s (this one matches).
  - Root `ARCHITECTURE.md:120` says heartbeat=6 hours — firmware has 5 minutes. Root `ARCHITECTURE.md:121` says command poll=15000ms — firmware has 5000ms.
  - Both ARCHITECTURE files reference "Battery monitoring" / AT+CBC — no AT+CBC command exists in the firmware.

- **SMS_CMD dropped during alarm.** When `alarmInProgress=true`, the Main ESP32's loop skips the LILYGO drain block (`AntiTheftSystemMainESP.ino:166`). During the 30-90 second alarm window, the alarm task (Core 0) reads `SerialLilyGO` looking only for `LILYGO_OK`/`LILYGO_ERROR` (`AntiTheftSystemMainESP.ino:272-278`). Any `SMS_CMD:` that arrives during this window is read by the alarm task and silently discarded (it doesn't match `LILYGO_OK`/`LILYGO_ERROR`, so it's just printed to Serial and ignored). Documented in `CHANGES.md` as known limitation #1.

- **HTTP cleartext (not HTTPS) for all ntfy traffic.** `httpPostText()` uses `http://ntfy.sh/...` (line 189). Alert content, GPS coordinates, and photos are sent unencrypted. The `httpInit()` function does toggle `AT+HTTPSSL` based on URL scheme (line 165), and the Worker ingest URL uses `https://` (line 48), but all ntfy posts use `http://`. The comment at lines 105-109 acknowledges this as an accepted trade-off.

- **Worker HTTPS to ntfy, LILYGO HTTP to ntfy.** The Worker uses `https://ntfy.sh/...` (line 284) while the LILYGO uses `http://ntfy.sh/...` (line 189). This is inconsistent but not broken — ntfy.sh accepts both.

- **Dashboard auth is client-side only.** The SHA-256 hash comparison at `index.html:248` can be bypassed by anyone who opens DevTools. The ntfy topics, command channel, and `/api/analyze` endpoint have no server-side authentication. Anyone who discovers the topic name can read all alerts and send arbitrary commands.

- **LILYGO `handleSMSCommand()` does not send SMS_CMD for GPS/HELP.** GPS and HELP are handled locally by the LILYGO (lines 526-534) via `httpPostText()` to the ntfy alert topic. This means:
  - GPS response goes to ntfy (visible on dashboard, WhatsApp) but NOT back to the sender as an SMS reply (unless the Worker picks it up as a "Command Reply").
  - Actually, the GPS handler doesn't post as `SMS_REPLY:` — it posts directly with `Title: Command Reply` header. Since the LILYGO uses HTTPINIT which **strips custom headers** (per memory notes), this ntfy message arrives without a title. The Worker's cron filter at line 142-147 looks for `m.message.startsWith("SMS_REPLY:")` or `m.message.startsWith("Command Reply")` — the GPS response body starts with `"GPS: 42.35..."`, which matches neither. **GPS command replies never reach the user via SMS or WhatsApp.**

- **LILYGO RX buffer size discrepancy between old and new firmware.** The old `ESP32Lilygo.txt` set `Serial2.setRxBufferSize(1024)` (line 110). The current `AntiTheftSystemLilygo.ino` sets it to `16384` (line 719). The 16 KB buffer is correct for receiving 50 KB photos — but worth noting the change.

- **`alarmInProgress` is `volatile bool` but used across cores without atomic guarantee.** On ESP32, `bool` reads/writes are atomic at the hardware level (32-bit aligned), so this works in practice. But it's technically a data race by C++ standards. Not a real bug on this platform.

### Not implemented

- **Battery monitoring (AT+CBC).** Referenced in `ARCHITECTURE.md:94`, `docs/ARCHITECTURE.md:95`, and `README.md:211`. **No AT+CBC command exists anywhere in the firmware.** No battery percentage is ever sent in any ntfy message. The dashboard has CSS for a battery widget in the ARCHITECTURE doc but no corresponding HTML/JS in the actual `index.html`. **Completely absent.**

- **Native SMS outbound (USE_NATIVE_SMS=1).** All code exists behind `#if USE_NATIVE_SMS` but is compiled out. The `sendSMS()`, `pollIncomingSMS()`, SMS init, URC handler, and rate limiter are all present in source but inactive. Would need `USE_NATIVE_SMS` set to `1` and a SIM that supports SMS (SpeedTalk, not Hologram) to activate.

- **Outbound SMS from Worker (USE_TWILIO_SMS=false).** `worker.js:24`: `const USE_TWILIO_SMS = false`. The `sendSMS()` function exists, `TWILIO_SMS_FROM`/`TWILIO_SMS_TO` secrets are documented, but outbound SMS is disabled. Memory notes say "Outbound SMS broken at carrier level (Twilio trial + T-Mobile A2P 10DLC)."

- **HTTPS for ntfy.sh uploads.** Comment at `AntiTheftSystemLilygo.ino:105-109` acknowledges HTTP-only. `AT+HTTPSSL=1` is toggled by `httpInit()` based on URL scheme, but no ntfy URL uses `https://`. Would need to change URLs from `http://ntfy.sh/` to `https://ntfy.sh/` and test SIM7600 TLS handshake.

- **OTA firmware updates.** Listed in `README.md:255` as future work. No OTA code exists.

- **Multi-zone sensors.** Listed in `README.md:257` as future work. Only vibration (GPIO 13) and reed (GPIO 14) exist.

- **Network recovery on LILYGO.** The old `ESP32Lilygo.txt` had an `ensureNetwork()` function (lines 64-82) with `AT+NETOPEN` recovery. The current firmware (`AntiTheftSystemLilygo.ino`) has **no network recovery function** — it relies entirely on the HTTPINIT subsystem managing its own connections. If the PDP context drops or the modem loses registration, HTTP requests will fail with no automatic recovery. The HTTPINIT retry wrapper (3 attempts with backoff) provides some resilience but cannot recover from a lost PDP context.

- **REMOTE_ARM / REMOTE_DISARM protocol messages.** Documented in both ARCHITECTURE files as LILYGO-to-Main messages, but the current LILYGO firmware **never sends them**. The `handleSMSCommand()` function uses `SMS_CMD:ARM` / `SMS_CMD:DISARM` instead. The Main ESP32 does handle `REQUEST_PHOTO` (line 171) but not `REMOTE_ARM` or `REMOTE_DISARM`. This protocol was likely from an earlier version and was superseded by the `SMS_CMD:` convention.

- **Modem UART mutex on LILYGO.** `claude_code_sms_feature_prompt.md` (line 67) flagged that `SerialAT` is shared between the command poller, HTTP code, and GPS poller without a mutex. Since the LILYGO is single-core single-task, this isn't a bug — but it means all operations are serialized. During the ~30-60 second alert handler (HTTP posts + photo upload), no commands are polled. This is by design but means command latency can spike to 60+ seconds during alerts.

---

## Summary

| Category | Count |
|----------|-------|
| Working | 26 items |
| Partially working / known bugs | 11 items |
| Not implemented | 8 items |

**Highest-priority items for next phase (SMS bridge):**
1. Fix Worker `VALID_COMMANDS` to include IMMOBILIZE/RESTORE (one-line fix).
2. Decide on `since=0` first-boot behavior — currently replays stale commands.
3. GPS command replies don't reach user via any channel (header stripping + body format mismatch).
4. All ntfy traffic is HTTP cleartext.
5. No network recovery on the HTTPINIT-based firmware (old TCP firmware had `ensureNetwork()`).
