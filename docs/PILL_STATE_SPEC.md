# Pill State Machine — Implementation Contract

Target: webapp/public/index.html. Replace the multi-writer status/ignition pill logic (5 writers, 12 call sites, racing) with a single-writer state machine. Fixes correctness (pill never confidently wrong), not raw speed (confirmation stays SSE-bound, ~2s when SSE is healthy).

## Verified assumptions (read-only audit)
- SSE `msg.time` = epoch SECONDS, ntfy server clock.
- KV /status `ts` = msg.time*1000 = epoch MS, same ntfy clock (worker.js:578, :867-869). SSE msg.time*1000 and KV ts are directly comparable.
- KV system_status = { armed:bool, immobilized:bool, ts:number }; ts is ONE shared field bumped by ANY "System" message (status or ignition).
- 24h history (loadHistory) holds the latest "System X" message + msg.time for page-load seeding.

## State (only source of truth in the page)
state = {
  status:'ARMED'|'DISARMED'|'UNKNOWN',  statusTs:number,   // ntfy ms; 0 = no-confidence
  ignition:'IMMOBILIZED'|'OK'|'UNKNOWN', ignitionTs:number, // ntfy ms; 0 = no-confidence
  pendingStatus:   null | {cmd:'ARM'|'DISARM',        sentAt:number, tsAtSend:number},
  pendingIgnition: null | {cmd:'IMMOBILIZE'|'RESTORE', sentAt:number, tsAtSend:number},
  offline:boolean,
}
- sentAt = Date.now() (browser ms) — ONLY for the 60s timeout.
- tsAtSend = that dimension's ts at click time (ntfy ms) — for clock-safe pending resolution.

## Only two functions touch the pills
- render(): paints both pills purely from state. The ONLY pill-DOM writer (replaces W1-W5).
- applyObservation(obs): the ONLY state mutator; mutates per rules, then render(). All 12 former call sites route here; no call site touches DOM directly.

render() rules:
- status pill: pendingStatus set → pending visual ("ARMING..."/"DISARMING...", pulsing); else show state.status (UNKNOWN = neutral "Unknown", never confident green/red).
- ignition pill: pendingIgnition set → "IMMOBILIZING..."/"RESTORING..."; else show state.ignition (UNKNOWN neutral).
- offline=true → OFFLINE banner/overlay; does not change pill values.

## Authority
Live SSE > KV poll > nothing. Live SSE is newest by nature (ntfy live stream = new messages only). KV is a 0-120s-stale backstop. localStorage is NEVER a confident pill state. UNKNOWN/OFFLINE carry ts=0 so they never block a real observation; the ts gate governs confident-over-confident only.

## applyObservation rules

R1 — sse_live (body startsWith "System ", historical=false)
  Parse: "System ARMED"→status/ARMED, "System DISARMED"→status/DISARMED, "System IMMOBILIZED"→ignition/IMMOBILIZED, "System ignition_ok"→ignition/OK.
  - state[dim]=value; state[dimTs]=msg.time*1000; state.offline=false
  - if state[pendingDim]!=null: clear it + confirm flash (live truth resolves the command)
  - render()

R2 — kv (pollWorkerStatus /status, every 5s; kvTs=data.ts) — per dimension D (status←data.armed, ignition←data.immobilized):
  - if mapped kvValue != state[D] AND kvTs > state[D_ts]:
        state[D]=kvValue; state[D_ts]=kvTs
        if state[pendingD]!=null AND kvTs > state[pendingD].tsAtSend: clear pending + confirm flash
  - else: no-op for D (value-unchanged or older → never touch ts, never resolve pending)
  - never sets offline; render()

R3 — command (ARM/DISARM/IMMOBILIZE/RESTORE click)
  - state[pendingD] = {cmd, sentAt:Date.now(), tsAtSend:state[D_ts]}
  - write NO confident value (removes optimistic ignition writes)
  - render()  // pending visual makes ARM/etc feedback visible

R4 — timeout (interval; pending unresolved AND Date.now()-sentAt > 60000)
  - if state[D_ts] > state[pendingD].tsAtSend:  // newer device msg already arrived → value correct
        clear pendingD only (leave value)
    else:                                        // no device response observed
        clear pendingD; state[D]='UNKNOWN'; state[D_ts]=0
  - render()

R5 — offline (no live message 15min)
  - state.offline=true; do NOT touch status/ignition/ts; render()
  (cleared by R1 setting offline=false on next live msg — never reasserts old state)
  render() override: status pill shows "OFFLINE" (pill-offline), ignition pill shows neutral "Unknown" (pill-unknown) — without mutating state values. Offline banner reads pillState.status for "Was ARMED" text.

R6 — init (page load)
  - init: status=UNKNOWN/0, ignition=UNKNOWN/0, pendings=null, offline=false
  - in loadHistory(): find latest "System" status-type msg → apply as kv-tier (value + ts=msg.time*1000); same for latest ignition-type msg
  - localStorage NOT read for pill state; SSE onopen does NOT reassert

## Must remove (confident-wrong / racing paths)
- staleGuardTs and all uses (replaced by tsAtSend + R4).
- Optimistic ignition end-state writes on IMMOBILIZE/RESTORE click (old I4/I5, ~:1152-1153).
- Confident localStorage init of pills (old C1, ~:466) and any localStorage write feeding pill state.
- SSE onopen updateStatus(lastKnownStatus) reassert (old C2, ~:522).
- Offline-recovery reassert of lastKnownStatus (old C3, ~:611-612).
- Unguarded steady-state KV overwrite (old C8, ~:576-578) — superseded by R2.

## SSE timestamp change (load-bearing)
Live-message STATE observations MUST use msg.time*1000, NOT Date.now() (current :607). Feed-display timestamps may stay.

## Firmware-deferred (dashboard cannot fix)
V7: MainESP suppresses STATUS: send when alarmInProgress (:305,315) — key-fob toggle during an active alarm posts nothing, so neither SSE nor KV learns it. Bench item, out of scope here.

## Acceptance tests (hardware, after deploy)
1. ARM via remote → "ARMING..." → ARMED (visible transition). DISARM same.
2. Regression: arm, let it go slow / SSE drop → pill must NOT flip to a wrong confident state; correct state asserts when a device message lands.
3. IMMOBILIZE → "IMMOBILIZING..." → IMMOBILIZED; RESTORE → IGNITION OK. No flip-flop.
4. Refresh while armed/immobilized → seeds correct state from history, no stale-confident flash.
5. Redundant command (ARM while already armed) → no false instant confirm; resolves on real device msg or times out to Unknown (safe).
