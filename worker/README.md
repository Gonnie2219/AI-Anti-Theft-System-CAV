# antitheft-whatsapp-bridge

Cloudflare Worker that polls ntfy.sh for anti-theft alerts and forwards them to WhatsApp via Twilio.

## Why

US A2P 10DLC regulations prevent unregistered long-code SMS delivery on T-Mobile. WhatsApp via Twilio Sandbox bypasses this restriction for urgent alerts.

## How It Works

1. Cron trigger fires every minute
2. Worker polls ntfy.sh for new messages on the alert topic
3. Filters for `Anti-Theft ALERT` titles only (ignores heartbeats, status changes, photos)
4. Formats a concise WhatsApp message with reason + GPS location
5. Sends via Twilio WhatsApp API
6. Deduplicates using KV (24h TTL per message ID)

## Deploy

```bash
cd worker

# Create KV namespace and update wrangler.toml with the returned ID
wrangler kv namespace create ANTITHEFT_STATE

# Deploy the worker
wrangler deploy

# Set secrets (you'll be prompted for each value)
wrangler secret put TWILIO_ACCOUNT_SID
wrangler secret put TWILIO_AUTH_TOKEN
wrangler secret put TWILIO_FROM        # whatsapp:+14155238886 (Twilio sandbox)
wrangler secret put TWILIO_TO          # whatsapp:+1YOURNUMBER
wrangler secret put NTFY_TOPIC         # antitheft-gonnie-2219
```

## Debug

```bash
wrangler tail
```

## Test

Send a test alert to ntfy and wait up to 60s for the cron to fire:

```bash
curl -X POST https://ntfy.sh/antitheft-gonnie-2219 \
  -H "Title: Anti-Theft ALERT" \
  -H "Priority: urgent" \
  -d '**ALERT:** Test (Worker Deploy)
Time: 26/04/20,15:15:00

Location: 42.345882,-83.055489

[View Location on Google Maps](https://maps.google.com/?q=42.345882,-83.055489)'
```

## Twilio WhatsApp Sandbox Caveat

Sandbox sessions expire every **72 hours**. To rejoin, send:

```
join <your-sandbox-keyword>
```

to **+1 415 523 8886** on WhatsApp.
