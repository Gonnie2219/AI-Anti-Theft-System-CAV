/**
 * Cloudflare Worker: Anti-Theft Alert Bridge + Command Queue
 *
 * Alert delivery is website-only (dashboard SSE + ntfy + Web Push).
 * Retired Twilio WhatsApp/SMS delivery paths were removed 2026-07.
 *   FAST: LILYGO POSTs directly to /ingest → immediate Web Push fan-out
 *   SAFE: Cron polls ntfy every 60s → safety-net Web Push
 * Both paths share a body-content-hash dedup key in KV.
 *
 * Command path (KV fallback lane; primary is the browser's direct MQTT publish):
 *   Dashboard → browser POSTs to /commands/dispatch → KV cmd_queue → /commands/poll
 *   /commands/poll reads KV with a numeric timestamp cursor, authenticated by DEVICE_POLL_TOKEN
 *
 * Status path (vehicle → dashboard):
 *   LILYGO posts "System X" to ntfy → cron extracts → KV system_status → GET /status
 *
 * Required secrets (set via `wrangler secret put`):
 *   NTFY_TOPIC
 *   CMD_SECRET — shared secret for authenticating dashboard and LILYGO /ingest
 *   DEVICE_POLL_TOKEN — bearer token for LILYGO /commands/poll authentication
 *   VAPID_PRIVATE_KEY — P-256 private scalar (base64url) for Web Push VAPID
 *
 * Web Push path (user-facing channel):
 *   Dashboard PWA → POST /push/subscribe → KV push_sub:<sha256(endpoint)>
 *   Alerts (ingest + cron) → sendWebPushToAll → push services (RFC 8291/8292)
 *
 * KV namespace binding: ANTITHEFT_STATE
 * The Worker keeps its historical name "antitheft-whatsapp-bridge" — the URL
 * is hardcoded in the LILYGO firmware; do not rename.
 */

const VALID_COMMANDS = ["ARM", "DISARM", "STATUS", "PHOTO", "GPS", "HELP", "IMMOBILIZE", "RESTORE"];

// ── Web Push (RFC 8291 aes128gcm + RFC 8292 VAPID) ───────────
// Private key (P-256 scalar, base64url) lives in the VAPID_PRIVATE_KEY secret.
const VAPID_PUBLIC_KEY = "BAEaWU5rTz4gL4Ksprzmr3VLWWEFR4cr9ZfZkmbhuvQwT2WpM8PGjXeKCqIXH2chOpOe1WZgFTOuBCu6ZYKJ-sA";
const VAPID_SUBJECT = "mailto:gonnie2219@gmail.com";
const DASHBOARD_URL = "https://webapp-seven-livid-86.vercel.app/";

// Verify Bearer token or X-CMD-Secret header against the CMD_SECRET env var.
// Used to authenticate dashboard /commands/dispatch and LILYGO /ingest.
// Rejects everything when CMD_SECRET is unset or implausibly short — an
// empty or misconfigured secret must never match an empty header.
function checkCmdSecret(request, env) {
  if (!env.CMD_SECRET || env.CMD_SECRET.length < 16) return false;
  const authHeader = request.headers.get("Authorization") || "";
  if (authHeader === `Bearer ${env.CMD_SECRET}`) return true;
  const secretHeader = request.headers.get("X-CMD-Secret") || "";
  if (secretHeader === env.CMD_SECRET) return true;
  return false;
}

