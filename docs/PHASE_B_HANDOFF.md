# Phase B Handoff — 2026-06-24

## Phase A (committed, pushed, hardware-verified)

Commit `11e3b5d` — three firmware fixes, all hardware-verified 2026-06-24:

- **C1:** LILYGO `IMG_BUF_SIZE` 51200 -> 65536 (match Main's 64KB buffer)
- **I2:** Boot armed-state sync — Main sends STATUS:ARMED/DISARMED + IMMOBILIZED to LILYGO after handshake
- **I1:** Deferred STATUS via `statusPending` flag — queued when alarm blocks UART, flushed after alarm completes

## Phase B (uncommitted, in working tree, pending preview verification)

Four files modified, all unstaged:

### webapp/api/analyze.js — I5 (auth + SSRF)

- CMD_SECRET auth gate: `req.headers['x-cmd-secret']` compared raw against `process.env.CMD_SECRET`. Same hash convention as the Worker (see Vercel Env Setup below).
- SSRF tightened: hostname must end with `ntfy.sh` AND pathname must start with `/file/` (attachment download URLs only). Rejects all other ntfy paths.

### worker/worker.js — I6, I7, B2 auth, B3 timestamps

- **I7:** Dead `m.message.startsWith("Command Reply")` clause removed from reply filter. "Command Reply" was the ntfy Title header, never the body — `SMS_REPLY:` check already catches all replies.
- **I6:** Safety-lockout forwarding. New `lockouts` category matches `m.message.startsWith("Immobilize refused")`. Routes through **existing** `sendWhatsApp()` and `sendWebPushToAll()` — no new sending mechanism. Deduped by `msg:${msg.id}` like all other categories.
- **B2:** `analyzePhoto()` now receives `cmdSecret` parameter, passes it as `X-CMD-Secret` header to the Vercel endpoint.
- **B3:** `parseSystemStatus()` returns `{ statusTs, ignitionTs }` instead of single `ts`. Legacy migration: if `obj.ts` exists without `obj.statusTs`, copies `ts` to both dimensions and deletes `ts`. Status extraction writes per-dimension timestamps.

### webapp/public/index.html — B3 pill timestamps, analyze auth

- **B3:** `pollWorkerStatus()` reads `data.statusTs` / `data.ignitionTs` with `|| data.ts || 0` fallback. `applyObservation` R2 uses per-dimension `sTs` / `iTs` local vars (with `obs.ts` fallback for R6 history seeds).
- **Analyze auth:** `analyzePhoto()` sends `X-CMD-Secret: CMD_SECRET` header. `CMD_SECRET` is the existing password-derived global (index.html:303/357). Early return `if (!CMD_SECRET) return;` skips analysis silently if no login session.

### docs/ARCHITECTURE.md — Phase C doc updates

- Image buffer: 50KB -> 64KB (prose and Key Parameters table)
- Concurrency: added `statusPending` deferred-STATUS description
- CAM protocol: 3-photo burst -> 2 warmup + 1 keeper frame
- GPS: added gated-on-armed description
- analyze.js auth: documented `X-CMD-Secret` requirement

## Vercel Env Setup (required before analyze works)

Both must be set before preview or prod testing:

1. **CMD_SECRET** (scope: Production AND Preview)
   - Value: first 32 hex chars of `SHA256("cmd:" + <login password>)`
   - Same value as the Worker's wrangler secret `CMD_SECRET`
   - Derive with: `echo -n "cmd:<password>" | sha256sum | cut -c1-32`
   - Set with: `vercel env add CMD_SECRET Production` then `vercel env add CMD_SECRET Preview`

2. **ANTHROPIC_API_KEY** (scope: add Preview — currently Production-only)
   - Set with: `vercel env add ANTHROPIC_API_KEY Preview`

## Regression Risk

**B3 (pill per-dimension timestamps)** is the regression-risk item — it touches the pill state machine R2 logic. If pills misbehave on preview (flicker, stale values, flipped dimensions), back out B3 only and ship B1 (I6/I7) + B2 (I5) without it.

## Preview Checklist

Before committing or deploying to prod, verify on preview:

- [ ] (a) Logged-in analyze of a real alert photo works (badge appears with threat level)
- [ ] (b) Logged-out or wrong-secret analyze returns 401 (no crash, "AI unavailable" badge or silent skip)
- [ ] (c) Immobilize-while-moving safety lockout reaches WhatsApp (trigger on hardware or check Worker logs)
- [ ] (d) Arm -> disarm -> immobilize -> restore pill transitions behave correctly (no flicker/stale/flipped)
- [ ] (e) Non-`/file/` ntfy URL rejected by analyze endpoint (curl test: POST with ntfy.sh topic URL, expect 400)

## Deploy Order

1. Deploy Worker to staging: `cd worker && npx wrangler deploy` (or `--env staging` if configured)
2. Deploy webapp to Vercel PREVIEW (never `--prod`): `cd webapp && npx vercel` (no `--prod` flag)
3. Run preview checklist above
4. Commit the four Phase B/C files only after preview passes
5. User runs all prod deploys manually
