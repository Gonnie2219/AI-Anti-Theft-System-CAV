# MQTT Command-Path Migration Plan

**Baseline fallback commit:** `0b810ff` (reliable HTTP polling — `git checkout 0b810ff -- firmware/AntiTheftSystemLilygo.ino` to roll back)

**Blocker note:** Cloudflare KV daily write quota was exhausted during bench testing (~34k ops/day from polling); resets 00:00 UTC. MQTT migration eliminates this entirely.

**Status:** DESIGN COMPLETE, not yet implemented. Implement in a fresh session against a reset KV quota.

---

## 1. Broker Choice

**Recommendation: HiveMQ Cloud (free tier)**

| Criterion | HiveMQ Cloud | EMQX Cloud | Cloudflare Pub/Sub |
|-----------|-------------|------------|-------------------|
| Free tier | 100 connections, 10 GB/mo | 1M session-min/mo, 1 GB traffic | Beta, Workers-only, no AT+CMQTT compatibility |
| TLS (8883) | Yes, Let's Encrypt | Yes | N/A (WebSocket-only from Workers) |
| REST publish API | Yes (`/api/v1/mqtt/publish`, basic-auth) | Yes (HTTP API plugin) | No REST -- must publish from within a Worker binding |
| Setup | 2-click cluster creation, instant credentials | Similar | Requires wrangler CLI, beta caveats |
| Client cert required? | No -- username/password auth over TLS | No | N/A |

**Cloudflare Pub/Sub is ruled out** -- it's a Workers binding, not a standard MQTT broker. The SIM7600's AT+CMQTT* can only connect to a standard MQTT 3.1.1 / 5.0 broker over TCP/TLS. There's no way to bridge AT+CMQTT to a Cloudflare binding without an intermediary, which defeats the purpose.

**HiveMQ Cloud wins** because: free tier is generous for a single device, REST publish API lets the Worker fire-and-forget on dispatch, and it's the most mature hosted broker with the least setup friction.

### Connection Parameters

```
Host:       <cluster-id>.s1.eu.hivemq.cloud   (assigned on creation)
Port:       8883 (TLS)
Client ID:  antitheft-lilygo-01
Username:   antitheft-device       (created in HiveMQ console)
Password:   <generated>            (stored in secrets.h)
```

### Topics

```
cmd/antitheft/commands   -- Worker publishes, LILYGO subscribes (QoS 1)
cmd/antitheft/ack        -- LILYGO publishes command ack (optional, QoS 0)
```

Short topic names minimize per-message overhead. The `cmd/` prefix scopes the command namespace away from any future telemetry topics.

---

## 2. Device-Side AT Sequence

### One-time setup (in `setup()`, after SSL config and network attach)

```
AT+CMQTTSTART                                    -> OK
AT+CMQTTSSLCFG=0,0                               -> OK  (link MQTT client 0 to SSL context 0,
                                                          which already has our TLS1.2/no-cert/SNI config)
AT+CMQTTCONNECT=0,"tcp://<host>:8883","antitheft-lilygo-01",60,1,"antitheft-device","<password>"
                                                  -> +CMQTTCONNECT: 0,0  (success)
                                                  -> keepalive=60s, clean_session=1 initially (see section 4)
AT+CMQTTSUB=0,"cmd/antitheft/commands",1          -> +CMQTTSUB: 0,0  (subscribed, QoS 1)
```

The `AT+CSSLCFG` settings from `setup()` (lines 1097-1100: TLS1.2, authmode=0, ignorelocaltime=1, enableSNI=1) apply to SSL context 0, which is the same context `AT+CMQTTSSLCFG=0,0` binds to the MQTT client. No additional SSL config needed.

### Steady-state -- command arrives as a URC

```
+CMQTTRECV: 0,25,3
cmd/antitheft/commands
ARM
```

Format: `+CMQTTRECV: <client>,<topic_len>,<payload_len>\r\n<topic>\r\n<payload>`

### Parsing (new function `handleMqttRecv`)

```
1. Detect "+CMQTTRECV:" in serial buffer
2. Parse topic_len and payload_len from the URC header
3. Read <topic_len> bytes -> topic string
4. Read <payload_len> bytes -> payload string
5. If topic == "cmd/antitheft/commands":
     handleSMSCommand(payload)   // reuse existing -- ARM/DISARM/etc. all work as-is
```

