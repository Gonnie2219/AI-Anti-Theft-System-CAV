# SMS Test Plan

Validates the SMS bridge fixes and the ntfy→Worker KV command path migration.

## Architecture (current)

```
INBOUND COMMANDS:
  Phone SMS → Twilio → Worker /sms/inbound → KV "cmd_queue" → LILYGO polls /commands/poll
  Dashboard → Worker /commands/dispatch    → KV "cmd_queue" → LILYGO polls /commands/poll

OUTBOUND ALERTS (unchanged):
  Sensors → Main ESP → LILYGO → ntfy alert topic → Worker cron → WhatsApp/SMS
                              → Worker /ingest    → WhatsApp (fast path)
  Dashboard SSE still subscribes to ntfy alert topic.
```

**RETIRED:** ntfy command topic `antitheft-gonnie-2219-cmd` is no longer used. Commands now flow through Worker KV. ntfy is only used for alerts, photos, status, and heartbeats.

## Prerequisites

### Reflash LILYGO
1. Open `firmware/AntiTheftSystemLilygo.ino` in Arduino IDE.
2. Board: **ESP32 Wrover Module** (PSRAM enabled).
3. Partition scheme: **Huge APP (3MB No OTA/1MB SPIFFS)**.
4. Upload speed: 115200.
5. Flash and open Serial Monitor at 115200 baud.
6. Wait for `LILYGO_READY` and `[CMD] first boot — seeding cursor`.

### Redeploy Worker
```bash
cd worker
npx wrangler deploy
```
Verify with `npx wrangler tail` — keep this running during all tests below.

### Redeploy Dashboard
```bash
cd webapp
npx vercel --prod
```
Or push to GitHub — Vercel auto-deploys from master.

---

## Test 1 — IMMOBILIZE and RESTORE via SMS (Fix 1)

**What was broken:** Worker rejected IMMOBILIZE and RESTORE as invalid commands.

### Steps
1. Dispatch `IMMOBILIZE` via Worker (simulates SMS or dashboard):
   ```bash
   curl -X POST -d "IMMOBILIZE" https://antitheft-whatsapp-bridge.gonnie2219.workers.dev/commands/dispatch
   ```
2. Watch `wrangler tail` — should show `{"status":"queued","command":"IMMOBILIZE",...}`.
3. Watch LILYGO Serial Monitor — should show `[CMD] worker: IMMOBILIZE`.
4. Main ESP Serial Monitor should show `SMS command: IMMOBILIZE` and `Ignition immobilized via command`.
5. Repeat with `RESTORE`.

### Pass criteria
- Worker returns 200 with `"status":"queued"` for both commands.
- LILYGO: `[CMD] worker: IMMOBILIZE` and `[CMD] worker: RESTORE` appear.
- Main ESP: relay toggles, NVS persists state across reboot.

### Fail symptoms
- Worker returns 400 → VALID_COMMANDS not updated (Fix 1 not deployed).
- LILYGO never sees command → HTTPS/SSL issue or Worker URL mismatch.

---

## Test 2 — GPS and HELP replies reach the Worker (Fix 2)

**What was broken:** GPS/HELP reply bodies lacked the `SMS_REPLY:` prefix. Also, the loop() UART handler stripped the prefix from Main ESP replies before posting to ntfy.

### Steps
1. Dispatch `GPS` command:
   ```bash
   curl -X POST -d "GPS" https://antitheft-whatsapp-bridge.gonnie2219.workers.dev/commands/dispatch
   ```
2. Watch LILYGO Serial Monitor for `[CMD] worker: GPS` and the httpPostText call.
3. Watch `wrangler tail` for the cron picking up the reply. Look for `Reply <id>: sms=true`.
4. Check WhatsApp — you should receive the GPS coordinates (or "GPS: no fix").
5. Repeat with `HELP` — WhatsApp should show the command list.

### Pass criteria
- `wrangler tail` logs show the reply message body starts with `SMS_REPLY:`.
- WhatsApp receives clean text (prefix stripped): `GPS: 40.xxx,-74.xxx https://maps.google.com/...` or `Commands: ARM DISARM ...`.

