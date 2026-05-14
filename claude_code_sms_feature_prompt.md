# Claude Code Prompt — Add Native SMS Channel to Anti-Theft System

Copy everything below this line into Claude Code in your `~/ArduinoFiles/UROPProject` working directory.

---

## Context: what this project is

This is my UROP anti-theft system (HP0787, Wayne State ECE, advisor Dr. Lubna Alazzawi). It's a three-board ESP32-based platform that detects unauthorized vehicle access and alerts me via multiple channels.

The three boards and their roles:
- **Main ESP32** (`AntiTheftSystemMainESP.ino`) — sensors, RF remote, arm/disarm state machine, status LEDs, FreeRTOS dual-core orchestration. Main loop on Core 1, alarm task on Core 0. Mutex-protected UART to the LILYGO.
- **ESP32-CAM** (`AntiTheftSystemESP32CAM.ino`) — photo capture on `PHOTO` command, JPEG over UART back to Main.
- **LILYGO T-SIM7600G-H** (`AntiTheftSystemLilygo.ino`) — cellular modem, GPS, all uplink/downlink to the cloud.

Current alert channels (Phase 4c, all working — **do not break any of these**):
1. **ntfy.sh push** — plain HTTP POST to raw IP `159.203.148.75`, topic `antitheft-gonnie-2219`
2. **ntfy.sh email forwarding** — to `gonnie2219@gmail.com`
3. **Web dashboard** at `webapp-seven-livid-86.vercel.app` — shows live status, battery, GPS, photo, and has Arm / Disarm / Request GPS / Request Photo buttons. Real-time via SSE. The LILYGO must already be polling (or being pushed) commands from the server somehow — read the code carefully to find the existing downlink mechanism.
4. **WhatsApp** — via Cloudflare Worker (`antitheft-whatsapp-bridge.gonnie2219.workers.dev`) that polls ntfy and forwards via Twilio Sandbox. The Worker also runs **AI image analysis** (e.g. "AI Analysis: HIGH: person clearly visible in frame") and appends that to the WhatsApp message. The AI runs **server-side**, not on the LILYGO.

Important: the firmware in this repo is the live Phase 4c version. Read each `.ino` file from disk before making any assumptions — do not trust any older snapshot.

## What just changed

I swapped the Hologram SIM for a Speedtalk SIM today (May 1, 2026). Hologram silently dropped all outbound SMS for months (every approach failed: AT+CMGS text mode, PDU mode, Hologram REST API). Speedtalk just successfully sent its first SMS — verified end-to-end with `+CMGS: 0` and an actual SMS landing on my phone.