`handleSMSCommand()` (line 785-817) already does the complete dispatch: uppercase, log, `SerialMain.println("SMS_CMD:...")`. It's transport-agnostic. The only difference is the entry point -- MQTT URC instead of `pollWorkerCommands()` JSON parse.

---

## 3. The URC Interleaving Problem (Critical)

### The problem

Five functions read from `SerialAT` in a greedy loop:

| Function | Line | What it accumulates | What it looks for |
|----------|------|---------------------|-------------------|
| `sendAT()` | 122-131 | `resp` | The `expect` string (usually "OK") |
| `waitHttpAction()` | 197-217 | `acc` | `"+HTTPACTION:"` |
| `waitDownloadPrompt()` | 220-229 | `r` | `"DOWNLOAD"` or `"ERROR"` |
| `waitOK()` | 232-241 | `r` | `"OK"` or `"ERROR"` |
| `httpReadBody()` | 479-500 | `acc` | `"+HTTPREAD:"` then `"OK"` |

Every one of these does `while (SerialAT.available()) resp += (char)SerialAT.read()`, consuming **all** pending bytes. If a `+CMQTTRECV` URC arrives while `waitHttpAction()` is waiting for `+HTTPACTION:`, the MQTT data gets slurped into `acc`, recognized as neither `+HTTPACTION:` nor a timeout, and silently discarded when the function returns.

### Proposed solution: centralized read-and-dispatch layer

Replace the per-function greedy reads with a single `drainSerialAT()` function that:

1. Reads all available bytes from `SerialAT` into a **persistent accumulator** (a global `String atBuffer`)
2. Scans for complete URC lines
3. If it finds `+CMQTTRECV:` -- extracts it, queues the command into a single-slot buffer (`String mqttPendingCmd`)
4. If it finds `+CMQTTCONNLOST:` -- sets `mqttConnected = false`
5. Returns the remaining buffer (non-MQTT data) to the caller for their own pattern matching

### New globals

```cpp
String atBuffer;                     // persistent serial accumulator
String mqttPendingCmd;               // queued MQTT command (single slot -- commands are rare)
bool   mqttConnected = false;
unsigned long lastMqttReconnect = 0;
```

### New function

```cpp
void drainSerialAT() {
    while (SerialAT.available()) atBuffer += (char)SerialAT.read();

    // Scan for +CMQTTRECV and extract it
    int mqIdx = atBuffer.indexOf("+CMQTTRECV:");
    if (mqIdx >= 0) {
        // parse topic_len, payload_len, extract payload, store in mqttPendingCmd
        // remove the consumed bytes from atBuffer
    }

    // Scan for +CMQTTCONNLOST and flag reconnect
    int lostIdx = atBuffer.indexOf("+CMQTTCONNLOST:");
    if (lostIdx >= 0) {
        mqttConnected = false;
        // remove consumed bytes
    }
}
```

### Modified existing functions

Each function's greedy `while (SerialAT.available()) resp += (char)SerialAT.read()` becomes:

```cpp
// Before (e.g. waitHttpAction, line 201):
while (SerialAT.available()) acc += (char)SerialAT.read();

// After:
drainSerialAT();
acc += atBuffer;
atBuffer = "";
```

The caller still gets all the non-MQTT serial data in its accumulator and searches for its own URC as before. MQTT URCs are intercepted and queued before the caller ever sees them.

### Functions that need this change (5 total)

| Function | Line(s) to change | Notes |
|----------|--------------------|-------|
| `sendAT()` | 122 (pre-drain) + 127 (read loop) | Line 122 currently discards pre-existing data -- must drain-and-dispatch instead of discard |
| `waitHttpAction()` | 201 | Main risk point -- this waits up to 60s for POST responses |
| `waitDownloadPrompt()` | 224 | Short-lived, but still at risk |
| `waitOK()` | 236 | Short-lived |
| `httpReadBody()` | 479 (pre-drain) + 485 (read loop) | Reads body data -- MQTT URC could interleave mid-body |

### In `loop()` -- how the queued command gets processed

```cpp
void loop() {
    unsigned long now = millis();

    // MQTT command -- check the queue (replaces HTTP poll block entirely)
    if (mqttPendingCmd.length() > 0) {
        String cmd = mqttPendingCmd;
        mqttPendingCmd = "";
        handleSMSCommand(cmd);
    }

    // MQTT reconnect if needed
    if (!mqttConnected && now - lastMqttReconnect > 10000) {
        mqttReconnect();
        lastMqttReconnect = now;
    }

    // ... rest of loop (UART from Main, heartbeat, status, GPS) unchanged
}
```

