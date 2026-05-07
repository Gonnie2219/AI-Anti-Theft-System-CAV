/**
 * Cloudflare Worker: Anti-Theft Alert Bridge + Command Queue
 *
 * Two delivery paths for alerts:
 *   FAST: LILYGO POSTs directly to /ingest → immediate WhatsApp delivery
 *   SAFE: Cron polls ntfy every 60s → safety-net WhatsApp + photo matching
 * Both paths share a body-content-hash dedup key in KV.
 *
 * Command path (inbound SMS → vehicle):
 *   Twilio webhook → /sms/inbound → KV queue (cmd_queue) → LILYGO polls /commands/poll
 *   Dashboard      → /commands/dispatch → KV queue        → LILYGO polls /commands/poll
 *
 * Status path (vehicle → dashboard):
 *   LILYGO posts "System X" to ntfy → cron extracts → KV system_status → GET /status
 *
 * Required secrets (set via `wrangler secret put`):
 *   TWILIO_ACCOUNT_SID, TWILIO_AUTH_TOKEN,
 *   TWILIO_FROM, TWILIO_TO          — WhatsApp (whatsapp:+… prefix)
 *   TWILIO_SMS_FROM, TWILIO_SMS_TO  — SMS (plain +1… numbers)
 *   NTFY_TOPIC, ANALYZE_URL (Vercel /api/analyze endpoint)
 *
 * KV namespace binding: ANTITHEFT_STATE
 */

const VALID_COMMANDS = ["ARM", "DISARM", "STATUS", "PHOTO", "GPS", "HELP", "IMMOBILIZE", "RESTORE"];
const OWNER_LAST10 = "6093589220";
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

    // CORS preflight for dashboard cross-origin requests
    if (request.method === "OPTIONS" && (url.pathname.startsWith("/commands/") || url.pathname === "/status")) {
      return new Response(null, {
        headers: {
          "Access-Control-Allow-Origin": "*",
          "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
          "Access-Control-Allow-Headers": "Content-Type",
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
  if (!raw) return { whatsapp: false, sms: false };
  if (raw === "1") return { whatsapp: true, sms: true }; // legacy: fully delivered
  try {
    return JSON.parse(raw);
  } catch {
    return { whatsapp: false, sms: false };
  }
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

  // Write dedup key so cron doesn't re-send the text alert.
  // Cron can still send a follow-up photo if one arrives.
  await env.ANTITHEFT_STATE.put(
    dedupKey,
    JSON.stringify({ whatsapp: waOk, sms: smsOk, ingest: true }),
    { expirationTtl: 86400 }
  );

  return Response.json({ status: "ok", whatsapp: waOk, sms: smsOk });
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
    if (dedup.whatsapp && dedup.sms) {
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

  // Queue command in KV for LILYGO to poll via /commands/poll
  await enqueueCommand(env, rawBody, "sms");

  return twiml(`Command queued: ${rawBody}`);
}

// ── Command Queue (single-key KV FIFO) ───────────────────────
// Uses a single KV key "cmd_queue" instead of list() with prefix,
// because KV list() has up to 60s eventual consistency across edge
// locations, which is too slow for 5-second polling.

const CMD_QUEUE_KEY = "cmd_queue";
const CMD_MAX_AGE_MS = 300000; // 5 minutes

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

async function enqueueCommand(env, command, source) {
  const id = `${Date.now()}-${crypto.randomUUID().slice(0, 8)}`;
  const entry = { id, command, ts: Date.now(), source };

  // Read current queue, append, prune stale entries, write back.
  // Race condition window is tiny (two commands in same ms) and
  // worst case is one command overwritten — user retries.
  const raw = await env.ANTITHEFT_STATE.get(CMD_QUEUE_KEY);
  let queue = [];
  if (raw) {
    try { queue = JSON.parse(raw); } catch {}
  }
  const cutoff = Date.now() - CMD_MAX_AGE_MS;
  queue = queue.filter((e) => e.ts > cutoff);
  queue.push(entry);

  await env.ANTITHEFT_STATE.put(CMD_QUEUE_KEY, JSON.stringify(queue), {
    expirationTtl: 600,
  });
  return id;
}

async function handleCommandsPoll(request, env) {
  const url = new URL(request.url);
  const since = url.searchParams.get("since") || "0";

  const raw = await env.ANTITHEFT_STATE.get(CMD_QUEUE_KEY);
  let queue = [];
  if (raw) {
    try { queue = JSON.parse(raw); } catch {}
  }

  const commands = [];
  let latestId = "";

  for (const entry of queue) {
    if (entry.id > latestId) latestId = entry.id;
    if (entry.id > since) {
      commands.push({ id: entry.id, command: entry.command, ts: entry.ts });
    }
  }

  // If queue is empty, return current time so LILYGO can advance
  // past the first-boot sentinel "0" without waiting for a real command.
  if (!latestId) latestId = String(Date.now());

  return corsJson({ commands, latestId });
}

async function handleCommandsDispatch(request, env) {
  const body = (await request.text()).trim().toUpperCase();

  if (!VALID_COMMANDS.includes(body)) {
    return corsJson(
      { error: `Invalid command. Valid: ${VALID_COMMANDS.join(" ")}` },
      400
    );
  }

  const id = await enqueueCommand(env, body, "dashboard");
  return corsJson({ status: "queued", command: body, id });
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

  // Base64 encode
  const computed = btoa(String.fromCharCode(...new Uint8Array(sig)));
  return computed === expected;
}
