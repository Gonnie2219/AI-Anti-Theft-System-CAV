# System Review -- 2026-06-24

## (a) Executive Summary

The system is in good shape. All three canonical firmware sketches are byte-identical to their repo mirrors. The five recent commits (3a7428b through 0b71331) are all pushed to origin/master, and the working tree is clean.

**Power-efficiency work** is the headline: GPS gated on armed state, LED duty-cycling from 100% to 2.5%, and event-driven motion pushes. All hardware-verified.

One **critical bug** found: `IMG_BUF_SIZE` mismatch between Main (65536) and LILYGO (51200), introduced in commit 3a7428b when Main was bumped but LILYGO was not. Photos between 50-64 KB would be accepted by Main but silently rejected by LILYGO.

**7 Important issues** documented below, including the long-standing STATUS suppression during alarm, a theoretical MQTT URC loss during split serial reads, an unauthenticated AI analysis endpoint, and IMMOBILIZE_REJECTED notifications not reaching WhatsApp/Web Push.

The pill state machine, command dispatch, Worker queue logic, and AI analysis endpoint are all structurally sound.

---

## (b) Repo & Git State

| Item | Value |
|------|-------|
| Branch | `master` |
| Ahead of origin | 0 commits |
| Behind origin | 0 commits |
| Uncommitted changes | None |
| Untracked | `.wrangler/` (build artifact, expected) |
| Latest commit | `0b71331` feat(firmware/LILYGO): GPS gated on armed state + event-driven motion status |

All 5 commits from the previous session are now pushed:

```
0b71331 feat(firmware/LILYGO): GPS gated on armed state + event-driven motion status
5d3be64 feat(firmware/Main): LED blink + event-driven motion push
62db41f fix(dashboard): restore confirm dialog + handler after IMMOBILIZE/RESTORE timeout-retry
80a73ea Move dashboard MQTT config to gitignored config.js
3a7428b Fix CAM photo path: single-frame capture + RX-safe handoff
```

Planning docs read: `SESSION_STATE_MQTT.md`, `MQTT_MIGRATION_PLAN.md`, `docs/PILL_STATE_SPEC.md`, `docs/ARCHITECTURE.md`, `firmware/CHANGES.md`.

---

## (c) Canonical-File Integrity

| Sketch | Canonical path | Repo mirror | Status |
|--------|---------------|-------------|--------|
| LILYGO | `C:\Users\gonni\ArduinoFiles\AntiTheftSystemLilygo\AntiTheftSystemLilygo.ino` | `firmware/AntiTheftSystemLilygo.ino` | **Identical** (after `--strip-trailing-cr`) |
| Main ESP32 | `C:\Users\gonni\ArduinoFiles\AntiTheftSystemMainESP\AntiTheftSystemMainESP.ino` | `firmware/AntiTheftSystemMainESP.ino` | **Identical** |
| ESP32-CAM | `C:\Users\gonni\ArduinoFiles\AntiTheftSystemESP32CAM\AntiTheftSystemESP32CAM.ino` | `firmware/AntiTheftSystemESP32CAM.ino` | **Identical** |

No drift. All canonical sketches match their repo mirrors.

---

## (d) Recent Accomplishments

### Power Efficiency (front and center)

#### 1. GPS Gated on Armed State
- **Commit:** `0b71331` (LILYGO firmware)
- **Status:** Committed, hardware-verified Jun 23
- **Mechanism:** GPS engine (`AT+CGPS`) starts OFF at boot (`lilygo.ino:81` `gpsOn = false`, `:1399`). Enabled on `STATUS:ARMED` (`:1230-1233`), disabled on `STATUS:DISARMED` (`:1234-1238`). `IMMOBILIZED` and `IGNITION_OK` are intentionally ignored so GPS stays active through the arm-to-immobilize theft-tracking sequence.
- **Edge case:** `handleAlert()` at `:1159-1163` turns GPS on if still off (covers boot-while-armed).
- **Guard:** `updateGPS()` at `:862` early-returns when `!gpsOn` -- no AT error spam, last-known lat/lon preserved, speed cleared to -1.0.
- **Savings:** GPS module draws ~25-50 mA when active. Now off during idle (disarmed) state.