The HTTP poll block (`pollWorkerCommands()` + `httpTerm()` + the adaptive cadence logic) is **deleted entirely**. Replaced by the `mqttPendingCmd` check -- zero latency, zero data cost, no HTTP.

**Why a single-slot queue is sufficient:** Commands are dispatched one at a time by a human pressing a button. The MQTT URC arrives, gets queued, and the very next `loop()` iteration (50ms later) picks it up. Even rapid button-mashing can't overflow a single slot at this rate.

---

## 4. Reconnect / Resilience

### Detection

The SIM7600 emits `+CMQTTCONNLOST: 0,<reason>` when the broker connection drops. `drainSerialAT()` catches this and sets `mqttConnected = false`.

### Reconnect strategy

```
if (!mqttConnected && now - lastMqttReconnect > 10000) {  // 10s backoff
    ensureNetwork();
    AT+CMQTTCONNECT=0,"tcp://<host>:8883","antitheft-lilygo-01",60,0,"user","pass"
                                                              // clean_session=0
    AT+CMQTTSUB=0,"cmd/antitheft/commands",1
    mqttConnected = true;
    lastMqttReconnect = now;
}
```

### Keepalive interval

Set to **60 seconds** in `AT+CMQTTCONNECT`. This is well within the 30-120s carrier NAT timeout window. The modem sends MQTT PINGREQ every 60s -- each is 2 bytes. Cost: 2 bytes x 1/min = 120 bytes/hour = **0.00012 MB/hour** -- effectively zero.

If the NAT drops at 30s (aggressive), lower keepalive to 25s. Still negligible data. Tune after field observation.

### QoS and session flags

- **QoS 1** for the command subscription. Broker guarantees at-least-once delivery. If the device is briefly disconnected, the broker queues messages and delivers them on reconnect (provided the session is persistent).
- **clean_session=0** (persistent session) on reconnect. The broker remembers the subscription and queues QoS 1 messages while the client is offline. On the *first* connect after flashing new firmware, use `clean_session=1` to clear any stale session state, then `clean_session=0` thereafter. The firmware tracks this with a boolean in NVS.
- **What happens to a command published during reconnect:** The broker holds it in the persistent session queue. When the LILYGO reconnects with `clean_session=0`, the broker replays the queued message immediately. The device receives it as a normal `+CMQTTRECV` URC. No command is lost.

---

## 5. Worker Change

### `handleCommandsDispatch` -- KV write to MQTT publish

