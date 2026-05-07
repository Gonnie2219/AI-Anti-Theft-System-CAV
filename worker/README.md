# antitheft-whatsapp-sms-bridge

Cloudflare Worker that polls ntfy.sh for anti-theft alerts and forwards them to WhatsApp + SMS via Twilio. Also handles inbound SMS commands from Twilio webhooks.

## Why

US A2P 10DLC regulations prevent unregistered long-code SMS delivery on T-Mobile. WhatsApp via Twilio Sandbox bypasses this restriction for urgent alerts. SMS is sent in parallel via Twilio's Messages API.

## How It Works

### Outbound (alerts → user)

1. Cron trigger fires every minute
2. Worker polls ntfy.sh for new messages on the alert topic
3. Filters for `Anti-Theft ALERT`, `Photo Evidence`, and `Command Reply` titles
4. Alerts → sends both WhatsApp + SMS via Twilio (with per-channel dedup/retry)
5. Command Replies → sends SMS only (response to user's inbound command)
6. Deduplicates using KV with JSON `{whatsapp, sms}` tracking (24h TTL)

### Inbound (user → system)

1. User texts a command (ARM, DISARM, STATUS, PHOTO, GPS, HELP) to the Twilio number
2. Twilio sends webhook to `POST /sms/inbound`
3. Worker validates Twilio signature (HMAC-SHA1) and sender whitelist
4. Posts command to ntfy command topic (`antitheft-gonnie-2219-cmd`)
5. Returns TwiML response: "Command queued: ARM" (user gets immediate SMS confirmation)
6. LILYGO polls the command topic every 15s and executes the command

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
wrangler secret put TWILIO_SMS_FROM    # +12186566685 (trial Twilio number, full SMS capability)
wrangler secret put TWILIO_SMS_TO      # +16093589220
wrangler secret put NTFY_TOPIC         # antitheft-gonnie-2219
```

### Configure Twilio Inbound Webhook

In Twilio Console → Phone Numbers → your number → Messaging → "A message comes in":
- URL: `https://<your-worker>.workers.dev/sms/inbound`
- Method: POST

### Trial Account & 10DLC

Currently using **Twilio trial account** with number `+12186566685` (Nisswa, MN — full SMS+MMS+Voice). The trial account can only send to verified Caller IDs (`+16093589220` is verified).

**Trial account limitation:** All outbound SMS are prefixed with "Sent from your Twilio trial account - " automatically. This is cosmetic and goes away after upgrading to a paid account.

**Production rollout:** Sending to non-verified recipients requires Twilio paid upgrade + 10DLC Sole Proprietor registration.

## Debug

```bash
wrangler tail
```

## Test

### Outbound (alert → WhatsApp + SMS)

```bash
curl -X POST https://ntfy.sh/antitheft-gonnie-2219 \
  -H "Title: Anti-Theft ALERT" \
  -H "Priority: urgent" \
  -d '**ALERT:** Test (SMS bridge)
Time: 26/05/04,15:15:00

Location: 42.345882,-83.055489

[View Location on Google Maps](https://maps.google.com/?q=42.345882,-83.055489)'
```

Expect: WhatsApp + SMS within 60s.

### Inbound (SMS command → system)

Text `STATUS` from +16093589220 to the Twilio number. Expect:
- Immediate SMS reply: "Command queued: STATUS"
- ntfy cmd topic receives "STATUS" message
- LILYGO picks up within 15s

## Twilio WhatsApp Sandbox Caveat

Sandbox sessions expire every **72 hours**. To rejoin, send:

```
join <your-sandbox-keyword>
```

to **+1 415 523 8886** on WhatsApp.

## Twilio Trial Account Caveat

All outbound SMS from the trial account are automatically prefixed with:

> Sent from your Twilio trial account -

This prefix is added by Twilio and cannot be removed until the account is upgraded to paid. It does not affect functionality — commands and alerts work normally.
