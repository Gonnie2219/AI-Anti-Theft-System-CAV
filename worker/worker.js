/**
 * Cloudflare Worker: Anti-Theft Alert Bridge + Command Queue
 *
 * Two delivery paths for alerts:
 *   FAST: LILYGO POSTs directly to /ingest → immediate WhatsApp delivery
 *   SAFE: Cron polls ntfy every 60s → safety-net WhatsApp + photo matching
 * Both paths share a body-content-hash dedup key in KV.
 *
 * Command path (KV-only):
 *   Dashboard → browser POSTs to /commands/dispatch → KV cmd_queue → /commands/poll
 *   SMS       → Twilio → /sms/inbound → KV cmd_queue → /commands/poll
 *   /commands/poll reads KV with a numeric timestamp cursor, authenticated by DEVICE_POLL_TOKEN
 *
 * Status path (vehicle → dashboard):
 *   LILYGO posts "System X" to ntfy → cron extracts → KV system_status → GET /status
 *
 * Required secrets (set via `wrangler secret put`):
 *   TWILIO_ACCOUNT_SID, TWILIO_AUTH_TOKEN,
 *   TWILIO_FROM, TWILIO_TO          — WhatsApp (whatsapp:+… prefix)
 *   TWILIO_SMS_FROM, TWILIO_SMS_TO  — SMS (plain +1… numbers)
 *   NTFY_TOPIC, ANALYZE_URL (Vercel /api/analyze endpoint)
 *   CMD_SECRET — shared secret for authenticating dashboard and LILYGO /ingest
 *   DEVICE_POLL_TOKEN — bearer token for LILYGO /commands/poll authentication
 *   VAPID_PRIVATE_KEY — P-256 private scalar (base64url) for Web Push VAPID
 *
 * Web Push path (user-facing channel, in ADDITION to WhatsApp):
 *   Dashboard PWA → POST /push/subscribe → KV push_sub:<sha256(endpoint)>
 *   Alerts (ingest + cron) → sendWebPushToAll → push services (RFC 8291/8292)
 *
 * KV namespace binding: ANTITHEFT_STATE
 */

const VALID_COMMANDS = ["ARM", "DISARM", "STATUS", "PHOTO", "GPS", "HELP", "IMMOBILIZE", "RESTORE"];
const OWNER_LAST10 = "6093589220";

// ── Web Push (RFC 8291 aes128gcm + RFC 8292 VAPID) ───────────
// Private key (P-256 scalar, base64url) lives in the VAPID_PRIVATE_KEY secret.
const VAPID_PUBLIC_KEY = "BAEaWU5rTz4gL4Ksprzmr3VLWWEFR4cr9ZfZkmbhuvQwT2WpM8PGjXeKCqIXH2chOpOe1WZgFTOuBCu6ZYKJ-sA";
const VAPID_SUBJECT = "mailto:gonnie2219@gmail.com";
const DASHBOARD_URL = "https://webapp-seven-livid-86.vercel.app/";

