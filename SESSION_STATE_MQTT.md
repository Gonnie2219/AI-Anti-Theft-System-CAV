# MQTT Migration Session State

## DONE THIS SESSION (committed 60d969e, pushed)

- **drainSerialAT() rewritten** for real SIM7600 multi-line +CMQTTRXSTART/RXTOPIC/RXPAYLOAD/RXEND framing (the +CMQTTRECV the old code parsed does NOT exist on firmware LE20B05SIM7600G22). Multi-packet concat, trailing CR/LF/space trim, bounded splice with payload-marker guard, atBuffer 8192 soft / 16384 hard cap.
- **connectMQTT()**: defensive teardown (DISC->STOP, no REL on cold start), TLS ctx 0 reuse, Form A subscribe (AT+CMQTTSUB=0,<strlen>,1 then '>' then topic bytes NO trailing CRLF), 8s connect timeout, pre-connect abort on alarmInProgress||SerialMain.available().
- **consumeMqttCommand()**: copy-clear-dispatch via handleSMSCommand(), max 3/pass.
- **loop wiring** gated on !alarmInProgress && !SerialMain.available(); non-blocking reconnect 10s backoff.
- HTTP command poll **RETAINED** in parallel as fallback (NOT yet retired).
- **Broker**: HiveMQ Serverless Free, host `b14f23266c4e43fa94f6a6dbe430a163.s1.eu.hivemq.cloud`, TLS 8883, topic `cmd/antitheft/commands`, cred `antitheft-device`.

## HARDWARE-VERIFIED

connect+subscribe OK; ARM dispatched ~5s via MQTT; photo upload stayed intact with a concurrent command (alarmInProgress gate holds).

## OPEN / NEXT SESSION (priority order)

1. **AI REGRESSION** (webapp, NOT firmware): dashboard shows "AI unavailable" on every photo. `/api/analyze` on Vercel (`webapp-ndb0vm6c4-gonnies-projects.vercel.app`). Was fixed earlier as commit 145de7d (model -> claude-sonnet-4-6, HTTP 200 verified) but is now failing again. Start with a read-only check of what the endpoint returns.
2. **SOAK MQTT** a day or two with poll still running; if reliable, **RETIRE the HTTP command poll** -> that is when the data-efficiency win fully lands.
3. **STEP 3**: Cloudflare Worker publishes commands to MQTT over WebSocket (port 8884; REST is HiveMQ Starter-only) so dashboard buttons drive MQTT instead of KV. Worker: `antitheft-whatsapp-bridge.gonnie2219.workers.dev`.

## KNOWN BUGS NOT TOUCHED

- Photo is 3rd burst frame + CAM_PARTIAL...RETRY_OK (seconds-late, wrong frame). Main ESP32/CAM firmware. Likely want FIRST frame.
- GPS "no fix" indoors is expected (needs sky view), not a bug.
- worker.js VALID_COMMANDS missing IMMOBILIZE/RESTORE (SMS path); GPS/HELP reply-format mismatch at worker.js:142.

## TODO HOUSEKEEPING

Rotate HiveMQ password (secrets.h + console) -- printed in clear text in dev screenshots.

## ROLLBACK

- Rollback for MQTT migration: `git checkout 0b810ff`
- Latest good: `60d969e`

## CONVENTIONS

