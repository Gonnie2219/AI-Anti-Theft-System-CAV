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
  const messages = await pollNtfy(env);
  if (messages.length === 0) return;

  // Separate into alerts and photos
  const alerts = messages.filter((m) => m.title.startsWith("Anti-Theft ALERT"));
  const photos = messages.filter((m) => m.title === "Photo Evidence");

  console.log(`Found ${alerts.length} alert(s), ${photos.length} photo(s)`);

  for (const alert of alerts) {
    const dedupKey = `msg:${alert.id}`;
    if (await env.ANTITHEFT_STATE.get(dedupKey)) {
      console.log(`Skipping duplicate: ${alert.id}`);
      continue;
    }

    // Find matching photo within 30s after the alert
    const matchedPhoto = photos.find(
      (p) => p.time >= alert.time && p.time - alert.time <= 30
    );

    const mediaUrl =
      matchedPhoto && matchedPhoto.attachment && matchedPhoto.attachment.url
        ? matchedPhoto.attachment.url
        : null;

    const message = formatWhatsAppMessage(alert);
    const success = await sendWhatsApp(env, message, mediaUrl);

    if (!success) {
      console.error(`Failed to send alert ${alert.id}, stopping batch`);
      return;
    }

    await env.ANTITHEFT_STATE.put(dedupKey, "1", { expirationTtl: 86400 });
    if (matchedPhoto) {
      await env.ANTITHEFT_STATE.put(`msg:${matchedPhoto.id}`, "1", {
        expirationTtl: 86400,
      });
      console.log(`Forwarded alert ${alert.id} with photo ${matchedPhoto.id}`);
    } else {
      console.log(`Forwarded alert ${alert.id} (no photo matched)`);
    }
  }

  // Mark orphan photos as consumed so they don't pile up
  for (const photo of photos) {
    const dedupKey = `msg:${photo.id}`;
    if (!(await env.ANTITHEFT_STATE.get(dedupKey))) {
      await env.ANTITHEFT_STATE.put(dedupKey, "1", { expirationTtl: 86400 });
      console.log(`Consumed orphan photo ${photo.id}`);
    }
  }

  // Advance cursor past all processed messages
  const latestTime = Math.max(...messages.map((m) => m.time));
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

  // Filter: alerts and photo evidence messages
  return messages.filter(
    (msg) =>
      msg.event === "message" &&
      msg.title &&
      (msg.title.startsWith("Anti-Theft ALERT") ||
        msg.title === "Photo Evidence")
  );
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