// Verify Bearer token or X-CMD-Secret header against the CMD_SECRET env var.
// Used to authenticate dashboard /commands/dispatch and LILYGO /ingest.
function checkCmdSecret(request, env) {
  const authHeader = request.headers.get("Authorization") || "";
  if (authHeader === `Bearer ${env.CMD_SECRET}`) return true;
  const secretHeader = request.headers.get("X-CMD-Secret") || "";
  if (secretHeader === env.CMD_SECRET) return true;
  return false;
}
// Set to true after Twilio 10DLC Sole Proprietor campaign is approved.
// Until then, every send attempt fails with Twilio error 30034 (carrier
// violation) AND may incur carrier filtering fees on the sending number.
const USE_TWILIO_SMS = false;

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

    // Inbound SMS webhook from Twilio
    if (request.method === "POST" && url.pathname === "/sms/inbound") {
      return handleInboundSMS(request, env);
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

// Parse dedup value: handles legacy "1" and new JSON format
function parseDedup(raw) {
  if (!raw) return { whatsapp: false, sms: false, push: false };
  if (raw === "1") return { whatsapp: true, sms: true, push: true }; // legacy: fully delivered
  try {
    return JSON.parse(raw);
  } catch {
    return { whatsapp: false, sms: false, push: false };
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

  // Format and send WhatsApp immediately (text only, no photo)
  const alert = { message: body };
  const message = formatWhatsAppMessage(alert, null);
  let waOk = false;
  if (await sendWhatsApp(env, message, null)) {
    waOk = true;
    console.log("[ingest] WhatsApp sent");
  } else {
    console.error("[ingest] WhatsApp failed");
  }

  // SMS — text-only alert (no photo available at ingest time)
  let smsOk = !USE_TWILIO_SMS; // true when disabled → suppresses cron retry
  if (USE_TWILIO_SMS) {
    const smsBody = formatSMSMessage(alert, null);
    smsOk = await sendSMS(env, smsBody);
    console.log(smsOk ? "[ingest] SMS sent" : "[ingest] SMS failed");
  }

  // Web Push fan-out (mirrors the WhatsApp fast path)
  let pushSent = 0;
  try {
    pushSent = await pushAlert(env, body);
    console.log(`[ingest] web push sent to ${pushSent} subscription(s)`);
  } catch (err) {
    console.error(`[ingest] web push failed: ${err.message}`);
  }

  // Write dedup key so cron doesn't re-send the text alert.
  // Cron can still send a follow-up photo if one arrives.
  await env.ANTITHEFT_STATE.put(
    dedupKey,
    JSON.stringify({ whatsapp: waOk, sms: smsOk, push: true, ingest: true }),
    { expirationTtl: 86400 }
  );

  return Response.json({ status: "ok", whatsapp: waOk, sms: smsOk, push: pushSent });
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

  // Separate into alerts, photos, and command replies
  const alerts = fresh.filter(
    (m) => typeof m.message === "string" && m.message.startsWith("ALERT:")
  );

  const photos = fresh.filter(
    (m) =>
      m.attachment &&
      m.attachment.url &&
      m.attachment.name &&
      /\.jpe?g$/i.test(m.attachment.name)
  );

  const replies = fresh.filter(
    (m) =>
      typeof m.message === "string" &&
      (m.message.startsWith("SMS_REPLY:") ||
        m.message.startsWith("Command Reply"))
  );

  console.log(`Found ${alerts.length} alert(s), ${photos.length} photo(s), ${replies.length} reply(s)`);

  // --- Process alerts (WhatsApp + SMS) ---
  for (const alert of alerts) {
    const dedupKey = `msg:${alert.id}`;
    const dedup = parseDedup(await env.ANTITHEFT_STATE.get(dedupKey));
    if (dedup.whatsapp && dedup.sms && dedup.push) {
      console.log(`Skipping fully delivered: ${alert.id}`);
      continue;
    }

    // Check if /ingest already sent the text-only WhatsApp for this alert
    const bKey = await bodyDedupKey(alert.message || "");
    const ingestDedup = parseDedup(await env.ANTITHEFT_STATE.get(bKey));
    const ingestHandled = ingestDedup.whatsapp;

    // Find matching photo within 30s after the alert
    const matchedPhoto = photos.find(
      (p) => p.time >= alert.time && p.time - alert.time <= 30
    );

    const mediaUrl =
      matchedPhoto && matchedPhoto.attachment && matchedPhoto.attachment.url
        ? matchedPhoto.attachment.url
        : null;

    // Run AI analysis on the photo if available
    let aiVerdict = null;
    if (mediaUrl && env.ANALYZE_URL) {
      aiVerdict = await analyzePhoto(env.ANALYZE_URL, mediaUrl);
    }

    // WhatsApp
    if (!dedup.whatsapp) {
      if (ingestHandled) {
        // /ingest already sent text-only WhatsApp; send photo follow-up if available
        dedup.whatsapp = true;
        if (mediaUrl) {
          const photoMsg = aiVerdict
            ? `📷 Photo + AI: ${aiVerdict}`
            : "📷 Alert photo";
          await sendWhatsApp(env, photoMsg, mediaUrl);
          console.log(`Photo follow-up for ingest-handled alert ${alert.id}`);
        }
      } else {
        // Normal cron path: full WhatsApp with text + photo + AI
        const message = formatWhatsAppMessage(alert, aiVerdict);
        if (await sendWhatsApp(env, message, mediaUrl)) {
          dedup.whatsapp = true;
        } else {
          console.error(`WhatsApp failed for ${alert.id}`);
        }
      }
    }

    // SMS
    if (!dedup.sms) {
      if (USE_TWILIO_SMS) {
        const smsBody = formatSMSMessage(alert, mediaUrl);
        if (await sendSMS(env, smsBody)) {
          dedup.sms = true;
        } else {
          console.error(`SMS failed for ${alert.id}`);
        }
      } else {
        dedup.sms = true; // mark delivered to avoid retry noise
      }
    }

    // Web Push (skipped when /ingest already pushed this alert)
    if (!dedup.push) {
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
    }

    await env.ANTITHEFT_STATE.put(dedupKey, JSON.stringify(dedup), {
      expirationTtl: 86400,
    });
    // Also write body dedup key so /ingest won't re-process
    await env.ANTITHEFT_STATE.put(bKey, JSON.stringify(dedup), {
      expirationTtl: 86400,
    });
    if (matchedPhoto) {
      await env.ANTITHEFT_STATE.put(
        `msg:${matchedPhoto.id}`,
        JSON.stringify({ whatsapp: true, sms: true }),
        { expirationTtl: 86400 }
      );
      console.log(`Forwarded alert ${alert.id} with photo ${matchedPhoto.id}`);
    } else {
      console.log(`Forwarded alert ${alert.id} (wa:${dedup.whatsapp} sms:${dedup.sms} ingest:${ingestHandled})`);
    }
  }

  // --- Process command replies (SMS only) ---
  for (const reply of replies) {
    const dedupKey = `msg:${reply.id}`;
    const dedup = parseDedup(await env.ANTITHEFT_STATE.get(dedupKey));
    if (dedup.sms) {
      console.log(`Skipping delivered reply: ${reply.id}`);
      continue;
    }

    let body = reply.message || "(no response)";
    if (body.startsWith("SMS_REPLY:")) body = body.substring(10).trim();
    if (USE_TWILIO_SMS) {
      if (await sendSMS(env, body)) {
        dedup.sms = true;
        dedup.whatsapp = true; // not applicable for replies
      } else {
        console.error(`SMS failed for reply ${reply.id}`);
      }
    } else {
      dedup.sms = true;
      dedup.whatsapp = true;
    }
    await env.ANTITHEFT_STATE.put(dedupKey, JSON.stringify(dedup), {
      expirationTtl: 86400,
    });
    console.log(`Reply ${reply.id}: sms=${dedup.sms}`);
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
    if (keyword === "ARMED")       stored.armed = true;
    else if (keyword === "DISARMED") stored.armed = false;
    else if (keyword === "IMMOBILIZED") stored.immobilized = true;
    else if (keyword === "IGNITION_OK") stored.immobilized = false;
    else continue;
    stored.ts = msg.time * 1000;
    await env.ANTITHEFT_STATE.put("system_status", JSON.stringify(stored));
    console.log(`Status updated: ${keyword}`);
  }

  // Mark orphan photos as consumed so they don't pile up
  for (const photo of photos) {
    const dedupKey = `msg:${photo.id}`;
    if (!(await env.ANTITHEFT_STATE.get(dedupKey))) {
      await env.ANTITHEFT_STATE.put(
        dedupKey,
        JSON.stringify({ whatsapp: true, sms: true }),
        { expirationTtl: 86400 }
      );
      console.log(`Consumed orphan photo ${photo.id}`);
    }
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

async function sendWhatsApp(env, message, mediaUrl) {
  const url = `https://api.twilio.com/2010-04-01/Accounts/${env.TWILIO_ACCOUNT_SID}/Messages.json`;
  const auth = btoa(`${env.TWILIO_ACCOUNT_SID}:${env.TWILIO_AUTH_TOKEN}`);

  const params = {
    To: env.TWILIO_TO,
    From: env.TWILIO_FROM,
    Body: message,
  };
  if (mediaUrl) {
    params.MediaUrl = mediaUrl;
    console.log(`Attaching photo: ${mediaUrl}`);
  }
  const body = new URLSearchParams(params);

  const resp = await fetch(url, {
    method: "POST",
    headers: {
      Authorization: `Basic ${auth}`,
      "Content-Type": "application/x-www-form-urlencoded",
    },
    body: body.toString(),
  });

  if (!resp.ok) {
    const err = await resp.text();
    console.error(`Twilio error ${resp.status}: ${err}`);
    return false;
  }

  return true;
}

async function analyzePhoto(analyzeUrl, imageUrl) {
  try {
    const resp = await fetch(analyzeUrl, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ imageUrl }),
    });
    if (!resp.ok) {
      console.error(`Analyze API error: ${resp.status}`);
      return null;
    }
    const data = await resp.json();
    return data.threatLevel && data.verdict
      ? `${data.threatLevel}: ${data.verdict}`
      : null;
  } catch (err) {
    console.error(`Analyze failed: ${err.message}`);
    return null;
  }
}

function formatWhatsAppMessage(alert, aiVerdict) {
  const body = alert.message || "";
  const lines = body.split("\n");

  let reason = "Unknown";
  let location = "";
  let mapsUrl = "";

  for (const line of lines) {
    const alertMatch = line.match(/^ALERT:\s*(.+)/);
    if (alertMatch) {
      reason = alertMatch[1].trim();
    }

    const locMatch = line.match(/^Location:\s*([-\d.]+,[-\d.]+)/);
    if (locMatch) {
      location = locMatch[1];
    }

    const urlMatch = line.match(/(https:\/\/maps\.google\.com\/[^\s]+)/);
    if (urlMatch) {
      mapsUrl = urlMatch[1];
    }
  }

  let msg = `🚨 Anti-Theft ALERT\nReason: ${reason}`;

  if (location) {
    msg += `\nLocation: ${location}`;
  }
  if (mapsUrl) {
    msg += `\n${mapsUrl}`;
  }
  if (aiVerdict) {
    msg += `\n\n🤖 AI Analysis: ${aiVerdict}`;
  }

  // Keep under 600 chars (expanded for AI verdict)
  return msg.slice(0, 600);
}

function formatSMSMessage(alert, photoUrl) {
  const body = alert.message || "";
  const lines = body.split("\n");

  let reason = "Unknown";
  let mapsUrl = "";

  for (const line of lines) {
    const alertMatch = line.match(/ALERT:\s*(.+)/);
    if (alertMatch) reason = alertMatch[1].trim();

    const urlMatch = line.match(/(https:\/\/maps\.google\.com\/[^\s)]+)/);
    if (urlMatch) mapsUrl = urlMatch[1];
  }

  // Single concise line: reason + Maps URL + photo URL.
  // Target ≤160 chars to avoid multi-segment billing on long-code SMS.
  let msg = `ALERT: ${reason}`;
  if (mapsUrl) msg += ` ${mapsUrl}`;
  if (photoUrl) msg += ` Photo: ${photoUrl}`;
  return msg.slice(0, 160);
}