#### 2. LED Duty-Cycling (Main ESP32)
- **Commit:** `5d3be64` (Main firmware)
- **Status:** Committed, hardware-verified Jun 23
- **Mechanism:** Non-blocking blink at `main.ino:376-388`. Status LEDs flash for 50 ms every 2000 ms instead of staying solid. At `:380-387`, timer resets when `blinkElapsed >= 2000` (turn ON), then at `:384-387` turns OFF once `blinkElapsed >= 50`.
- **Arm/disarm confirmation:** `lastLedBlinkMs = millis()` resets in RF remote handler (`:313`, `:324`) and SMS command handler (`:650`, `:663`), giving a full 50 ms flash on state change for visual feedback.
- **Savings:** LED current drops from 100% to 2.5% duty cycle (~20 mA per LED to ~0.5 mA).

#### 3. Event-Driven Motion Status Push
- **Commits:** `5d3be64` (Main), `0b71331` (LILYGO)
- **Status:** Committed, hardware-verified Jun 23
- **Mechanism (Main):** `motionStateChanged` flag set at `main.ino:298-299` on STATIONARY/MOVING transition (skips initial UNKNOWN). Checked at `:363` -- triggers immediate UART push instead of waiting for the periodic 5s/30s interval.
- **Mechanism (LILYGO):** At `lilygo.ino:1504-1514`, MOTION: payload is parsed, and when `motionState != prevMotion && prevMotion != "UNKNOWN"`, an immediate `sendStatusUpdate()` fires.
- **Result:** Dashboard motion latency reduced from ~45s to ~4-7s.

### Other Recent Accomplishments

#### 4. CAM Single-Frame Capture + RX-Safe Handoff
- **Commit:** `3a7428b`
- **Status:** Committed, hardware-verified Jun 23
- **Changes:**
  - CAM: replaced 3-frame burst with 2 warmup frames (AE/AWB settle) + 1 keeper (`cam.ino:35`, `:101-113`). `jpeg_quality` 10 to 12. RX drain at end of `takePhoto()` (`:150`).
  - Main: `IMG_BUF_SIZE` 51200 to 65536 (`main.ino:81`). PHOTO command moved to after buzzer for RX-safe timing (`:473-474`). Separate `CAM_TIMEOUT` vs `CAM_GARBAGE` diagnosis.
  - Eliminated phantom second burst and wrong-frame ambiguity.

#### 5. Dashboard MQTT Config to Gitignored config.js
- **Commit:** `80a73ea`
- **Status:** Committed
- **Details:** MQTT credentials extracted to `webapp/public/config.js` (gitignored). Dashboard gracefully falls back to HTTP dispatch when `config.js` is 404 (production). MQTT path is local-dev-only.

#### 6. Confirm Dialog Fix for IMMOBILIZE/RESTORE
- **Commit:** `62db41f`
- **Status:** Committed, deployed to Vercel prod
- **Fix:** `failAction()` at `index.html:1306-1311` stores `originalOnclick` (which includes `confirmImmobilize`/`confirmRestore`) and restores it on retry. The confirm dialog is not lost after a timeout.

---

## (e) Bug Review by Severity

### Critical

#### C1. IMG_BUF_SIZE Mismatch (Main vs LILYGO)