export default {
  async scheduled(event, env, ctx) {
    ctx.waitUntil(handleCron(env));
  },

  async fetch(request, env) {
    const url = new URL(request.url);

    // Fast-path alert ingest from LILYGO (bypasses ntfy→cron 60s delay)
    if (request.method === "POST" && url.pathname === "/ingest") {
      return handleIngest(request, env);
    }

    // Command queue: LILYGO polls for pending commands
    if (request.method === "GET" && url.pathname === "/commands/poll") {
      return handleCommandsPoll(request, env);
    }

    // Command queue: dashboard dispatches commands
    if (request.method === "POST" && url.pathname === "/commands/dispatch") {
      return handleCommandsDispatch(request, env);
    }

    // Dashboard polls for current system status
    if (request.method === "GET" && url.pathname === "/status") {
      return handleStatusPoll(env);
    }

    // Web Push: subscription management + test send (CMD_SECRET auth)
    if (request.method === "POST" && url.pathname === "/push/subscribe") {
      return handlePushSubscribe(request, env);
    }
    if (request.method === "POST" && url.pathname === "/push/unsubscribe") {
      return handlePushUnsubscribe(request, env);
    }
    if (request.method === "POST" && url.pathname === "/push/test") {
      return handlePushTest(request, env);
    }

    // CORS preflight for dashboard cross-origin requests
    if (request.method === "OPTIONS" && (url.pathname.startsWith("/commands/") || url.pathname.startsWith("/push/") || url.pathname === "/status")) {
      return new Response(null, {
        headers: {
          "Access-Control-Allow-Origin": "*",
          "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
          "Access-Control-Allow-Headers": "Content-Type, Authorization",
        },
      });
    }

    // Health check
    const lastPoll = await env.ANTITHEFT_STATE.get("last_poll_timestamp");
    return Response.json({
      status: "ok",
      worker: "antitheft-whatsapp-sms-bridge",
      last_poll_timestamp: lastPoll || "never",
    });
  },
};

// Parse dedup value: handles legacy "1" and JSON formats.
// Only the `push` field is read since WhatsApp/SMS delivery was retired.
function parseDedup(raw) {
  if (!raw) return { push: false };
  if (raw === "1") return { push: true }; // legacy: fully delivered
  try {
    return JSON.parse(raw);
  } catch {
    return { push: false };
  }
}

// ── Web Push engine ──────────────────────────────────────────
function b64urlDecode(s) {
  s = s.replace(/-/g, "+").replace(/_/g, "/");
  while (s.length % 4) s += "=";
  return Uint8Array.from(atob(s), (c) => c.charCodeAt(0));
}