// ── Outbound SMS via Twilio ──────────────────────────────────
async function sendSMS(env, message) {
  const url = `https://api.twilio.com/2010-04-01/Accounts/${env.TWILIO_ACCOUNT_SID}/Messages.json`;
  const auth = btoa(`${env.TWILIO_ACCOUNT_SID}:${env.TWILIO_AUTH_TOKEN}`);

  const body = new URLSearchParams({
    To: env.TWILIO_SMS_TO,
    From: env.TWILIO_SMS_FROM,
    Body: message,
  });

  const resp = await fetch(url, {
    method: "POST",
    headers: {
      Authorization: `Basic ${auth}`,
      "Content-Type": "application/x-www-form-urlencoded",
    },
    body: body.toString(),
  });

  if (!resp.ok) {
    const err = await resp.text();
    // Parse Twilio error JSON for structured logging.
    // Error 30034 = carrier violation (10DLC campaign not yet approved).
    // Error 21610 = unsubscribed recipient.
    try {
      const errData = JSON.parse(err);
      const code = errData.code || resp.status;
      if (code === 30034) {
        console.error(`Twilio SMS BLOCKED code=30034 (10DLC campaign pending): ${errData.message}`);
      } else {
        console.error(`Twilio SMS error code=${code}: ${errData.message}`);
      }
    } catch {
      console.error(`Twilio SMS error HTTP ${resp.status}: ${err.slice(0, 300)}`);
    }
    return false;
  }

  // NOTE: Twilio may accept the message (HTTP 201) but fail delivery
  // asynchronously with error 30034. That failure only shows in Twilio
  // Console or via a status callback webhook, not in this response.
  return true;
}