- Canonical sketch: `C:\Users\gonni\ArduinoFiles\AntiTheftSystemLilygo\AntiTheftSystemLilygo.ino` (IDE flashes from here), mirrored to `...\UROPProject\firmware\`.
- Workflow: read-first, show-diff, verify-on-hardware-before-commit.
- Flash the PROBE by opening `MqttProbe\MqttProbe.ino` directly -- never paste probe code into the AntiTheftSystemLilygo folder.

## 2026-06-16 SESSION -- state-pill correctness + latency

### DONE & DEPLOYED

- **State desync fixed** (single-source-of-truth, Worker system_status, removed optimistic writes) -- commit 5e9dac8, worker deployed.
- **AI analyze regression fixed** (was stale Vercel deploy; redeployed claude-sonnet-4-6) -- 145de7d deployed via `npx vercel --prod`.
- **Pending-pill SSE confirm fix, ~45s->~14s** (was confirming off SMS_REPLY echo; now `startsWith` authoritative "System ARMED/DISARMED") -- commit bbf0017.
- **Always-on live-SSE state reconcile + Fixes 3/4/5** -- commit 4e4e825:
  - Fix 1: Authoritative "System X" messages reconcile pill unconditionally on live SSE (not gated on pendingCmd). Handles late arrival after timeout, unsolicited changes, SSE reconnect race.
  - Fix 2: `staleGuardTs` timestamp gates KV poll after timeout -- poll blocked until `data.ts > staleGuardTs`, preventing stale KV assertion. Chosen over nulling `lastKnownStatus` (which would lock out poll forever).
  - Fix 3: Timeout clears `lastKnownStatus`/`lastIgnitionState` + localStorage so SSE reconnect can't reassert stale.
  - Fix 4: Removed unconditional `indexOf` ignition matching (fired on SMS_REPLY echoes and history replay) -- replaced by prefix-matched live reconcile.
  - Fix 5: IMMOBILIZE/RESTORE timeout shows "IGNITION ?" pending pill instead of reverting to stale `previousIgnition`.

### STILL BROKEN (verified on hardware -- TOP PRIORITY NEXT)

1. **"No response -- retry" on successful ARM**: ARM commands that actually succeed (~45s arm, under 60s timeout) still hit the timeout and show "No response -- retry". The always-on reconcile and/or clearTimeout is NOT firing correctly on the late live "System ARMED" message. Fix 1 was supposed to prevent this; it doesn't fully. Needs diagnosis: why does the live authoritative SSE message not clear the pending/timeout state?

2. **IMMOBILIZE/RESTORE pill flips back and forth then lands WRONG**: on RESTORE, pill went correct (IGNITION OK) then re-asserted IMMOBILIZED. Something re-asserts stale ignition state after the correct one arrives -- suspect staleGuardTs/poll racing the ignition reconcile. Pill showed IMMOBILIZED/IGNITION OK contradicting actual device state.

3. **Fresh page load shows stale state**: pill briefly showed ARMED while device was DISARMED (red LED on). Load-time state from KV/localStorage may be stale or contradicting device.

### FIRMWARE (defer to bench -- biggest latency limiter)

- Device emits "System ARMED"/"System IGNITION_OK" ~30-45s after the command; ARM/RESTORE green/yellow LED itself takes ~30s. This is command-delivery + firmware HTTP-post-to-ntfy latency. No dashboard fix can make confirmation faster than the device emits. Investigate device poll cadence + ensureNetwork/HTTP post path on ARM and RESTORE specifically (DISARM/IMMOBILIZE are faster -- find why the asymmetry).

### DEFERRED (unchanged)

- **SMS cleanup**: 10DLC abandoned -- remove SMS surfaces (opt-in link, opt-in.html, consent.js, terms/privacy SMS refs), WhatsApp/ntfy untouched. Full audit map gathered this session (not yet acted on).
- **SMS_REPLY label**: firmware-generated (LILYGO `AntiTheftSystemLilygo.ino:1477`) -- relabel at bench, not a cloud fix.
- **Double-photo-burst + first-frame CAM bugs**: Main ESP32/CAM firmware.
- **MQTT migration**: soak period ongoing (HTTP poll retained in parallel); Step 3 (Worker->MQTT WebSocket) not started.
- **HiveMQ password rotation**: printed in clear text in dev screenshots.

## DASHBOARD / DISPLAY BUGS (found post-commit 60d969e, next session)

1. **STATE DESYNC** (priority): dashboard state pill shows DISARMED while the device is actually ARMED (event feed simultaneously shows "System ARMED" / "SMS_REPLY: Already armed"). The arm/disarm STATE displayed on the dashboard does not track the device's real state. Likely same root as the existing reply-gating/state-machine open bug. Owner-facing correctness risk (could think car is unprotected). Investigate webapp state handling + how STATUS:ARMED/DISARMED and command replies update the pill.

2. **AI UNAVAILABLE** (priority, webapp): every photo shows "AI unavailable" badge. `/api/analyze` on Vercel. Was fixed earlier (commit 145de7d, model claude-sonnet-4-6, HTTP 200) but regressed. Start read-only: check what `/api/analyze` returns now (auth/model/error). NOTE: today (Jun 16) the model `claude-sonnet-4-20250514` was already retired once this session -- confirm 145de7d's model string is still valid and the API key/env on Vercel is set.

3. **DOUBLE PHOTO BURST** (Main ESP32/CAM firmware): on trigger the CAM correctly does a 3-frame burst, but then fires ANOTHER 3-frame burst 5-10s later. Should be ONE burst per trigger. Investigate why the capture fires twice (possible: alarm re-trigger within debounce, or PHOTO command sent twice, or the CAM_PARTIAL...RETRY path re-requesting). Combine with the existing "wrong frame / want FIRST frame not 3rd" CAM bug.

4. **BATTERY INDICATOR** (verify, don't assume): dashboard shows a battery %/voltage that may be inaccurate. CONFIRM which battery it reads -- telemetry is via AT+CBC in heartbeats, which queries the MODEM power = almost certainly the LILYGO 18650, NOT the ESP32/CAM AA pack (earlier reading "52% 3.82V" is consistent with single 18650). Next session: read the actual AT+CBC parsing, verify the value is accurate and correctly labeled in the dashboard as the LILYGO/modem battery. The ESP32/CAM AA pack is NOT monitored at all -- note whether that should be added.

5. **SMS OPT-IN UI** (cleanup): dashboard still shows an "SMS Opt-In" link/feature, but SMS is not in use (USE_TWILIO_SMS=false, USE_NATIVE_SMS=0 -- comms are ntfy push + dashboard + WhatsApp). Decide: remove the SMS Opt-In UI, or keep it dormant for the pending 10DLC path. If 10DLC is still a planned feature, leave but clarify; if abandoned, remove from the dashboard.

## [Jun 17 2026] Pill state machine -- single-writer rewrite (IMPLEMENTED, NOT DEPLOYED)

STATE: webapp/public/index.html edited (191 ins / 161 del), docs/PILL_STATE_SPEC.md written. NOT committed, NOT deployed, NOT pushed. Working tree dirty. Live prod is still old 4e4e825 (buggy).

WHAT: Replaced 5-writer/12-call-site racing pill logic with single-writer state machine. Contract in docs/PILL_STATE_SPEC.md. Fixes 4 dashboard bugs at root (disarm-flips-back, no-response-on-ARM, immobilize/restore flip-flop, stale-on-load). Correctness only -- confirmation stays SSE-bound (~2s healthy), not faster.

ARCHITECTURE:
- pillState object = sole source of truth {status,statusTs,ignition,ignitionTs,pendingStatus,pendingIgnition,offline}.
- renderPills() = ONLY pill-DOM writer.
- applyObservation(obs) = ONLY state mutator; obs.type sse_live(R1)/kv(R2)/command(R3)/timeout(R4)/offline(R5). All former call sites route here.
- pendingCmd SPLIT: pillState.pendingStatus/pendingIgnition = pill pending (4 pill cmds); pendingAction = button UI + reply-matching (ALL cmds incl GPS/PHOTO/STATUS).
- confirmAction()/failAction() restore the BUTTON only, never touch pillState. Both guard on `if(!pendingAction)return` -> single restore guaranteed.
- Key timestamps: SSE+KV both ntfy ms (msg.time*1000 == data.ts). KV gate R2: only if value changed AND obs.ts>dimTs. tsAtSend (dim ts at click) gates pending resolution, clock-safe.

VERIFIED (read-only, pre-deploy): zero dangling refs (lastKnownStatus/staleGuardTs/showPendingPill/updateStatus/updateIgnition/lastIgnitionState/isOffline/pendingCmd all 0 matches). 7 CSS classes all exist in <style>. Double-fire trace walked: ARM resolves only via applyObservation R1/R2; reply-block scoped to GPS/PHOTO/STATUS; success+timeout both restore button exactly once.

REMOVED: staleGuardTs, optimistic ignition writes, confident localStorage pill init, SSE-onopen reassert, offline-recovery reassert, unguarded KV overwrite (old C8).

NOT DONE / NEXT:
- HARDWARE TEST PENDING (5 acceptance cases in PILL_STATE_SPEC.md): arm/disarm visible transition; SLOW-ARM REGRESSION (no wrong-confident flip -- the priority); immobilize/restore no flip-flop; refresh-while-armed seeds from history; redundant ARM no false instant confirm.
- Deploy path: cd webapp && npx vercel --prod ONLY (no wrangler -- worker untouched; no reflash -- firmware untouched).
- Push ONLY after hardware passes. If it fails: pillState rewrite is on disk uncommitted; fallback baseline is bbf0017 (last hardware-verified). Local is 2 ahead of origin/master (5e9dac8); 4e4e825+bbf0017 unpushed.
- FIRMWARE-DEFERRED (V7, bench): MainESP suppresses STATUS: send during alarmInProgress (:305,315) -- key-fob toggle mid-alarm posts nothing. Dashboard cannot fix.