- **Main:** `#define IMG_BUF_SIZE 65536` (`main.ino:81`)
- **LILYGO:** `#define IMG_BUF_SIZE 51200` (`lilygo.ino:44`)
- **Introduced:** Commit `3a7428b` bumped Main from 51200 to 65536 but did not update LILYGO.
- **Impact:** If the CAM produces a JPEG between 50-64 KB, Main accepts it and sends `IMG:55000` to LILYGO. LILYGO's `receiveImage()` at `:1118` checks `expected > IMG_BUF_SIZE` and returns false -- photo is silently dropped. The alert goes through but without the photo. At VGA quality 12, typical JPEGs are 25-45 KB, but detailed/bright scenes can exceed 50 KB.
- **Fix:** Bump LILYGO's `IMG_BUF_SIZE` to 65536 (and its `malloc()` at `:1289`). LILYGO has PSRAM, so 64 KB is easily affordable. Also update `ARCHITECTURE.md` Key Parameters table (currently says "50KB").

### Important

#### I1. STATUS Suppression During alarmInProgress (Main)

- **Location:** `main.ino:316` and `:327`
- **Behavior:** `if (!alarmInProgress) SerialLilyGO.println("STATUS:ARMED/DISARMED");`
- **Impact:** If the owner uses the RF remote to arm/disarm while an alarm task is running on Core 0, the STATUS message is silently dropped. The cloud and dashboard never learn about the state change. This can leave the dashboard showing the wrong armed state.
- **Note:** SMS_CMD-based disarm during alarm IS handled (alarm task at `:549` forwards DISARM to `handleSMSCommand` which does send STATUS). Only the RF remote path is affected.
- **Existing:** Documented in `PILL_STATE_SPEC.md:83` as "V7: firmware-deferred."
- **Fix:** Queue the STATUS message and send it after `alarmInProgress` clears, or have the alarm task check and re-send the current armed state on exit.

#### I2. LILYGO Armed State Not Synced at Boot

- **Location:** `lilygo.ino:80` (`systemArmed = false`), `:1399-1400`
- **Behavior:** LILYGO always boots with `systemArmed = false` and `gpsOn = false`. It relies on STATUS:ARMED from Main to learn the armed state. But Main only sends STATUS on STATE CHANGES (RF remote or SMS command), not at boot.
- **Impact:** If the system boots while armed (NVS on Main), LILYGO doesn't know it's armed until the first arm/disarm event. GPS stays off (wrong for armed state). Command poll cadence uses `lastActivityMs` (boot counts as activity, so fast-poll for 10 min mitigates), but after 10 min it goes to slow poll while armed.
- **Mitigated by:** `handleAlert()` turns GPS on if `!gpsOn`; boot = activity for fast-poll window.
- **Fix:** Have Main send `STATUS:ARMED` or `STATUS:DISARMED` to LILYGO during boot, after `LILYGO_READY` is received (around `main.ino:241`).

#### I3. drainSerialAT() Discards Partial MQTT URC Blocks

- **Location:** `lilygo.ino:163-169`
- **Behavior:** If `+CMQTTRXSTART` is found but `+CMQTTRXEND` hasn't arrived yet, the code does `atBuffer = atBuffer.substring(0, rxIdx)` -- keeping data before the MQTT marker and discarding the partial block. Those bytes were already consumed from the serial port and cannot be re-read.
- **Impact:** If a MQTT URC arrives across two `drainSerialAT()` calls (e.g., during a tight HTTP wait loop), the command is permanently lost. At 115200 baud, a typical ARM URC (~120 bytes) takes ~10 ms. The probability depends on the timing alignment between URC arrival and drain calls (10-20 ms intervals in wait loops).
- **Severity:** Low probability in practice (short URCs usually arrive atomically), but the loss is silent and unrecoverable.
- **Fix:** Preserve the partial block in atBuffer instead of discarding it. Change line 168 from `atBuffer = atBuffer.substring(0, rxIdx)` to keeping the MQTT data: leave atBuffer intact and only return the non-MQTT prefix to callers via a separate mechanism.

#### I4. Worker system_status Uses Single Shared Timestamp