New cellular config:
- **APN: `Wholesale`** (not `hologram` anymore — change this everywhere)
- Phone number on the Speedtalk SIM: **+1 313 208 1968** (this is the system's number)
- My phone (the owner): **+1 609 358 9220**
- IMEI on the LILYGO: 862636057063651

## What I want to add

### Goal 1 — SMS as a new outbound alert channel
On every alarm event, in addition to the existing four channels, send an SMS to the owner with:
- Alert reason (`Vibration` / `Door Opened`)
- Timestamp
- GPS Google Maps link
- A link to the captured photo on ntfy.sh (NOT the binary photo — see decisions below)
- AI interpretation text if available

### Goal 2 — SMS as an inbound control channel
Accept commands sent via SMS from the owner's number:
- `ARM` → arm the system, reply with confirmation
- `DISARM` → disarm
- `STATUS` → reply with armed state, battery %, GPS link, last alarm time
- `PHOTO` → trigger a photo capture and reply with the ntfy URL once uploaded
- `GPS` → reply with current GPS link
- `HELP` → reply with the list of valid commands

Reject any SMS from a number that isn't in the authorized list (whitelist `OWNER_PHONE` for now; design so adding more numbers later is one config change).

## Design decisions already made — implement these as specified

1. **APN change.** Replace `hologram` with `Wholesale` in the LILYGO firmware. Verify that `AT+CGDCONT=1,"IP","Wholesale"` is what's actually sent.
2. **Photos in SMS = link only, not binary.** SIM7600 technically supports MMS but Speedtalk's $5 GPS Tracker plan provisioning is uncertain, and MMS over AT commands is fragile. Send the ntfy.sh photo URL as plain text in the SMS body — the recipient taps it and views the photo on the ntfy web page. Same UX as our existing email forwarding. The URL pattern is `https://ntfy.sh/antitheft-gonnie-2219` (the most recent attachment shows up there).
3. **AI interpretation in SMS.** The AI runs in the Cloudflare Worker, not on the LILYGO. Two paths to choose between, ask me before picking:
   - **(A) LILYGO sends two SMS:** first SMS immediately on alarm with reason + GPS link + photo link, second SMS sent ~10 sec later after polling ntfy or the server for the AI result.
   - **(B) Cloudflare Worker sends the SMS:** instead of the LILYGO. The Worker already does this for WhatsApp, has the photo, has the AI text, and has Twilio. Speedtalk's role becomes inbound-only + a fallback outbound when the Worker can't reach the LILYGO.
   - I lean toward **(B)** for outbound (single source of truth, AI included natively, no dual-fire risk) plus **direct LILYGO SMS as a fallback** if the heartbeat or ntfy POST fails. But it's your call to advocate for one — don't just default to my lean.
4. **160-char limit.** GSM 7-bit SMS is 160 chars per segment. The full message (reason + timestamp + Maps URL + photo URL + AI line) will exceed that. Use concatenated SMS (UDH) if SIM7600 supports it cleanly via `AT+CMGF=1` with long body — otherwise split into 2 SMS with `(1/2)` `(2/2)` prefixes. Document which path you chose.
5. **Inbound SMS via URC.** Use `AT+CNMI=2,1,0,0,0` to get unsolicited `+CMTI: "SM",N` notifications when an SMS arrives, then `AT+CMGR=N` to read, `AT+CMGD=N` to delete after processing. Poll the modem URC stream from the LILYGO main loop (non-blocking, integrates with the existing ntfy/HTTP polling cadence). Do NOT busy-wait — must coexist with the existing alarm pipeline and TCP operations.
6. **Authorized number whitelist.** `#define OWNER_PHONE "+16093589220"` at the top of the LILYGO file. Strip whitespace, normalize the `+` prefix, and string-compare. Reject silently (no reply) if it doesn't match — don't tell spammers they hit a real device.
7. **Echo off.** The system already runs `ATE0` in setup. Keep it. The reason this matters: my one-shot SMS test sketch didn't run ATE0 and every AT command got echoed into the SMS body. Don't repeat that mistake in the integration.
8. **Mutex discipline.** The Main ESP32 already uses a `SemaphoreHandle_t lilygoMutex` for the LILYGO UART. The LILYGO itself probably needs analogous protection on `SerialAT` because the inbound SMS poller, the outbound TCP code, and the GPS polling loop all share the modem UART. If the existing code doesn't already have one for the modem UART, add it.

## Open questions — ask me before deciding

1. AI integration path: **(A)** vs **(B)** above. Which architecture should we go with?
2. Should `ARM` / `DISARM` over SMS only work when the system is in a particular state, or should they always be accepted? (e.g. should a duplicate `ARM` while already armed reply "Already armed" or just silently re-confirm?)
3. Should an SMS-issued `PHOTO` command run the full alarm pipeline (photo → forward to LILYGO → upload to ntfy → reply with link), or a lighter "snapshot" path (photo to ntfy only, no alarm escalation)?
4. SMS rate limiting — should the system cap outbound SMS at e.g. 10/hour to avoid runaway scenarios if a sensor goes haywire? Speedtalk plan has no overage protection enabled (line just suspends), so a runaway is a real risk.

Don't pick a path on any of these yourself. Stop and ask.

## Suggested implementation order

Each step is its own git commit so we can roll back cleanly. Use real commit messages, not "wip".

1. **APN swap + smoke test.** Just change `hologram` to `Wholesale` in `AntiTheftSystemLilygo.ino`. Flash, verify modem still registers, ntfy still works, web dashboard still gets heartbeats. Commit: `firmware(lilygo): switch APN from Hologram to Speedtalk Wholesale`.
2. **Outbound SMS function.** Add a `sendSMS(String to, String body)` helper to the LILYGO firmware. Drains echo, sets `AT+CMGF=1` and `AT+CSCS="GSM"` once at boot, handles the `>` prompt, sends Ctrl+Z (`SerialAT.write(26)`), waits for `+CMGS:` or `+CMS ERROR:`. Returns bool. Don't call it from the alarm pipeline yet — just wire it up and verify it can be called from a debug command on USB serial.
3. **Outbound SMS in the alert pipeline.** Hook `sendSMS` into the alarm path. Implement the chosen architecture from open question 1. Test by triggering the reed switch with the system armed and confirming all five channels fire (ntfy push, ntfy email, dashboard, WhatsApp, SMS).
4. **Inbound SMS poller.** Add the `+CMTI:` URC handler to the LILYGO main loop. Implement `AT+CMGR` parsing — extract sender number, message body, strip whitespace. Whitelist check. Plumb the parsed command into a function that hands off to the Main ESP32 over the existing UART link (e.g. `SMS_CMD:ARM`, `SMS_CMD:DISARM`, etc).
5. **Main ESP32 SMS command handler.** On the Main ESP32, add handling for the new `SMS_CMD:*` UART messages from the LILYGO. Map them onto the existing arm/disarm/photo logic — do NOT duplicate that logic, reuse what's there. The RF remote and the web dashboard already trigger arm/disarm; SMS becomes a third trigger path through the same internal function.
6. **SMS reply path.** When the Main ESP32 acts on a command, send back an acknowledgement string to the LILYGO over UART (e.g. `SMS_REPLY:Armed at 12:34`). LILYGO calls `sendSMS(ownerPhone, replyBody)` to confirm to the user.
7. **Documentation.** Update the header comment block in each modified `.ino` file with the new protocol additions. Update `README.md` if there is one in the repo, otherwise create a short one summarizing the SMS feature.

Stop after each step and report what you did, what works, what didn't. Don't string the whole thing together silently.

## Things to read before writing any code

1. All three `.ino` files in full (mine in this repo are Phase 4c, possibly more recent than what you might assume).
2. Look for existing downlink command flow — how does the web dashboard "Arm" button reach the Main ESP32 today? There's already a path; the SMS inbound flow should mirror it for consistency.
3. Look for the existing AI analysis hook — is the Worker reading ntfy and posting back somewhere the LILYGO can see, or is everything one-way?
4. Confirm what `AT` initialization the LILYGO already does at boot. Don't duplicate.
5. Note any existing rate limiting, retry, or watchdog patterns and follow them.

## Test plan I'll run after each step

- **APN swap:** boot, watch serial, look for `+CREG: 0,1`, send a known-good ntfy POST, see it in the dashboard.
- **Outbound SMS:** send myself a test SMS from a debug serial command, see it land on my phone.
- **Outbound in pipeline:** trigger the reed switch (door magnet), see SMS plus all four existing channels fire.
- **Inbound:** text `STATUS` from my phone to +1 313 208 1968, see a reply with armed state and battery within ~30 sec.
- **Inbound `ARM` / `DISARM`:** verify LEDs change and dashboard reflects the new state.
- **Spam rejection:** send an SMS from a non-whitelisted number (I'll borrow my roommate's phone), confirm no reply and no state change.

## Things that are explicitly out of scope for this PR

- Changing the AI analysis prompt or the Cloudflare Worker logic itself. If we go with architecture (B) and the Worker needs to gain SMS capability, that's a separate followup.
- Adding new sensors.
- The relay/immobilization feature (relay module is in the mail, separate task).
- Migrating away from WhatsApp. WhatsApp stays. SMS is additive.

## Constants you'll need at the top of the LILYGO file

```c
#define OWNER_PHONE   "+16093589220"
#define SYSTEM_PHONE  "+13132081968"   // Speedtalk SIM (informational, not used for sending)
const char APN[] = "Wholesale";        // was "hologram"
```

## When you're done

Show me a diff summary per file, the full updated header comments, and the git log with the commit messages. I'll review before flashing.

If anything in this brief is ambiguous or contradicts what you find in the actual code, stop and ask. Don't paper over a real conflict.