function b64urlEncode(buf) {
  return btoa(String.fromCharCode(...new Uint8Array(buf)))
    .replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

async function sha256Hex(s) {
  const hash = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(s));
  return [...new Uint8Array(hash)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

// VAPID JWT (ES256) — RFC 8292
async function vapidAuthHeader(env, endpoint) {
  const pub = b64urlDecode(VAPID_PUBLIC_KEY); // 65-byte uncompressed point
  const jwk = {
    kty: "EC", crv: "P-256",
    x: b64urlEncode(pub.slice(1, 33)),
    y: b64urlEncode(pub.slice(33, 65)),
    d: env.VAPID_PRIVATE_KEY,
  };
  const key = await crypto.subtle.importKey(
    "jwk", jwk, { name: "ECDSA", namedCurve: "P-256" }, false, ["sign"]
  );
  const enc = new TextEncoder();
  const header = b64urlEncode(enc.encode(JSON.stringify({ typ: "JWT", alg: "ES256" })));
  const claims = b64urlEncode(enc.encode(JSON.stringify({
    aud: new URL(endpoint).origin,
    exp: Math.floor(Date.now() / 1000) + 12 * 3600,
    sub: VAPID_SUBJECT,
  })));
  const unsigned = `${header}.${claims}`;
  // WebCrypto ECDSA emits raw r||s — exactly what JWS ES256 requires
  const sig = await crypto.subtle.sign(
    { name: "ECDSA", hash: "SHA-256" }, key, enc.encode(unsigned)
  );
  return `vapid t=${unsigned}.${b64urlEncode(sig)}, k=${VAPID_PUBLIC_KEY}`;
}

async function hkdf(salt, ikm, info, length) {
  const key = await crypto.subtle.importKey("raw", ikm, "HKDF", false, ["deriveBits"]);
  return new Uint8Array(
    await crypto.subtle.deriveBits({ name: "HKDF", hash: "SHA-256", salt, info }, key, length * 8)
  );
}

// RFC 8291 aes128gcm payload encryption
async function encryptPushPayload(sub, payloadStr) {
  const uaPub = b64urlDecode(sub.keys.p256dh);    // client public key (65B)
  const authSecret = b64urlDecode(sub.keys.auth); // client auth secret (16B)

  const asKeys = await crypto.subtle.generateKey(
    { name: "ECDH", namedCurve: "P-256" }, true, ["deriveBits"]
  );
  const asPub = new Uint8Array(await crypto.subtle.exportKey("raw", asKeys.publicKey));
  const uaKey = await crypto.subtle.importKey(
    "raw", uaPub, { name: "ECDH", namedCurve: "P-256" }, false, []
  );
  const ecdhSecret = new Uint8Array(
    await crypto.subtle.deriveBits({ name: "ECDH", public: uaKey }, asKeys.privateKey, 256)
  );

  const enc = new TextEncoder();
  const keyInfo = new Uint8Array([...enc.encode("WebPush: info\0"), ...uaPub, ...asPub]);
  const ikm = await hkdf(authSecret, ecdhSecret, keyInfo, 32);
  const salt = crypto.getRandomValues(new Uint8Array(16));
  const cek = await hkdf(salt, ikm, enc.encode("Content-Encoding: aes128gcm\0"), 16);
  const nonce = await hkdf(salt, ikm, enc.encode("Content-Encoding: nonce\0"), 12);

  // Single record: payload || 0x02 (last-record delimiter)
  const plaintext = new Uint8Array([...enc.encode(payloadStr), 2]);
  const aesKey = await crypto.subtle.importKey("raw", cek, "AES-GCM", false, ["encrypt"]);
  const ciphertext = new Uint8Array(
    await crypto.subtle.encrypt({ name: "AES-GCM", iv: nonce }, aesKey, plaintext)
  );

  // Body: salt(16) | record size(4) | keyid len(1) | as_public(65) | ciphertext
  const body = new Uint8Array(86 + ciphertext.length);
  body.set(salt, 0);
  new DataView(body.buffer).setUint32(16, 4096);
  body[20] = 65;
  body.set(asPub, 21);
  body.set(ciphertext, 86);
  return body;
}

// Send one push; returns HTTP status from the push service
async function sendWebPush(env, sub, payloadStr) {
  const body = await encryptPushPayload(sub, payloadStr);
  const resp = await fetch(sub.endpoint, {
    method: "POST",
    headers: {
      Authorization: await vapidAuthHeader(env, sub.endpoint),
      "Content-Encoding": "aes128gcm",
      "Content-Type": "application/octet-stream",
      TTL: "3600",
      Urgency: "high",
    },
    body,
  });
  return resp.status;
}

// Fan a notification out to every stored subscription; prune dead ones
async function sendWebPushToAll(env, title, msgBody, url) {
  if (!env.VAPID_PRIVATE_KEY) return 0;
  const payload = JSON.stringify({ title, body: msgBody, url, tag: "antitheft-alert" });
  const list = await env.ANTITHEFT_STATE.list({ prefix: "push_sub:" });
  let sent = 0;
  for (const key of list.keys) {
    try {
      const raw = await env.ANTITHEFT_STATE.get(key.name);
      if (!raw) continue;
      const status = await sendWebPush(env, JSON.parse(raw), payload);
      if (status === 404 || status === 410) {
        await env.ANTITHEFT_STATE.delete(key.name);
        console.log(`[push] pruned dead subscription ${key.name}`);
      } else if (status >= 400) {
        console.error(`[push] send failed (${status}) for ${key.name}`);
      } else {
        sent++;
      }
    } catch (err) {
      console.error(`[push] error for ${key.name}: ${err.message}`);
    }
  }
  return sent;
}

// Extract reason / location / maps URL from an ALERT body for push payloads
function parseAlertParts(body) {
  let reason = "Unknown", location = "", mapsUrl = "";
  for (const line of body.split("\n")) {
    const a = line.match(/^ALERT:\s*(.+)/);
    if (a) reason = a[1].trim();
    const l = line.match(/^Location:\s*([-\d.]+,[-\d.]+)/);
    if (l) location = l[1];
    const u = line.match(/(https:\/\/maps\.google\.com\/[^\s]+)/);
    if (u) mapsUrl = u[1];
  }
  return { reason, location, mapsUrl };
}

async function pushAlert(env, alertBody) {
  const { reason, location, mapsUrl } = parseAlertParts(alertBody);
  const body = location ? `${reason} @ ${location}` : reason;
  return sendWebPushToAll(env, "🚨 Anti-Theft ALERT", body, mapsUrl || DASHBOARD_URL);
}

// ── Web Push endpoint handlers ───────────────────────────────
async function handlePushSubscribe(request, env) {
  if (!checkCmdSecret(request, env)) {
    return new Response("Unauthorized", { status: 401 });
  }
  let sub;
  try { sub = await request.json(); } catch { return corsJson({ error: "Invalid JSON" }, 400); }
  if (!sub || typeof sub.endpoint !== "string" || !sub.endpoint.startsWith("https://") ||
      !sub.keys || typeof sub.keys.p256dh !== "string" || typeof sub.keys.auth !== "string") {
    return corsJson({ error: "Invalid subscription" }, 400);
  }
  const key = `push_sub:${await sha256Hex(sub.endpoint)}`;
  await env.ANTITHEFT_STATE.put(key, JSON.stringify({
    endpoint: sub.endpoint,
    keys: { p256dh: sub.keys.p256dh, auth: sub.keys.auth },
  }));
  return corsJson({ status: "subscribed" });
}

async function handlePushUnsubscribe(request, env) {
  if (!checkCmdSecret(request, env)) {
    return new Response("Unauthorized", { status: 401 });
  }
  let body;
  try { body = await request.json(); } catch { return corsJson({ error: "Invalid JSON" }, 400); }
  if (!body || typeof body.endpoint !== "string") {
    return corsJson({ error: "Missing endpoint" }, 400);
  }
  await env.ANTITHEFT_STATE.delete(`push_sub:${await sha256Hex(body.endpoint)}`);
  return corsJson({ status: "unsubscribed" });
}

async function handlePushTest(request, env) {
  if (!checkCmdSecret(request, env)) {
    return new Response("Unauthorized", { status: 401 });
  }
  const sent = await sendWebPushToAll(
    env, "Anti-Theft Dashboard", "Test notification — Web Push is working.", DASHBOARD_URL
  );
  return corsJson({ status: "sent", delivered: sent });
}

// Content-hash dedup key shared by /ingest and cron paths
async function bodyDedupKey(body) {
  const data = new TextEncoder().encode(body.trim().slice(0, 200));
  const hash = await crypto.subtle.digest("SHA-256", data);
  const hex = [...new Uint8Array(hash)]
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
  return `body:${hex.slice(0, 16)}`;
}

// ── Fast-path ingest from LILYGO ─────────────────────────────
async function handleIngest(request, env) {
  if (!checkCmdSecret(request, env)) {
    return new Response("Unauthorized", { status: 401 });
  }

  const body = (await request.text()).trim();
  if (!body.startsWith("ALERT:")) {
    return Response.json({ status: "ignored", reason: "not an alert" });
  }

  const dedupKey = await bodyDedupKey(body);
  const existing = await env.ANTITHEFT_STATE.get(dedupKey);
  if (existing) {
    console.log(`[ingest] dedup hit: ${dedupKey}`);
    return Response.json({ status: "dedup" });
  }

  // Web Push fan-out — the sole fast-path delivery channel
  let pushSent = 0;
  try {
    pushSent = await pushAlert(env, body);
    console.log(`[ingest] web push sent to ${pushSent} subscription(s)`);
  } catch (err) {
    console.error(`[ingest] web push failed: ${err.message}`);
  }

  // Write dedup key so cron doesn't re-push this alert.
  await env.ANTITHEFT_STATE.put(
    dedupKey,
    JSON.stringify({ push: true, ingest: true }),
    { expirationTtl: 86400 }
  );

  return Response.json({ status: "ok", push: pushSent });
}

async function handleCron(env) {
  try { return await _handleCron(env); }
  catch (err) { console.error("Cron handler error:", err); }
}

async function _handleCron(env) {
  const messages = await pollNtfy(env);
  if (messages.length === 0) return;

  // Advance cursor past all fetched messages regardless of categorization
  const latestTime = Math.max(...messages.map((m) => m.time));
  await env.ANTITHEFT_STATE.put("last_poll_timestamp", String(latestTime));

  // Only forward messages from the last 5 minutes to avoid backlog floods
  const freshCutoff = Math.floor(Date.now() / 1000) - 300;
  const fresh = messages.filter((m) => m.time >= freshCutoff);
  if (fresh.length === 0) return;

  // Separate into alerts and safety lockouts. Photos and SMS_REPLY messages
  // are consumed by the dashboard directly over ntfy SSE — the cron only
  // needed them for the retired WhatsApp/SMS delivery.
  const alerts = fresh.filter(
    (m) => typeof m.message === "string" && m.message.startsWith("ALERT:")
  );

  const lockouts = fresh.filter(
    (m) =>
      typeof m.message === "string" && m.message.startsWith("Immobilize refused")
  );

  console.log(`Found ${alerts.length} alert(s), ${lockouts.length} lockout(s)`);

  // --- Process alerts (Web Push safety net; /ingest is the fast path) ---
  for (const alert of alerts) {
    const dedupKey = `msg:${alert.id}`;
    const dedup = parseDedup(await env.ANTITHEFT_STATE.get(dedupKey));
    if (dedup.push) {
      console.log(`Skipping delivered: ${alert.id}`);
      continue;
    }

    // Skip the push when /ingest already handled this alert body
    const bKey = await bodyDedupKey(alert.message || "");
    const ingestDedup = parseDedup(await env.ANTITHEFT_STATE.get(bKey));

    if (ingestDedup.push) {
      dedup.push = true;
    } else {
      try {
        const sent = await pushAlert(env, alert.message || "");
        console.log(`Web push for ${alert.id}: ${sent} subscription(s)`);
      } catch (err) {
        console.error(`Web push failed for ${alert.id}: ${err.message}`);
      }
      dedup.push = true; // best-effort: no retry loop on partial failures
    }

    await env.ANTITHEFT_STATE.put(dedupKey, JSON.stringify(dedup), {
      expirationTtl: 86400,
    });
    // Also write body dedup key so /ingest won't re-process
    await env.ANTITHEFT_STATE.put(bKey, JSON.stringify(dedup), {
      expirationTtl: 86400,
    });
    console.log(`Forwarded alert ${alert.id} (push:${dedup.push})`);
  }

  // --- Forward safety-lockout notifications (Web Push) ---
  for (const msg of lockouts) {
    const dedupKey = `msg:${msg.id}`;
    if (await env.ANTITHEFT_STATE.get(dedupKey)) continue;

    const body = msg.message || "";
    try {
      await sendWebPushToAll(env, "⚠️ Safety Lockout", body, DASHBOARD_URL);
    } catch (err) {
      console.error(`[lockout] push failed: ${err.message}`);
    }
    await env.ANTITHEFT_STATE.put(dedupKey, JSON.stringify({ push: true }), {
      expirationTtl: 86400,
    });
    console.log(`Forwarded safety lockout ${msg.id}`);
  }

  // --- Extract system status from "System X" messages ---
  // LILYGO posts "System ARMED", "System DISARMED", "System IMMOBILIZED",
  // "System IGNITION_OK" to ntfy when the Main ESP reports state changes.
  // Store the latest in KV so the dashboard can poll GET /status.
  for (const msg of fresh) {
    const body = (msg.message || "").trim();
    if (!body.startsWith("System ")) continue;
    const keyword = body.split("\n")[0].substring(7).trim().toUpperCase();
    const stored = parseSystemStatus(await env.ANTITHEFT_STATE.get("system_status"));
    if (keyword === "ARMED")            { stored.armed = true;  stored.statusTs = msg.time * 1000; }
    else if (keyword === "DISARMED")    { stored.armed = false; stored.statusTs = msg.time * 1000; }
    else if (keyword === "IMMOBILIZED") { stored.immobilized = true;  stored.ignitionTs = msg.time * 1000; }
    else if (keyword === "IGNITION_OK") { stored.immobilized = false; stored.ignitionTs = msg.time * 1000; }
    else continue;
    await env.ANTITHEFT_STATE.put("system_status", JSON.stringify(stored));
    console.log(`Status updated: ${keyword}`);
  }
}

async function pollNtfy(env) {
  const topic = env.NTFY_TOPIC;
  const stored = await env.ANTITHEFT_STATE.get("last_poll_timestamp");
  // Default to 5 minutes ago on first run
  const since = stored || String(Math.floor(Date.now() / 1000) - 300);

  const url = `https://ntfy.sh/${topic}/json?poll=1&since=${since}`;
  const resp = await fetch(url);

  if (!resp.ok) {
    console.error(`ntfy poll failed: ${resp.status} ${resp.statusText}`);
    return [];
  }

  const text = await resp.text();
  if (!text.trim()) return [];

  const messages = text
    .trim()
    .split("\n")
    .map((line) => {
      try {
        return JSON.parse(line);
      } catch {
        return null;
      }
    })
    .filter(Boolean);

  return messages.filter((msg) => msg.event === "message");
}

// ── Command Queue (KV fallback lane) ─────────────────────────
// Dashboard → /commands/dispatch → KV; LILYGO polls /commands/poll
// (authenticated by DEVICE_POLL_TOKEN). The browser's direct MQTT
// publish is the primary command path; this queue is the HTTP fallback.

const CMD_QUEUE_KEY = "cmd_queue";
const CMD_MAX_AGE_MS = 300000; // 5 minutes

async function enqueueCommand(env, command, source) {
  const raw = await env.ANTITHEFT_STATE.get(CMD_QUEUE_KEY);
  let queue = [];
  if (raw) { try { queue = JSON.parse(raw); } catch {} }
  const cutoff = Date.now() - CMD_MAX_AGE_MS;
  queue = queue.filter((e) => e.ts > cutoff);
  const entry = { id: String(Date.now()), command, ts: Date.now(), source };
  queue.push(entry);
  await env.ANTITHEFT_STATE.put(CMD_QUEUE_KEY, JSON.stringify(queue), { expirationTtl: 600 });
  return entry.id;
}

function corsJson(data, status = 200) {
  return Response.json(data, {
    status,
    headers: { "Access-Control-Allow-Origin": "*" },
  });
}

function parseSystemStatus(raw) {
  const def = { armed: false, immobilized: false, statusTs: 0, ignitionTs: 0 };
  if (!raw) return def;
  try {
    const obj = JSON.parse(raw);
    // Migrate legacy single-ts format
    if (obj.ts && !obj.statusTs) { obj.statusTs = obj.ts; obj.ignitionTs = obj.ts; delete obj.ts; }
    return { ...def, ...obj };
  } catch { return def; }
}

async function handleStatusPoll(env) {
  const raw = await env.ANTITHEFT_STATE.get("system_status");
  return corsJson(parseSystemStatus(raw));
}

async function handleCommandsPoll(request, env) {
  // Authenticate device
  const authHeader = request.headers.get("Authorization") || "";
  if (authHeader !== `Bearer ${env.DEVICE_POLL_TOKEN}`) {
    return new Response("Unauthorized", { status: 401 });
  }

  const url = new URL(request.url);
  const since = url.searchParams.get("since") || "0";

  // Parse cursor: handle legacy composite "ntfyId,kvTimestamp" and new numeric format
  let kvCursor = 0;
  if (since.includes(",")) {
    kvCursor = Number(since.split(",")[1]) || 0;
  } else {
    kvCursor = Number(since) || 0;
  }

  const commands = [];
  let kvLatest = kvCursor;

  // Read KV cmd_queue (sole command source — dashboard + SMS both write here)
  const readTs = Date.now();
  try {
    const raw = await env.ANTITHEFT_STATE.get(CMD_QUEUE_KEY);
    if (raw) {
      const queue = JSON.parse(raw);
      for (const entry of queue) {
        if (entry.ts > kvCursor) {
          commands.push({ id: entry.id, command: entry.command, ts: entry.ts, writeTs: entry.ts });
          if (entry.ts > kvLatest) kvLatest = entry.ts;
        }
      }
    }
  } catch {}

  // Advance past first-boot if no messages yet
  if (kvCursor === 0 && kvLatest === 0) {
    kvLatest = Date.now();
  }

  return corsJson({ commands, latestId: String(kvLatest), readTs });
}

async function handleCommandsDispatch(request, env) {
  if (!checkCmdSecret(request, env)) {
    return new Response("Unauthorized", { status: 401 });
  }

  const body = (await request.text()).trim().toUpperCase();

  if (!VALID_COMMANDS.includes(body)) {
    return corsJson(
      { error: `Invalid command. Valid: ${VALID_COMMANDS.join(" ")}` },
      400
    );
  }

  // Write to KV — HTTP fallback lane for dashboard commands.
  const id = await enqueueCommand(env, body, "dashboard");
  const writeTs = Date.now();

  return corsJson({ status: "queued", command: body, id, writeTs });
}