- **Location:** `worker.js:578` (`stored.ts = msg.time * 1000`)
- **Behavior:** The `system_status` KV object has one `ts` field bumped by any "System X" message (ARMED, DISARMED, IMMOBILIZED, IGNITION_OK). The dashboard's pill state machine R2 uses this single ts for both status and ignition dimensions.
- **Impact:** If ARMED arrives at T1 and IMMOBILIZED arrives at T2 > T1, KV ts becomes T2. A dashboard poll sees both dimensions at ts=T2. The status dimension's ts appears fresher than it really is. However, the `value-differs` guard in R2 (`obs.statusValue !== pillState.status`) prevents spurious updates when the value hasn't changed.
- **Severity:** Mitigated by the value guard. Could cause incorrect pending-command resolution in an unlikely edge case where both dimensions change simultaneously. The spec (PILL_STATE_SPEC.md) implicitly assumes per-dimension timestamps but doesn't mandate them in KV.
- **Fix:** Split `system_status.ts` into `statusTs` and `ignitionTs` in the Worker. Update `handleStatusPoll` and `applyObservation R2` to use per-dimension timestamps.

#### I5. /api/analyze Endpoint Lacks Authentication

- **Location:** `webapp/api/analyze.js:5-6`
- **Behavior:** The endpoint checks `req.method !== 'POST'` but has no authentication. Anyone who discovers the URL can POST image URLs and consume Anthropic API credits.
- **Impact:** Cost exposure. The SSRF protection (ntfy.sh hostname check at `:14`) limits the attack surface — an attacker can only analyze ntfy.sh images, not arbitrary URLs. But they can run up the API bill.
- **Fix:** Add `CMD_SECRET` auth check:
  ```js
  const secret = req.headers['x-cmd-secret'] || '';
  if (secret !== process.env.CMD_SECRET) return res.status(401).json({ error: 'Unauthorized' });
  ```
  Then pass the secret from the dashboard's `analyzePhoto()` function.

#### I6. Worker Doesn't Forward IMMOBILIZE_REJECTED to WhatsApp/Web Push

- **Location:** `worker.js:414-431` (cron message categorization)
- **Behavior:** The cron classifies messages as alerts (`ALERT:`), photos (`.jpg` attachment), or replies (`SMS_REPLY:` / `Command Reply`). The LILYGO's "Safety Lockout" message (`lilygo.ino:1519-1525`) has body starting with `"Immobilize refused"` — it doesn't match any category, so it's never forwarded to WhatsApp or Web Push.
- **Impact:** The safety lockout notification reaches the dashboard (via ntfy SSE) but NOT WhatsApp or push notifications. If the user isn't watching the dashboard, they won't know the immobilize was rejected.
- **Mitigated by:** The dashboard detects it via SSE body content (`index.html:672`) and shows a banner.
- **Fix:** Add a safety-lockout category to the cron handler, or have the LILYGO prefix the body with `SMS_REPLY:` so the existing reply path catches it.

#### I7. Worker "Command Reply" Body Check Is Dead Code

- **Location:** `worker.js:430`
- **Behavior:** `m.message.startsWith("Command Reply")` never matches because "Command Reply" is the ntfy Title header, not the message body. The body always starts with `"SMS_REPLY:"`. The `m.message.startsWith("SMS_REPLY:")` check at `:429` correctly catches all replies.
- **Impact:** None — the dead branch is harmless. But it suggests a past misunderstanding of the ntfy data model that could cause confusion in future edits.
- **Fix:** Remove the dead `|| m.message.startsWith("Command Reply")` clause.

### Minor

#### M1. Offline Banner innerHTML Could Be XSS Vector

- **Location:** `index.html:888` (`banner.innerHTML = html`)
- **Behavior:** GPS coordinates from `localStorage.getItem('lastGPS')` are interpolated into `html` at `:882-883` and set via `.innerHTML`.
- **Impact:** If localStorage is tampered (requires existing XSS), arbitrary HTML could be injected. In practice, GPS data is from parsed floats which can't contain HTML, so this is a theoretical self-XSS only.
- **Fix:** Use `textContent` or sanitize the GPS parts. Or build the banner with DOM methods instead of string concatenation.