```js
// Current (worker.js line 941):
const id = await enqueueCommand(env, body, "dashboard");  // KV write

// New:
const resp = await fetch(`https://<cluster>.s1.eu.hivemq.cloud/api/v1/mqtt/publish`, {
    method: "POST",
    headers: {
        "Content-Type": "application/json",
        "Authorization": "Basic " + btoa(env.HIVEMQ_USER + ":" + env.HIVEMQ_PASS),
    },
    body: JSON.stringify({
        topic: "cmd/antitheft/commands",
        payload: body,   // "ARM", "DISARM", etc.
        qos: 1,
    }),
});
```

### KV writes eliminated

`enqueueCommand()` (the `cmd_queue` KV write) is removed from the dispatch path. The `system_status` KV write for optimistic dashboard update (lines 947-951) stays -- it's one write per command (infrequent), not one per poll cycle.

### Should we keep KV as fallback?

No. The KV quota exhaustion is caused by the per-poll writes/reads, not the per-command writes. Removing the polling path eliminates the problem entirely. Keep the `system_status` KV (read by dashboard `GET /status`) -- it's 1 write per command + 1 read per 5s dashboard poll, well within quota.

### `handleCommandsPoll` and `/commands/poll` endpoint

Can be kept temporarily for migration safety (see section 6) or removed entirely post-cutover. The LILYGO won't call it anymore.

### Impact on KV quota

- Before: ~17,000 KV reads/day (5s poll x 17hr) + ~17,000 KV writes/day (cursor updates) = **34,000 ops/day**
- After: ~0 KV ops from the command path (only `system_status` reads from dashboard)

---

## 6. Migration Safety

### Recommendation: hard cutover with committed fallback

Belt-and-suspenders (running both MQTT and HTTP poll) is **not recommended** because:

1. The URC interleaving fix (section 3) touches the same `SerialAT` read functions used by the HTTP poll. Running both paths on the same serial port doubles the interleaving surface area.
2. The HTTP poll's `httpTerm()` per-cycle could interfere with the MQTT connection (both share the modem's TCP stack -- though they're nominally independent services, we've already seen the SIM7600 behave unexpectedly with session reuse).
3. Commands delivered via both paths would execute twice (ARM -> ARM is harmless, but PHOTO -> two photo captures wastes time and data).

### Rollback plan

- We have commit `0b810ff` (known-good HTTP polling) as the fallback.
- If MQTT fails during field testing: `git checkout 0b810ff -- firmware/AntiTheftSystemLilygo.ino`, reflash, confirmed working in <5 minutes.
- The Worker keeps the `/commands/poll` and `/commands/dispatch` (KV path) endpoints intact -- they're just not called. If we need to rollback the LILYGO, the Worker still serves the old poll path without any redeployment.

### Staged rollout

1. **Phase 1 (bench test):** Flash MQTT firmware, test with serial monitor. Verify `+CMQTTRECV` -> `handleSMSCommand` -> UART to Main. Test reconnect by power-cycling the modem.
2. **Phase 2 (Worker deploy):** Add MQTT publish to `handleCommandsDispatch` alongside the existing KV write. Both paths fire -- the LILYGO ignores the KV (it's not polling), the dashboard gets the KV-backed optimistic update.
3. **Phase 3 (field test):** Install in vehicle, send ARM/DISARM from dashboard, verify via serial + `wrangler tail`.
4. **Phase 4 (cleanup):** Remove `pollWorkerCommands()`, `enqueueCommand()`, `CMD_QUEUE_KEY` KV writes, and the `[LAT]` instrumentation from the LILYGO. Remove `/commands/poll` from the Worker (or keep as a dead endpoint).

---

## 7. Effort + Risk

### Effort estimate

| Component | Lines added | Lines removed | Functions touched |
|-----------|------------|---------------|-------------------|
| MQTT init in `setup()` | ~20 | 0 | `setup()` |
| `drainSerialAT()` + MQTT URC parser | ~50 | 0 | New function |
| AT read helper refactor (5 functions) | ~15 | ~10 | `sendAT`, `waitHttpAction`, `waitDownloadPrompt`, `waitOK`, `httpReadBody` |
| MQTT reconnect logic | ~20 | 0 | New function, `loop()` |
| `loop()` -- replace poll with MQTT check | ~10 | ~15 | `loop()` |
| Remove poll function + [LAT] instrumentation | 0 | ~100 | Delete `pollWorkerCommands`, `extractJsonNumber`, `[LAT]` lines |
| Worker -- add MQTT publish | ~15 | ~5 | `handleCommandsDispatch` |
| `secrets.h` -- MQTT credentials | ~3 | 0 | -- |
| **Total** | **~133** | **~130** | **Net ~0 lines** (architecture swap, not growth) |

### Top 3 risks specific to THIS SIM7600G

1. **MQTT + HTTP coexistence on the AT serial.** The SIM7600 runs MQTT and HTTP as separate internal services, but they share the UART for URCs. If `+CMQTTRECV` arrives in the middle of a binary photo upload (`httpPostBinary` writes raw bytes via `SerialAT.write` at line 355), the modem may interleave the URC into the HTTPDATA stream, corrupting the upload. This is the #1 risk -- it's the same class of bug that caused our HTTP 400 saga, just in the other direction. **Mitigation:** Test with deliberate concurrent command + photo scenarios. If it fails, gate MQTT unsubscribe/resubscribe around photo uploads.

2. **`AT+CMQTTSTART` / `AT+CMQTTCONNECT` interaction with `AT+HTTPINIT`.** SIMCom documentation says the services are independent, but our modem has already surprised us. If starting MQTT disrupts the HTTP service (or vice versa), alerts and photos break. **Mitigation:** Bench-test the full sequence -- MQTT connect -> HTTP POST -> MQTT receive -> HTTP POST -- before field deployment.

3. **Carrier NAT / keepalive tuning.** Hologram uses T-Mobile/AT&T infrastructure. Some T-Mobile towers have aggressive 30s NAT timeouts for idle TCP. If our 60s keepalive is too slow, the connection drops every 30s and we spend more time reconnecting than connected. **Mitigation:** Start with 25s keepalive (conservative), verify connection stability over 1 hour, then relax to 60s if stable. Keepalive data cost at 25s is still only 0.3 KB/hour -- irrelevant.
