# SpeedTalk Migration + Power Optimization — Firmware Changes

**Date:** May 3, 2026
**Author:** Gonnie Ben-Tal (with Claude as pair-coder)
**Affected files:**
- `AntiTheftSystemLilygo.ino` (major rewrite)
- `AntiTheftSystemMainESP.ino` (small but important fix)
- `AntiTheftSystemESP32CAM.ino` (unchanged — included for completeness)

## Goals

1. Switch the cellular gateway from the Hologram SIM (IPv4 data, no SMS) to the SpeedTalk SIM (IPv6-only data, native SMS).
2. Restore SMS bidirectional capability (send alerts; receive ARM / DISARM / STATUS / PHOTO / GPS / HELP commands).
3. Keep the existing ntfy.sh data path alive so the web dashboard and the Cloudflare Worker → WhatsApp bridge keep working.
4. Cut idle-state power consumption substantially. The previous firmware was polling and heartbeating so aggressively that the modem essentially never rested.

## Context: why this matters

SpeedTalk Mobile confirmed by email (May 1, 2026) that their network is **IPv6-only** with no IPv4 or dual-stack PDP context provisioning available. SMS rides on the cellular signaling layer (independent of IP) so it's unaffected. But the ntfy.sh data path connects via raw IPv4 (`159.203.148.75`) so it depends on the modem doing 464XLAT or NAT64 transparently.

The firmware now tries IPv4 first (if 464XLAT is in play this just works), and falls back to the well-known NAT64 prefix (`64:ff9b::/96` mapped onto our IPv4 → `64:ff9b::9fcb:944b`). The `tcpConnect()` function already had this two-attempt logic from the May 1 work; we kept it.

---

## File-by-file changes

### `AntiTheftSystemESP32CAM.ino`

**No changes.** The CAM firmware is a pure UART slave that responds to `PHOTO` commands and is independent of the cellular layer. Included in the bundle for completeness.

---

### `AntiTheftSystemMainESP.ino`

Two changes, both small but real:

1. **Removed destructive UART drain inside the `SMS_CMD:` handler (line 167 in the old file).** The old code had:
   ```cpp
   } else if (r.startsWith("SMS_CMD:")) {
     String smsCmd = r.substring(8);
     while (SerialLilyGO.available()) SerialLilyGO.read();  // drain stale data
     handleSMSCommand(smsCmd);
   }
   ```
   That drain discarded any back-to-back `SMS_CMD:` lines or queued status replies. Removed.

2. **Boot wait extended from 25s to 60s.** The LILYGO modem boot sequence (CFUN cycle, registration, NETOPEN) can take 40–50s, especially if the cell signal is marginal. The old 25s timeout meant the Main ESP32 would print "LILYGO did not report ready" on slow boots and start operating before the gateway was actually online.

3. **Photo receive timeout relaxed from 10s → 15s.** Generous margin for the CAM's 3-frame burst + SD writes before UART transfer starts. Fixed inaccurate comment about this.

Everything else (RF remote handling, dual-core alarm task, mutex pattern, `SMS_REPLY:` contract back to LILYGO) is unchanged.

---

### `AntiTheftSystemLilygo.ino` — major rewrite

The old file had three different categories of issues. I'll list every change with rationale.

#### A. SpeedTalk SIM correctness

| Change | Old | New | Why |
|---|---|---|---|
| APN | `"hologram"` | `"Wholesale"` | SpeedTalk's MVNO APN — confirmed from their support email and the confirmation in user memory. |
| PDP context type | `"IP"` (IPv4-only) | `"IPV4V6"` (dual-stack) | SpeedTalk says they're IPv6-only and won't provision IPv4. We request dual-stack so the modem can negotiate whatever the network actually offers. If the modem provides 464XLAT transparently we still get a usable IPv4 socket. |
| `AT+CGCONTRDP=1` diagnostic | unchanged | unchanged | Already prints what context types we got. Useful for confirming v4/v6 status at boot. |
| `AT+CGPADDR=1` diagnostic | unchanged | unchanged | Prints actual assigned address(es). |

#### B. SMS — restored from stub

The old file had `sendSMS()`, `sendAlertSMS()`, `checkSMSNotifications()`, and `checkUnreadSMS()` all stubbed out with "SMS disabled on Hologram SIM" early returns. The handler chain below them (`readAndProcessSMS`, `isAuthorizedSender`, `handleSMSCommand`, `waitForMainReply`) was real and complete. The SMS init AT commands in `setup()` were commented out.

All four stubs are now real implementations:

**`sendSMS(to, body)`** — text-mode AT+CMGS:
1. Rolling-hour rate limit check (`SMS_MAX_PER_HOUR = 10`). Returns false silently if exceeded.
2. Caps body at 320 chars (sanity).
3. Re-asserts `AT+CMGF=1` and `AT+CSCS="GSM"` on every call (cheap, and survives any modem state confusion after a `CFUN` cycle or auto-recovery).
4. Drains stale modem output, issues `AT+CMGS="<to>"`, waits up to 5s for `>` prompt.
5. Sends body, then `SerialAT.write((uint8_t)26)` for Ctrl+Z. The explicit `(uint8_t)` cast is what fixed the May 1 echo bug (previously the body was being written by `println` which added a stray `\r\n` that triggered modem echo even with `ATE0`).
6. Waits up to 30s for `+CMGS:` and `OK`. Increments `smsCount` on success.