#### M2. GPS Queried Twice When Heartbeat and GPS Update Align

- **Location:** `lilygo.ino:1253` (heartbeat calls `updateGPS()`), `:1547-1550` (periodic GPS update)
- **Behavior:** Both `HEARTBEAT_MS` and `GPS_UPDATE_MS` are 300000 (5 min). When they align, `updateGPS()` is called twice in quick succession. Second call returns the same data.
- **Impact:** Wastes one AT command cycle (~50 ms). Functionally harmless.
- **Fix:** Skip `updateGPS()` in heartbeat if `now - lastGPS < 30000` (recently updated).

#### M3. MQTT Reconnect During HTTP Poll Race

- **Location:** `lilygo.ino:1465-1472` (MQTT reconnect) vs `:1482-1486` (HTTP poll)
- **Behavior:** MQTT reconnect and HTTP poll both execute in the same loop iteration if conditions are met. MQTT reconnect involves multiple AT commands (DISC, STOP, START, CONNECT, SUB) that can take 10-20 seconds. During this time, the HTTP poll's timer has advanced, potentially triggering immediately after.
- **Impact:** No functional issue -- just a burst of modem activity. The `alarmInProgress` and `SerialMain.available()` guards prevent the worst case.
- **Fix:** Add `now = millis()` after `connectMQTT()` (already present at `:1471`), and reset `lastCmdPoll` to prevent an immediate double-poll.

#### M4. CAM SD Write Failure Not Propagated

- **Location:** `cam.ino:122` (`if (written != sendFb->len) Serial.println("CAM_ERROR: SD write incomplete")`)
- **Behavior:** SD write failure is logged but the photo is still streamed over UART. The Main and LILYGO receive the image normally.
- **Impact:** Cosmetic only -- the SD card is an archival copy, not the primary delivery path. The UART stream comes directly from the framebuffer.

#### M5. ARCHITECTURE.md Key Parameters Table Outdated

- **Location:** `docs/ARCHITECTURE.md:125` says "Image buffer | 50KB"
- **Reality:** Main is 65536 (64 KB) since commit 3a7428b.
- **Fix:** Update to "Image buffer | 64 KB (Main) / 50 KB (LILYGO)" -- and fix LILYGO per C1.

---

## (f) Known-Issue Status Table