### Fail symptoms
- `wrangler tail` shows no reply processing → body missing `SMS_REPLY:` prefix → Fix 2 not flashed.
- WhatsApp shows `SMS_REPLY: GPS:...` with prefix visible → Worker prefix-stripping not deployed (Fix 4).

---

## Test 3 — First-boot command replay (Fix 3)

**What was broken:** On power-up, stale commands were executed. With the new Worker KV queue (5min TTL), this is less likely but still possible.

### Steps
1. Dispatch a few test commands while the LILYGO is powered off:
   ```bash
   curl -X POST -d "ARM" https://antitheft-whatsapp-bridge.gonnie2219.workers.dev/commands/dispatch
   curl -X POST -d "PHOTO" https://antitheft-whatsapp-bridge.gonnie2219.workers.dev/commands/dispatch
   ```
2. Power on the LILYGO within 5 minutes (before TTL expires). Watch Serial Monitor.
3. First poll should show:
   ```
   [CMD] first boot — seeding cursor
   [CMD] skipped (first boot): ARM
   [CMD] skipped (first boot): PHOTO
   ```
4. System should NOT arm and should NOT trigger a photo capture.
5. After the first poll completes, dispatch a new command:
   ```bash
   curl -X POST -d "STATUS" https://antitheft-whatsapp-bridge.gonnie2219.workers.dev/commands/dispatch
   ```
6. This should execute normally: `[CMD] worker: STATUS`.

### Pass criteria
- Stale commands logged as `skipped (first boot)` — none executed.
- `lastCmdId` advances past the stale batch.
- Subsequent commands execute normally.

### Fail symptoms
- Serial shows `[CMD] worker: ARM` (no "skipped") → Fix 3 not flashed.
- System arms on boot → stale ARM command replayed.
- No commands work after boot → cursor not advancing (check `newLatestId` logic).

---

## Test 4 — SMS outbound plumbing (Fix 4)

**What was broken:** SMS was hardcoded as delivered in the dedup record, formatSMSMessage ignored photo URLs, reply bodies included the `SMS_REPLY:` protocol prefix, and Twilio errors were logged as opaque strings.

### Verifying with USE_TWILIO_SMS = false (current state)

1. Trigger an alarm (vibration sensor or dispatch PHOTO command).
2. Watch `wrangler tail`:
   - `/ingest` should log `sms: true` in the response (disabled = auto-success).
   - Cron should NOT retry SMS for this alert.
3. Dispatch `STATUS` command. Watch cron process the reply:
   - Should log `Reply <id>: sms=true` (auto-marked delivered).
   - Body in logs should NOT contain `SMS_REPLY:` prefix.

### Verifying with USE_TWILIO_SMS = true (after 10DLC approval)

1. In `worker/worker.js`, change `const USE_TWILIO_SMS = false;` to `true`.
2. Deploy: `npx wrangler deploy`.
3. Trigger an alarm.
4. Watch `wrangler tail` for one of:
   - `Twilio SMS BLOCKED code=30034 (10DLC campaign pending)` — expected before approval.
   - `[ingest] SMS sent` — 10DLC approved, SMS delivered.
5. Dispatch `STATUS` command. Cron should attempt SMS for the reply.
6. Check your phone for the SMS.

### Pass criteria
- With `USE_TWILIO_SMS = false`: no Twilio API calls in logs, dedup records mark sms=true.
- With `USE_TWILIO_SMS = true`: Twilio API called, structured error logged (30034 before approval, 201 after).
- Reply SMS text is clean (no `SMS_REPLY:` prefix).
- Alert SMS is single-line, ≤160 chars.

### Fail symptoms
- Cron retries SMS endlessly → dedup `sms` field not being set → check /ingest smsOk logic.
- SMS body shows `SMS_REPLY: System armed` → prefix stripping not deployed.
- Twilio error logged as `Twilio SMS error 403: [object Object]` → structured logging not deployed.

---

## Test 5 — Main ESP command replies reach the user (Fix 2, UART handler)

**What was broken:** When the Main ESP sent `SMS_REPLY:System armed` over UART, the LILYGO stripped the prefix before posting to ntfy. The body arrived as `System armed` instead of `SMS_REPLY: System armed`, so the Worker never matched it as a reply.

