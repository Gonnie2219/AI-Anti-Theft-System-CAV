/**
 * Cloudflare Worker: ntfy.sh → WhatsApp Bridge
 *
 * Polls ntfy.sh for anti-theft alerts and forwards them to WhatsApp via Twilio.
 * Deployed with cron trigger (every minute).
 *
 * Required secrets (set via `wrangler secret put`):
 *   TWILIO_ACCOUNT_SID, TWILIO_AUTH_TOKEN, TWILIO_FROM, TWILIO_TO, NTFY_TOPIC
 *
 * KV namespace binding: ANTITHEFT_STATE
 */

export default {
  async scheduled(event, env, ctx) {
    ctx.waitUntil(handleCron(env));
  },

  async fetch(request, env) {
    const lastPoll = await env.ANTITHEFT_STATE.get("last_poll_timestamp");
    return Response.json({
      status: "ok",
      worker: "antitheft-whatsapp-bridge",
      last_poll_timestamp: lastPoll || "never",
    });
  },
};

async function handleCron(env) {
  const alerts = await pollNtfy(env);
  if (alerts.length === 0) return;

  console.log(`Found ${alerts.length} alert(s) to forward`);

  for (const alert of alerts) {
    // Deduplication: skip if already sent
    const dedupKey = `msg:${alert.id}`;
    const existing = await env.ANTITHEFT_STATE.get(dedupKey);
    if (existing) {
      console.log(`Skipping duplicate: ${alert.id}`);
      continue;
    }

    const message = formatWhatsAppMessage(alert);
    const success = await sendWhatsApp(env, message);

    if (!success) {
      console.error(`Failed to send alert ${alert.id}, stopping batch`);
      return; // Don't advance cursor — retry next cron
    }

    // Mark as sent with 24h TTL
    await env.ANTITHEFT_STATE.put(dedupKey, "1", { expirationTtl: 86400 });
    console.log(`Forwarded alert ${alert.id}`);
  }

  // All alerts sent successfully — advance cursor to latest message time
  const latestTime = Math.max(...alerts.map((a) => a.time));
  await env.ANTITHEFT_STATE.put("last_poll_timestamp", String(latestTime));
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

  // Filter: only message events with anti-theft alert title
  return messages.filter(
    (msg) =>
      msg.event === "message" &&
      msg.title &&
      msg.title.startsWith("Anti-Theft ALERT")
  );
}

async function sendWhatsApp(env, message) {
  const url = `https://api.twilio.com/2010-04-01/Accounts/${env.TWILIO_ACCOUNT_SID}/Messages.json`;
  const auth = btoa(`${env.TWILIO_ACCOUNT_SID}:${env.TWILIO_AUTH_TOKEN}`);

  const body = new URLSearchParams({
    To: env.TWILIO_TO,
    From: env.TWILIO_FROM,
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
    console.error(`Twilio error ${resp.status}: ${err}`);
    return false;
  }

  return true;
}

function formatWhatsAppMessage(alert) {
  const body = alert.message || "";
  const lines = body.split("\n");

  let reason = "Unknown";
  let location = "";
  let mapsUrl = "";

  for (const line of lines) {
    const alertMatch = line.match(/\*\*ALERT:\*\*\s*(.+)/);
    if (alertMatch) {
      reason = alertMatch[1].trim();
    }

    const locMatch = line.match(/^Location:\s*([-\d.]+,[-\d.]+)/);
    if (locMatch) {
      location = locMatch[1];
    }

    const urlMatch = line.match(/\[View Location on Google Maps\]\((https:\/\/[^)]+)\)/);
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

  // Keep under 500 chars
  return msg.slice(0, 500);
}