| # | Issue | Status | Notes |
|---|-------|--------|-------|
| 1 | First-boot stale-command (since=0 returning cached commands) | **FIXED** | `lilygo.ino:754`: `skipExecution = (lastCmdId == "0")` skips all commands on first poll, then advances cursor. `worker.js:909-911` also seeds `kvLatest = Date.now()` when cursor is 0 and no commands exist. |
| 2 | STATUS suppression while alarmInProgress | **STILL PRESENT** | `main.ino:316,327`. RF remote arm/disarm during alarm doesn't send STATUS to LILYGO. SMS_CMD path at `:549-550` IS handled. See I1 above. |
| 3 | CAM warmup / first-frame behavior | **FIXED** | Commit `3a7428b`. 2 warmup frames + 1 keeper. Hardware-verified. |
| 4 | MQTT drainSerialAT() parsing wrong URC format | **CORRECTED** | `lilygo.ino:155-219` now parses `+CMQTTRXSTART/RXTOPIC/RXPAYLOAD/RXEND` multi-line URCs (correct for SIM7600G firmware LE20B05). The old `+CMQTTRECV` URC format (which doesn't exist on this modem) is gone. |
| 5 | Worker VALID_COMMANDS missing IMMOBILIZE/RESTORE | **FIXED** | `worker.js:33`: `["ARM", "DISARM", "STATUS", "PHOTO", "GPS", "HELP", "IMMOBILIZE", "RESTORE"]` |
| 6 | GPS/HELP reply-format mismatch (SESSION_STATE_MQTT.md line 27) | **N/A** | Body-content-based detection works correctly. LILYGO posts GPS/HELP replies with body starting `"SMS_REPLY: GPS:"` / `"SMS_REPLY: Commands:"`. Worker cron at `:429` matches `m.message.startsWith("SMS_REPLY:")`. Dashboard uses body content for all classification (`:634` comment: "SIM7600 HTTPINIT strips Title/Priority headers"). The referenced `worker.js:142` line has shifted -- now in the b64url utility functions. No current mismatch. |
| 7 | Double-photo-burst | **FIXED** | Commit `3a7428b`. Single keeper frame. `takePhoto()` drains RX at end (`:150`) to prevent phantom re-trigger. |
| 8 | IMMOBILIZE_REJECTED handling | **WORKING** | Main rejects at `main.ino:700-707`, sends `IMMOBILIZE_REJECTED:MOVING,...` to LILYGO. LILYGO at `:1516-1526` posts "Safety Lockout" to ntfy. Dashboard at `index.html:670-674` detects by body content and shows banner. |

---

## (g) Recommended Next Steps

### High Priority

1. **Fix C1 (IMG_BUF_SIZE mismatch):** Bump `lilygo.ino:44` from 51200 to 65536. Update `ARCHITECTURE.md` Key Parameters table. Reflash LILYGO, verify a photo > 50 KB lands on the dashboard.

2. **Fix I2 (armed-state sync at boot):** After the boot handshake loop in `main.ino` (~line 241), send the current armed/immobilized state to LILYGO:
   ```
   SerialLilyGO.println(armed ? "STATUS:ARMED" : "STATUS:DISARMED");
   if (immobilized) SerialLilyGO.println("STATUS:IMMOBILIZED");
   ```
   This ensures LILYGO's GPS gating, poll cadence, and `systemArmed` flag are correct from boot.

3. **MQTT soak and evaluation:** SESSION_STATE_MQTT.md notes that MQTT has been verified but is running in parallel with HTTP poll. The HTTP poll is the dominant data cost. Evaluate removing it once MQTT reliability is confirmed.

### Medium Priority

4. **Fix I1 (STATUS suppression during alarm):** Add a `statusPending` flag that queues the STATUS message. After `alarmInProgress = false` at `main.ino:568`, check and send the queued STATUS.

5. **Fix I3 (partial MQTT URC discard):** Restructure `drainSerialAT()` to preserve partial MQTT blocks between calls. The simplest approach: don't truncate at rxIdx -- leave the full buffer intact, and use a separate "callerBuffer" that excludes the MQTT region.

6. **Fix I5 (analyze auth):** Add `CMD_SECRET` auth to `/api/analyze.js` and pass it from the dashboard's `analyzePhoto()` fetch call.

7. **Fix I6 (safety lockout forwarding):** Either add a "safety lockout" category in the Worker cron, or prefix the LILYGO's "Immobilize refused" body with `SMS_REPLY:` so the existing reply path catches it.

8. **Split KV timestamp (I4):** Separate `system_status.ts` into `statusTs` and `ignitionTs` in the Worker for correct per-dimension pill resolution.

### Low Priority

9. **Update ARCHITECTURE.md** to reflect all recent changes (GPS gating, LED blink, event-driven motion, single-frame CAM, MQTT config).

10. **Remove dead code (I7):** `worker.js:430` -- `m.message.startsWith("Command Reply")` never matches (title vs body confusion). Remove the clause.

11. **Rotate HiveMQ password** (noted in SESSION_STATE_MQTT.md -- printed in clear text in dev screenshots).

12. **Clean up SMS surfaces** if 10DLC is permanently abandoned (opt-in link already removed per commit `11ff4a0`, but consent.js/terms/privacy SMS refs may remain).

---

*Report generated 2026-06-24. Read-only audit -- no code changes made.*