// ── Inbound SMS webhook from Twilio ──────────────────────────
async function handleInboundSMS(request, env) {
  // Parse form body
  const formData = await request.formData();
  const params = Object.fromEntries(formData);

  // Validate Twilio signature (HMAC-SHA1)
  const signature = request.headers.get("X-Twilio-Signature") || "";
  const requestUrl = new URL(request.url);
  // Twilio signs against the full URL as configured in the webhook
  const signUrl = requestUrl.origin + requestUrl.pathname;

  const isValid = await verifyTwilioSignature(
    env.TWILIO_AUTH_TOKEN,
    signUrl,
    params,
    signature
  );
  if (!isValid) {
    console.error("Invalid Twilio signature");
    return new Response("Forbidden", { status: 403 });
  }

  // Sender whitelist — compare last 10 digits
  const from = (params.From || "").replace(/[\s\-+]/g, "");
  const fromLast10 = from.slice(-10);
  if (fromLast10 !== OWNER_LAST10) {
    console.error(`Rejected SMS from ${params.From}`);
    return new Response("Forbidden", { status: 403 });
  }

  // Parse command
  const rawBody = (params.Body || "").trim().toUpperCase();
  if (!VALID_COMMANDS.includes(rawBody)) {
    return twiml(`Unknown command. Valid: ${VALID_COMMANDS.join(" ")}`);
  }

  // Write to KV — SMS commands use the KV path (Worker→KV is same-edge = fast).
  await enqueueCommand(env, rawBody, "sms");

  // Optimistic status update — same as dashboard dispatch, so the dashboard
  // reflects SMS-triggered state changes without waiting for the cron cycle.
  const SMS_STATUS_CMDS = { ARM: "armed", DISARM: "armed", IMMOBILIZE: "immobilized", RESTORE: "immobilized" };
  if (rawBody in SMS_STATUS_CMDS) {
    const stored = parseSystemStatus(await env.ANTITHEFT_STATE.get("system_status"));
    stored[SMS_STATUS_CMDS[rawBody]] = (rawBody === "ARM" || rawBody === "IMMOBILIZE");
    stored.ts = Date.now();
    await env.ANTITHEFT_STATE.put("system_status", JSON.stringify(stored));
  }

  return twiml(`Command queued: ${rawBody}`);
}