### Steps
1. Dispatch `ARM` command:
   ```bash
   curl -X POST -d "ARM" https://antitheft-whatsapp-bridge.gonnie2219.workers.dev/commands/dispatch
   ```
2. LILYGO forwards `SMS_CMD:ARM` to Main ESP via UART.
3. Main ESP replies with `SMS_REPLY:System armed` over UART.
4. Watch LILYGO Serial Monitor for `[REPLY] System armed`.
5. Watch `wrangler tail` — cron should pick up the reply and log `Reply <id>: sms=true`.
6. Check WhatsApp — you should receive "System armed" (prefix stripped by Worker).
7. Repeat with `DISARM`, `STATUS`, `IMMOBILIZE`, `RESTORE`.

### Pass criteria
- ntfy message body starts with `SMS_REPLY:` (visible in `wrangler tail` raw message).
- WhatsApp/SMS text is clean (no `SMS_REPLY:` prefix) — Worker strips it.
- All five Main-ESP-routed commands produce visible replies.

### Fail symptoms
- `wrangler tail` never processes the reply → body missing prefix → Fix 2 UART handler not flashed.
- WhatsApp shows `SMS_REPLY: System armed` with prefix → Worker prefix-stripping not deployed (Fix 4).

---

## Test 6 — Worker KV command queue (new architecture)

**What changed:** Commands no longer flow through ntfy command topic. They go through Worker KV queue.

### Verify endpoints
```bash
# Poll (should return empty or recent commands)
curl -s "https://antitheft-whatsapp-bridge.gonnie2219.workers.dev/commands/poll?since=0"

# Dispatch
curl -s -X POST -d "STATUS" "https://antitheft-whatsapp-bridge.gonnie2219.workers.dev/commands/dispatch"

# Poll again (should include STATUS command)
curl -s "https://antitheft-whatsapp-bridge.gonnie2219.workers.dev/commands/poll?since=0"
```

### Verify inbound SMS → KV queue
1. Send `STATUS` SMS from your phone.
2. Watch `wrangler tail` — `/sms/inbound` should NOT show `ntfy cmd post failed: 429`.
3. Instead it should show `Command queued: STATUS`.
4. LILYGO should pick it up within 5 seconds: `[CMD] worker: STATUS`.

### Verify dashboard buttons
1. Open dashboard at `webapp-seven-livid-86.vercel.app`.
2. Click ARM button.
3. Network tab should show POST to `antitheft-whatsapp-bridge.gonnie2219.workers.dev/commands/dispatch` (not ntfy.sh).
4. Response should be `{"status":"queued","command":"ARM",...}`.
5. LILYGO picks up command within 5 seconds.

### Pass criteria
- No ntfy 429 errors on inbound SMS.
- Commands arrive at LILYGO within 5-10 seconds.
- Dashboard buttons work without ntfy dependency.
- CORS headers present (`Access-Control-Allow-Origin: *`).

### Fail symptoms
- LILYGO shows `[CMD] poll HTTP 0` or timeout → HTTPS/SSL handshake failing with Worker.
- Commands never arrive → KV eventual consistency issue (check `wrangler tail` for queue contents).
- Dashboard buttons fail with CORS error → check browser console, verify OPTIONS handler deployed.

---

## Quick smoke test (autonomous, no phone needed)

Run these from a terminal to verify the full pipeline without sending a real SMS:

```bash
# 1. Verify Worker health
curl -s https://antitheft-whatsapp-bridge.gonnie2219.workers.dev | python3 -m json.tool

# 2. Dispatch and poll
ID=$(curl -s -X POST -d "STATUS" https://antitheft-whatsapp-bridge.gonnie2219.workers.dev/commands/dispatch | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
echo "Dispatched: $ID"
sleep 2
curl -s "https://antitheft-whatsapp-bridge.gonnie2219.workers.dev/commands/poll?since=0"

# 3. Verify CORS
curl -s -I -X OPTIONS https://antitheft-whatsapp-bridge.gonnie2219.workers.dev/commands/dispatch | grep -i access-control

# 4. Verify invalid command rejected
curl -s -X POST -d "INVALID" https://antitheft-whatsapp-bridge.gonnie2219.workers.dev/commands/dispatch
```

Expected: dispatch returns queued, poll returns the command, OPTIONS returns CORS headers, invalid command returns 400.