**`sendAlertSMS(reason)`** — composes a short alarm SMS:
```
ALERT: <reason>
<modem_time>
https://maps.google.com/?q=<lat>,<lon>
Photo: ntfy.sh/<topic>
```
Typically ~140 chars — fits in one SMS in most cases. The Maps URL can push it over 160 GSM-7 chars; if so, the modem sends a 2-part concatenated SMS (still arrives as one message on the receiving phone).

**`checkSMSNotifications()`** — primary path for `+CMTI: "ME",N` URCs:
- Non-blocking peek into `SerialAT` (only reads what's currently available, up to 512 chars or 200ms).
- Scans the buffer for `+CMTI:` URC patterns, extracts the index, calls `readAndProcessSMS(idx)`.
- Best-effort by design: if the URC arrives mid-`sendAT()` it will be eaten by that call's read loop. The CMGL safety-net catches those.

**`checkUnreadSMS()`** — safety-net poll, runs every 30s:
- Issues `AT+CMGL="REC UNREAD"`.
- Parses each `+CMGL: <idx>,...` line, calls `readAndProcessSMS(idx)`.
- Catches anything `checkSMSNotifications` missed.

**SMS init in `setup()`** (previously commented out, now active):
```cpp
sendAT("AT+CMGF=1",                "OK", 2000);   // text mode
sendAT("AT+CSCS=\"GSM\"",          "OK", 2000);   // GSM-7 character set
sendAT("AT+CPMS=\"ME\",\"ME\",\"ME\"", "OK", 3000); // SMS storage in modem
sendAT("AT+CNMI=2,1,0,0,0",        "OK", 2000);   // URC on new SMS
sendATWait("AT+CMGD=1,4", 5000);                    // wipe stale messages from previous boots
```

The `CPMS="ME","ME","ME"` line is an addition compared to the old commented-out block — it forces SMS storage in the modem's RAM rather than on the SIM card. RAM is faster to access via `CMGL` and avoids SIM-storage quirks where some carriers don't allow application-level SMS deletion.

The `CMGD=1,4` line wipes all stored SMS at boot. Without this, any messages sitting in storage from a previous power cycle would be re-processed on boot, which would (a) potentially toggle ARM state in unexpected ways and (b) waste SMS quota replying to old commands.

**Rate limiter — wired up.** The old file declared `smsCount` and `smsWindowStart` but never checked them. Now `sendSMS()` enforces a 10-message-per-rolling-hour cap. Protects against runaway SMS floods if a sensor gets stuck or a malicious actor spams the system into triggering alarms.

#### C. Power optimizations

Specific changes in priority order:

| Setting | Old | New | Reduction | Annual cellular savings |
|---|---|---|---|---|
| `POLL_INTERVAL_MS` (ntfy command poll) | 5,000 ms | 60,000 ms | **12×** | The big one. 720 polls/hr → 60 polls/hr. |
| `HEARTBEAT_MS` | 120,000 ms | 1,800,000 ms | **15×** | 30 heartbeat HTTPs/hr → 2/hr. |
| `GPS_UPDATE_MS` (read coordinates over AT bus) | 30,000 ms | 300,000 ms | **10×** | Doesn't save modem power — the GPS receiver runs continuously regardless — but cuts AT-bus contention. |

Why this matters: each ntfy poll is a full TCP open + GET + close cycle, ~1s of active radio time at ~150mA. At 5-second intervals the modem effectively never goes to idle. At 60-second intervals it has 59 seconds of idle time between cellular wakes. This roughly halves average current draw in the quiescent state.

**Boot startup TCP test removed.** The old setup did:
1. TCP connectivity test
2. Close connection
3. Open new connection for startup notification HTTP POST
4. Close
5. SMS

That's two unnecessary TCP cycles. The startup notification HTTP POST IS the connectivity test — if it succeeds we know HTTP works; if it fails we log "FAILED" and continue. SMS doesn't depend on data network so it goes out regardless.

**Alert handler no longer redundantly re-updates GPS.** Old code did `updateGPS()` unconditionally on every alarm. Now: `if (gpsLat.length() == 0) updateGPS();`. The periodic loop already keeps GPS fresh.

**`sendStatusNotification()` and `sendHeartbeat()` no longer force `updateGPS()`.** Same rationale — the periodic loop keeps it fresh.

#### D. Code-quality improvements

- Forward declarations added for all functions (cleaner compile, allows arbitrary call ordering).
- Boot banner now prints the active APN — instant visual confirmation when watching serial monitor.
- Comments distinguish "best-effort" paths from "must-succeed" paths so future readers know which failures matter.

---

## Post-review bugfixes (May 3, 2026)

Three bugs found during full code review:

1. **Serial2 RX buffer overflow during alarm (CRITICAL).** `sendAlertSMS()` blocked for 5-30s on SerialAT before `receiveImage()` read the photo from Serial2. The ~50KB photo arriving during that window overflowed the 16KB Serial2 RX buffer. **Fix:** reordered to `receiveImage()` first, then `sendAlertSMS()`. SMS still goes out before the HTTP upload.

2. **`ensureNetwork()` didn't restore `networkReady` on modem auto-recovery (MEDIUM).** If the network dropped and recovery failed, `networkReady` was set to `false`. If the modem later re-registered on its own, the early-return path in `ensureNetwork()` didn't set `networkReady = true`, so all HTTP calls would fail permanently. **Fix:** added `networkReady = true` on the early-return path.

3. **`sendAlertSMS()` sent a spurious "ALERT" SMS for user-requested photos (LOW).** The old guard `if (reason == "Photo Requested") return;` was dropped during the rewrite. Without it, a PHOTO SMS command produced 3 SMS (acknowledgement + alert + photo link) instead of 2. **Fix:** restored the guard.

---

## Compatibility / non-changes

The following protocols, contracts, and behaviors are **unchanged**. Anything that depends on these (the WhatsApp bridge worker, the web dashboard, the Main ESP32 firmware, the CAM firmware) does not need any changes.

- UART protocols between boards: `ALERT:`, `IMG:`, `IMG_END`, `NOIMG`, `STATUS:`, `LILYGO_READY`, `LILYGO_OK`, `LILYGO_ERROR`, `REMOTE_ARM`, `REMOTE_DISARM`, `REQUEST_PHOTO`, `SMS_CMD:`, `SMS_REPLY:`, `CAM_READY`, `PHOTO`.
- ntfy.sh topic names (`antitheft-gonnie-2219`, `antitheft-gonnie-2219-cmd`).
- Dashboard command vocabulary (ARM, DISARM, GPS, PHOTO).
- SMS command vocabulary (ARM, DISARM, STATUS, PHOTO, GPS, HELP).
- Phone number whitelist mechanism (last-10-digit match).
- Photo capture format (VGA, JPEG quality 10, 3-frame burst, last frame transmitted).
- Battery voltage → percent piecewise mapping.
- Image buffer size (50 KB).

## Known limitations carried over from previous firmware

These are pre-existing issues that I did **not** fix in this pass. Flagging them for future work:

1. **SMS commands during alarm processing are dropped.** The Main ESP32's alarm task on Core 0 reads `SerialLilyGO` looking only for `LILYGO_OK` / `LILYGO_ERROR`. Other lines (including `SMS_CMD:`) are read and discarded. A user who texts `STATUS` while an alarm is being processed will get no reply for that command. Workaround: send the command again after the alarm completes (~30–60 s).
2. **AT-stream URC eating.** A `+CMTI` URC that arrives mid-AT-command can be consumed by the in-flight `sendAT()` call. The 30 s `CMGL` safety-net poll catches these, so worst-case latency for an inbound SMS command is ~30 s.
3. **Heap fragmentation.** The firmware uses Arduino `String` heavily. For long-running operation (days/weeks) this can fragment the heap. Not observed in testing but worth monitoring.
4. **No deep sleep.** Even with the polling reductions, the modem stays in radio-active mode continuously. Real battery life optimization (going from days to weeks) would require deep-sleep modes synchronized with the modem's PSM/eDRX features. That's a substantial rearchitecture, not in scope here.

---

## Verification checklist (for live testing)

Once flashed, please confirm in this order:

1. **Boot:** LILYGO serial monitor shows `APN: Wholesale`, registration completes, `CGCONTRDP` prints contain `IPV4V6` or both an IPv4 and IPv6 address.
2. **Startup notification:** ntfy dashboard shows "System Startup" notification within ~30s of boot.
3. **Startup SMS:** owner phone receives "Anti-theft system online." SMS within ~30s.
4. **TESTSMS:** type `TESTSMS` in the LILYGO USB serial — owner phone receives test message.
5. **Inbound SMS:** text `STATUS` to the system phone (`+13132081968`). Owner receives reply within ~30s (could be near-instant if the URC isn't eaten).
6. **Alarm flow:** trigger reed switch while armed. Confirm:
   - Owner receives alarm SMS with GPS Maps link
   - ntfy dashboard shows ALERT notification
   - WhatsApp message arrives via Cloudflare bridge
   - Photo notification appears on ntfy
7. **Dashboard command:** send `ARM` from the dashboard. System should arm within ~60s (was 5s; this is the trade-off).
8. **Rate limiter:** trigger many alarms in succession. After 10 SMS in an hour, subsequent SMS should be skipped with a `[SMS] Rate limited` log line. ntfy notifications continue normally.

If step 1 shows only an IPv6 address (no IPv4) and step 2/4 fails, the modem isn't doing 464XLAT and we need to test the NAT64 fallback path explicitly. The serial log will say `[TCP] IPv4 failed` followed by `[TCP] Connected via NAT64` (or `NAT64 failed` if SpeedTalk doesn't advertise the well-known prefix).