// ── Command Queue (KV-only) ──────────────────────────────────
// All commands flow through KV: dashboard → /commands/dispatch → KV,
// SMS → /sms/inbound → KV.  LILYGO polls /commands/poll (authenticated
// by DEVICE_POLL_TOKEN).  Single source eliminates duplicate delivery.

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
  if (!raw) return { armed: false, immobilized: false, ts: 0 };
  try { return JSON.parse(raw); } catch { return { armed: false, immobilized: false, ts: 0 }; }
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

  // Write to KV — sole command path for both dashboard and SMS.
  const id = await enqueueCommand(env, body, "dashboard");
  const writeTs = Date.now();

  // Optimistic status update — lets the dashboard confirm the command
  // via GET /status without waiting for the full ntfy→cron round-trip.
  const STATUS_CMDS = { ARM: "armed", DISARM: "armed", IMMOBILIZE: "immobilized", RESTORE: "immobilized" };
  if (body in STATUS_CMDS) {
    const stored = parseSystemStatus(await env.ANTITHEFT_STATE.get("system_status"));
    stored[STATUS_CMDS[body]] = (body === "ARM" || body === "IMMOBILIZE");
    stored.ts = writeTs;
    await env.ANTITHEFT_STATE.put("system_status", JSON.stringify(stored));
  }

  return corsJson({ status: "queued", command: body, id, writeTs });
}

function twiml(message) {
  const xml = `<Response><Message>${escapeXml(message)}</Message></Response>`;
  return new Response(xml, {
    headers: { "Content-Type": "text/xml" },
  });
}

function escapeXml(s) {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

async function verifyTwilioSignature(authToken, url, params, expected) {
  // Build the data string: URL + sorted param keys with values concatenated
  const sortedKeys = Object.keys(params).sort();
  let data = url;
  for (const key of sortedKeys) {
    data += key + params[key];
  }

  const enc = new TextEncoder();
  const key = await crypto.subtle.importKey(
    "raw",
    enc.encode(authToken),
    { name: "HMAC", hash: "SHA-1" },
    false,
    ["sign"]
  );
  const sig = await crypto.subtle.sign("HMAC", key, enc.encode(data));

  // Base64 encode and compare in constant time to prevent timing attacks
  const computed = btoa(String.fromCharCode(...new Uint8Array(sig)));
  if (computed.length !== expected.length) return false;
  let result = 0;
  for (let i = 0; i < computed.length; i++) {
    result |= computed.charCodeAt(i) ^ expected.charCodeAt(i);
  }
  return result === 0;
}
