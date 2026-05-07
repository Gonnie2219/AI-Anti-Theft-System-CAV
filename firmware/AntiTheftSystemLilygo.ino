/*
 * Anti-Theft System  -  LILYGO T-SIM7600G-H Cellular Gateway
 *
 * Receives ALERT and STATUS commands from the Main ESP32 over UART2,
 * sends SMS alerts to the owner, and POSTs notifications + photo evidence
 * to ntfy.sh. Receives SMS commands from the owner and forwards them
 * back to Main over UART.
 *
 * SIM:  Hologram (AT&T/T-Mobile multi-carrier),  APN "hologram"
 *
 * Network notes:
 *   Hologram assigns IPv4 PDP addresses. We use the modem's built-in
 *   HTTP client (AT+HTTPINIT / AT+HTTPACTION) for ntfy.sh communication.
 *   SMS is routed through Twilio via a Cloudflare Worker webhook bridge
 *   (no native AT+CMGS needed).
 *
 * UART map:
 *   Serial   (UART0)  =  USB debug
 *   Serial1  (UART1)  =  SIM7600 modem (TX=27, RX=26)
 *   Serial2  (UART2)  =  Main ESP32     (TX=19, RX=21)
 */

#include <Arduino.h>
#include <HardwareSerial.h>
#include <Preferences.h>

// ── Pins ─────────────────────────────────────────────────────
#define MODEM_TX        27
#define MODEM_RX        26
#define MODEM_PWRKEY     4
#define MODEM_FLIGHT    25
#define LED_PIN         12
#define MAIN_RX         21
#define MAIN_TX         19

// ── Configuration ────────────────────────────────────────────
const char    APN[]            = "hologram";
const char    NTFY_TOPIC[]     = "antitheft-gonnie-2219";
const char    OWNER_PHONE[]    = "+16093589220";

#define USE_NATIVE_SMS      0          // 1 = SpeedTalk AT+CMGS, 0 = Twilio via ntfy

#define IMG_BUF_SIZE        51200      // 50 KB photo buffer
#define HEARTBEAT_MS      300000UL     // 5 minutes
#define GPS_UPDATE_MS     300000UL     // 5 minutes
#define NTFY_CMD_TOPIC   "antitheft-gonnie-2219-cmd"
#define NTFY_CMD_POLL_MS    5000UL     // 5 seconds
#define WORKER_INGEST_URL  "https://antitheft-whatsapp-bridge.gonnie2219.workers.dev/ingest"

#if USE_NATIVE_SMS
#define SMS_POLL_MS        60000UL     // safety-net SMS poll
#define SMS_MAX_PER_HOUR       10
#endif

// ── Aliases ──────────────────────────────────────────────────
#define SerialMon  Serial
#define SerialAT   Serial1
HardwareSerial SerialMain(2);

// ── Globals ──────────────────────────────────────────────────
uint8_t*      imgBuffer     = nullptr;
size_t        imgSize       = 0;
bool          networkReady  = false;
bool          systemArmed   = false;
String        gpsLat, gpsLon, gpsMapsLink;
unsigned long lastHeartbeat = 0;
unsigned long lastGPS       = 0;

// ntfy command polling
Preferences   preferences;
String        lastCmdId;
unsigned long lastCmdPoll   = 0;

#if USE_NATIVE_SMS
unsigned long lastSMSPoll   = 0;
int           smsCount      = 0;
unsigned long smsHourStart  = 0;
#endif

// ─────────────────────────────────────────────────────────────
//  AT command helper
// ─────────────────────────────────────────────────────────────
String sendAT(String cmd, String expect, unsigned long timeout) {
  while (SerialAT.available()) SerialAT.read();
  SerialAT.println(cmd);
  unsigned long start = millis();
  String resp;
  while (millis() - start < timeout) {
    while (SerialAT.available()) resp += (char)SerialAT.read();
    if (resp.indexOf(expect) >= 0) break;
    delay(10);
  }
  return resp;
}

// ─────────────────────────────────────────────────────────────
//  HTTP client  (uses SIM7600 built-in HTTPINIT subsystem)
// ─────────────────────────────────────────────────────────────
//
// The HTTPINIT subsystem is a separate network stack from NETOPEN/
// CIPOPEN. Each request goes: HTTPTERM (cleanup) -> HTTPINIT ->
// HTTPPARA (URL/content/headers) -> HTTPDATA (upload body) ->
// HTTPACTION (execute) -> wait for +HTTPACTION URC -> HTTPTERM.
//
// NOTE: We use HTTP, not HTTPS. The SIM7600 supports AT+HTTPSSL=1
// but in practice it fails TLS handshakes with ntfy.sh (certificate
// chain issues, modem firmware limitations). Alert content and GPS
// coordinates are sent in cleartext. Accepted trade-off for now —
// the ntfy topic name provides obscurity, not security.

static int waitHttpAction(unsigned long timeout) {
  unsigned long start = millis();
  String acc;
  while (millis() - start < timeout) {
    while (SerialAT.available()) acc += (char)SerialAT.read();
    int idx = acc.indexOf("+HTTPACTION:");
    if (idx >= 0) {
      int eol = acc.indexOf('\n', idx);
      if (eol > idx) {
        // Format:  +HTTPACTION: <method>,<status>,<datalen>
        String urc = acc.substring(idx, eol);
        int c1 = urc.indexOf(',');
        int c2 = urc.indexOf(',', c1 + 1);
        if (c1 > 0 && c2 > c1) {
          return urc.substring(c1 + 1, c2).toInt();
        }
      }
    }
    delay(20);
  }
  return -1;
}

static bool waitDownloadPrompt(unsigned long timeout) {
  unsigned long start = millis();
  String r;
  while (millis() - start < timeout) {
    while (SerialAT.available()) r += (char)SerialAT.read();
    if (r.indexOf("DOWNLOAD") >= 0) return true;
    if (r.indexOf("ERROR") >= 0) return false;
    delay(10);
  }
  return false;
}

static bool waitOK(unsigned long timeout) {
  unsigned long start = millis();
  String r;
  while (millis() - start < timeout) {
    while (SerialAT.available()) r += (char)SerialAT.read();
    if (r.indexOf("OK") >= 0) return true;
    if (r.indexOf("ERROR") >= 0) return false;
    delay(10);
  }
  return false;
}

static bool httpInit(String url, String contentType, String headers) {
  if (sendAT("AT+HTTPTERM", "OK", 1000).indexOf("OK") >= 0) {
    // ok, was active, now closed
  }
  while (SerialAT.available()) SerialAT.read();

  // Toggle SSL based on URL scheme
  sendAT("AT+HTTPSSL=" + String(url.startsWith("https") ? 1 : 0), "OK", 2000);

  String initResp = sendAT("AT+HTTPINIT", "OK", 5000);
  if (initResp.indexOf("OK") < 0) {
    SerialMon.println("[HTTP] HTTPINIT failed. Modem said: " + initResp);
    return false;
  }
  sendAT("AT+HTTPPARA=\"URL\",\"" + url + "\"", "OK", 3000);
  sendAT("AT+HTTPPARA=\"CONNECTTO\",30", "OK", 2000);
  sendAT("AT+HTTPPARA=\"RECVTO\",30", "OK", 2000);
  if (contentType.length() > 0) {
    sendAT("AT+HTTPPARA=\"CONTENT\",\"" + contentType + "\"", "OK", 3000);
  }
  if (headers.length() > 0) {
    String esc = headers;
    esc.replace("\"", "\\\"");
    sendAT("AT+HTTPPARA=\"USERDATA\",\"" + esc + "\"", "OK", 3000);
  }
  return true;
}

// Returns HTTP status code (200 = success) or -1 for TCP/connection failure.
int httpPostText(String topic, String headers, String body) {
  SerialMon.println("[HTTP] POST /" + topic + "  (" + String(body.length()) + " bytes)");
  if (!httpInit("http://ntfy.sh/" + topic, "text/plain", headers)) {
    sendAT("AT+HTTPTERM", "OK", 1000);
    return -1;
  }

  SerialAT.print("AT+HTTPDATA=");
  SerialAT.print(body.length());
  SerialAT.println(",10000");
  if (!waitDownloadPrompt(3000)) { sendAT("AT+HTTPTERM", "OK", 1000); return -1; }
  SerialAT.print(body);
  if (!waitOK(15000))            { sendAT("AT+HTTPTERM", "OK", 1000); return -1; }

  sendAT("AT+HTTPACTION=1", "OK", 3000);
  int status = waitHttpAction(60000);

  sendAT("AT+HTTPTERM", "OK", 1000);
  if (status < 0)       SerialMon.println("[HTTP] TCP fail");
  else if (status == 200) SerialMon.println("[HTTP] OK");
  else                    SerialMon.println("[HTTP] HTTP " + String(status));
  return status;
}

// Returns HTTP status code (200 = success) or -1 for TCP/connection failure.
int httpPostBinary(String topic, String headers, const uint8_t* data, size_t len) {
  SerialMon.println("[HTTP] POST /" + topic + "  (" + String(len) + " binary bytes)");
  if (len > 100000) { SerialMon.println("[HTTP] body too large"); return -1; }

  if (!httpInit("http://ntfy.sh/" + topic, "application/octet-stream", headers)) {
    sendAT("AT+HTTPTERM", "OK", 1000);
    return -1;
  }

  SerialAT.print("AT+HTTPDATA=");
  SerialAT.print(len);
  SerialAT.println(",60000");
  if (!waitDownloadPrompt(3000)) { sendAT("AT+HTTPTERM", "OK", 1000); return -1; }

  size_t sent = 0;
  while (sent < len) {
    size_t chunk = (len - sent > 1024) ? 1024 : (len - sent);
    SerialAT.write(data + sent, chunk);
    sent += chunk;
    delay(10);
  }
  if (!waitOK(30000)) { sendAT("AT+HTTPTERM", "OK", 1000); return -1; }

  sendAT("AT+HTTPACTION=1", "OK", 3000);
  int status = waitHttpAction(120000);

  sendAT("AT+HTTPTERM", "OK", 1000);
  if (status < 0)       SerialMon.println("[HTTP] TCP fail");
  else if (status == 200) SerialMon.println("[HTTP] OK");
  else                    SerialMon.println("[HTTP] HTTP " + String(status));
  return status;
}

// ─────────────────────────────────────────────────────────────
//  Retry wrappers — exponential backoff, skip retry on 4xx
// ─────────────────────────────────────────────────────────────
bool httpPostTextRetry(String topic, String headers, String body) {
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) {
      unsigned long backoff = 2000UL << (attempt - 1);  // 2s, 4s
      SerialMon.println("[HTTP] retry " + String(attempt) + "/2 in " + String(backoff) + "ms");
      delay(backoff);
    }
    int rc = httpPostText(topic, headers, body);
    if (rc == 200) return true;
    if (rc >= 400 && rc < 500) {
      SerialMon.println("[HTTP] 4xx client error, not retrying");
      return false;
    }
  }
  return false;
}

bool httpPostBinaryRetry(String topic, String headers, const uint8_t* data, size_t len) {
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) {
      unsigned long backoff = 2000UL << (attempt - 1);
      SerialMon.println("[HTTP] retry " + String(attempt) + "/2 in " + String(backoff) + "ms");
      delay(backoff);
    }
    int rc = httpPostBinary(topic, headers, data, len);
    if (rc == 200) return true;
    if (rc >= 400 && rc < 500) {
      SerialMon.println("[HTTP] 4xx client error, not retrying");
      return false;
    }
  }
  return false;
}

// ─────────────────────────────────────────────────────────────
//  Direct POST to arbitrary URL (for Worker /ingest fast path)
// ─────────────────────────────────────────────────────────────
int httpPostDirect(const char* url, String body) {
  SerialMon.println("[INGEST] POST (" + String(body.length()) + " bytes)");
  if (!httpInit(String(url), "text/plain", "")) {
    sendAT("AT+HTTPTERM", "OK", 1000);
    return -1;
  }

  SerialAT.print("AT+HTTPDATA=");
  SerialAT.print(body.length());
  SerialAT.println(",10000");
  if (!waitDownloadPrompt(3000)) { sendAT("AT+HTTPTERM", "OK", 1000); return -1; }
  SerialAT.print(body);
  if (!waitOK(15000))            { sendAT("AT+HTTPTERM", "OK", 1000); return -1; }

  sendAT("AT+HTTPACTION=1", "OK", 3000);
  int status = waitHttpAction(30000);

  sendAT("AT+HTTPTERM", "OK", 1000);
  if (status == 200) SerialMon.println("[INGEST] OK");
  else               SerialMon.println("[INGEST] failed: " + String(status));
  return status;
}

// ─────────────────────────────────────────────────────────────
//  JSON helper (no library — simple field extraction)
// ─────────────────────────────────────────────────────────────
String extractJsonString(const String& json, const String& field) {
  String key = "\"" + field + "\":\"";
  int start = json.indexOf(key);
  if (start < 0) return "";
  start += key.length();
  int end = json.indexOf('"', start);
  if (end < 0) return "";
  return json.substring(start, end);
}

// ─────────────────────────────────────────────────────────────
//  HTTP read body (for GET responses)
// ─────────────────────────────────────────────────────────────
String httpReadBody(int maxLen) {
  String cmd = "AT+HTTPREAD=0," + String(maxLen);
  while (SerialAT.available()) SerialAT.read();
  SerialAT.println(cmd);

  unsigned long start = millis();
  String acc;
  while (millis() - start < 10000) {
    while (SerialAT.available()) acc += (char)SerialAT.read();
    if (acc.indexOf("OK") >= 0 || acc.indexOf("ERROR") >= 0) break;
    delay(20);
  }

  // Parse: +HTTPREAD: <len>\r\n<body>\r\nOK
  int idx = acc.indexOf("+HTTPREAD:");
  if (idx < 0) return "";
  int nl = acc.indexOf('\n', idx);
  if (nl < 0) return "";
  int okIdx = acc.indexOf("\r\nOK", nl);
  if (okIdx < 0) okIdx = acc.indexOf("\nOK", nl);
  if (okIdx < 0) return acc.substring(nl + 1);
  return acc.substring(nl + 1, okIdx);
}

// ─────────────────────────────────────────────────────────────
//  ntfy command polling (Twilio SMS → Worker → ntfy → here)
// ─────────────────────────────────────────────────────────────
void pollNtfyCommands() {
  // On first boot (lastCmdId=="0"), since=0 returns up to 12h of cached
  // messages. We must advance the cursor past them without executing,
  // otherwise stale ARM/PHOTO commands fire unexpectedly.
  bool skipExecution = (lastCmdId == "0");
  if (skipExecution) {
    SerialMon.println("[CMD] first boot — advancing cursor past stale messages");
  }

  String url = "http://ntfy.sh/" + String(NTFY_CMD_TOPIC)
             + "/json?poll=1&since=" + lastCmdId;

  if (!httpInit(url, "", "")) {
    sendAT("AT+HTTPTERM", "OK", 1000);
    return;
  }

  sendAT("AT+HTTPACTION=0", "OK", 3000);  // GET
  int status = waitHttpAction(15000);

  if (status != 200) {
    sendAT("AT+HTTPTERM", "OK", 1000);
    if (status > 0) SerialMon.println("[CMD] poll HTTP " + String(status));
    return;
  }

  String body = httpReadBody(4096);
  sendAT("AT+HTTPTERM", "OK", 1000);

  if (body.length() == 0) return;

  // Each line is a JSON object
  int pos = 0;
  String newestId;
  while (pos < (int)body.length()) {
    int eol = body.indexOf('\n', pos);
    if (eol < 0) eol = body.length();
    String line = body.substring(pos, eol);
    line.trim();
    pos = eol + 1;

    if (line.length() == 0) continue;

    String event = extractJsonString(line, "event");
    if (event != "message") continue;

    String id  = extractJsonString(line, "id");
    String msg = extractJsonString(line, "message");
    if (id.length() == 0 || msg.length() == 0) continue;

    newestId = id;
    msg.trim();
    msg.toUpperCase();

    if (skipExecution) {
      SerialMon.println("[CMD] skipped (first boot): " + msg);
    } else {
      SerialMon.println("[CMD] ntfy: " + msg);
      handleSMSCommand(msg);
    }
  }

  if (newestId.length() > 0) {
    lastCmdId = newestId;
    preferences.putString("lastCmdId", lastCmdId);
  }
}

// ─────────────────────────────────────────────────────────────
//  GPS
// ─────────────────────────────────────────────────────────────
void updateGPS() {
  String r = sendAT("AT+CGPSINFO", "OK", 2000);
  SerialMon.println("[GPS] raw: " + r);
  int idx = r.indexOf("+CGPSINFO:");
  if (idx < 0) return;
  String info = r.substring(idx + 11);
  info.trim();
  if (info.length() < 10 || info.charAt(0) == ',') return;

  int c1 = info.indexOf(',');
  int c2 = info.indexOf(',', c1 + 1);
  int c3 = info.indexOf(',', c2 + 1);
  int c4 = info.indexOf(',', c3 + 1);
  if (c4 < 0) return;

  float lat = info.substring(0, 2).toFloat() + info.substring(2, c1).toFloat() / 60.0;
  if (info.substring(c1 + 1, c2) == "S") lat = -lat;
  float lon = info.substring(c2 + 1, c2 + 4).toFloat() + info.substring(c2 + 4, c3).toFloat() / 60.0;
  if (info.substring(c3 + 1, c4) == "W") lon = -lon;

  gpsLat = String(lat, 6);
  gpsLon = String(lon, 6);
  gpsMapsLink = "https://maps.google.com/?q=" + gpsLat + "," + gpsLon;
}

String getModemTime() {
  String r = sendAT("AT+CCLK?", "OK", 2000);
  int i = r.indexOf("+CCLK: \"");
  if (i < 0) return "";
  int j = r.indexOf("\"", i + 8);
  if (j <= i) return "";
  return r.substring(i + 8, j);
}

// ─────────────────────────────────────────────────────────────
//  SMS — outbound (rate limited)
// ─────────────────────────────────────────────────────────────
#if USE_NATIVE_SMS
bool sendSMS(String to, String body) {
  unsigned long now = millis();
  if (now - smsHourStart > 3600000UL) { smsHourStart = now; smsCount = 0; }
  if (smsCount >= SMS_MAX_PER_HOUR) {
    SerialMon.println("[SMS] rate limit reached");
    return false;
  }

  SerialMon.println("[SMS] -> " + to + " (" + String(body.length()) + " chars)");

  while (SerialAT.available()) SerialAT.read();
  SerialAT.print("AT+CMGS=\"");
  SerialAT.print(to);
  SerialAT.println("\"");

  // Wait for '>' prompt
  unsigned long start = millis();
  String pre;
  bool gotPrompt = false;
  while (millis() - start < 5000) {
    while (SerialAT.available()) pre += (char)SerialAT.read();
    if (pre.indexOf(">") >= 0) { gotPrompt = true; break; }
    delay(20);
  }
  if (!gotPrompt) { SerialMon.println("[SMS] no prompt"); return false; }

  SerialAT.print(body);
  SerialAT.write((uint8_t)26);  // Ctrl+Z to send

  start = millis();
  String post;
  while (millis() - start < 30000) {
    while (SerialAT.available()) post += (char)SerialAT.read();
    if (post.indexOf("+CMGS:") >= 0 && post.indexOf("OK") >= 0) {
      smsCount++;
      SerialMon.println("[SMS] sent (" + String(smsCount) + "/" + String(SMS_MAX_PER_HOUR) + " this hour)");
      return true;
    }
    if (post.indexOf("ERROR") >= 0) { SerialMon.println("[SMS] error"); return false; }
    delay(50);
  }
  SerialMon.println("[SMS] timeout");
  return false;
}

#endif // USE_NATIVE_SMS

// ─────────────────────────────────────────────────────────────
//  SMS — inbound
// ─────────────────────────────────────────────────────────────
static String last10(String s) {
  s.replace(" ", ""); s.replace("-", ""); s.replace("+", "");
  int len = s.length();
  return (len <= 10) ? s : s.substring(len - 10);
}

static bool isAuthorized(String number) {
  return last10(number) == last10(String(OWNER_PHONE));
}

void handleSMSCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();
  SerialMon.println("[CMD] " + cmd);

  if (cmd == "ARM") {
    SerialMain.println("SMS_CMD:ARM");
  } else if (cmd == "DISARM") {
    SerialMain.println("SMS_CMD:DISARM");
  } else if (cmd == "STATUS") {
    SerialMain.println("SMS_CMD:STATUS");
  } else if (cmd == "PHOTO") {
    SerialMain.println("SMS_CMD:PHOTO");
  } else if (cmd == "IMMOBILIZE") {
    SerialMain.println("SMS_CMD:IMMOBILIZE");
  } else if (cmd == "RESTORE") {
    SerialMain.println("SMS_CMD:RESTORE");
  } else if (cmd == "GPS") {
    updateGPS();
    String gpsReply = gpsLat.length() > 0
      ? ("SMS_REPLY: GPS: " + gpsLat + "," + gpsLon + " " + gpsMapsLink)
      : "SMS_REPLY: GPS: no fix";
    httpPostText(NTFY_TOPIC, "Title: Command Reply\r\nPriority: default\r\nTags: speech_balloon", gpsReply);
  } else if (cmd == "HELP") {
    httpPostText(NTFY_TOPIC, "Title: Command Reply\r\nPriority: default\r\nTags: speech_balloon",
                 "SMS_REPLY: Commands: ARM DISARM STATUS PHOTO GPS IMMOBILIZE RESTORE HELP");
  }
  // Unknown commands silently dropped (Worker already validated)
}

#if USE_NATIVE_SMS
void pollIncomingSMS() {
  String r = sendAT("AT+CMGL=\"REC UNREAD\"", "OK", 5000);
  int idx = 0;
  while ((idx = r.indexOf("+CMGL:", idx)) >= 0) {
    int eol = r.indexOf('\n', idx);
    if (eol < 0) break;
    String header = r.substring(idx, eol);

    // Quoted fields:  index, "REC UNREAD", "<sender>", , "<datetime>"
    int q1 = header.indexOf('"');
    int q2 = header.indexOf('"', q1 + 1);
    int q3 = header.indexOf('"', q2 + 1);
    int q4 = header.indexOf('"', q3 + 1);
    if (q3 < 0 || q4 < 0) { idx = eol + 1; continue; }
    String sender = header.substring(q3 + 1, q4);

    int bodyStart = eol + 1;
    int bodyEnd = r.indexOf("+CMGL:", bodyStart);
    if (bodyEnd < 0) bodyEnd = r.indexOf("\nOK", bodyStart);
    if (bodyEnd < 0) bodyEnd = r.length();
    String body = r.substring(bodyStart, bodyEnd);
    body.trim();

    if (isAuthorized(sender)) handleSMSCommand(body);
    else SerialMon.println("[SMS] rejected from " + sender);

    idx = bodyEnd;
  }
  sendAT("AT+CMGD=1,1", "OK", 3000);   // delete read messages
}
#endif // USE_NATIVE_SMS

// ─────────────────────────────────────────────────────────────
//  Photo reception from Main ESP32
// ─────────────────────────────────────────────────────────────
bool receiveImage() {
  imgSize = 0;
  unsigned long timeout = millis() + 30000;

  while (millis() < timeout) {
    if (!SerialMain.available()) { delay(10); continue; }

    String line = SerialMain.readStringUntil('\n');
    line.trim();
    if (line == "NOIMG") return false;
    if (!line.startsWith("IMG:")) continue;

    size_t expected = line.substring(4).toInt();
    if (expected == 0 || expected > IMG_BUF_SIZE || !imgBuffer) return false;

    SerialMon.println("[IMG] expecting " + String(expected) + " bytes");
    unsigned long byteTimeout = millis() + 20000;
    while (imgSize < expected && millis() < byteTimeout) {
      size_t avail = SerialMain.available();
      if (avail > 0) {
        size_t toRead = expected - imgSize;
        if (avail < toRead) toRead = avail;
        SerialMain.readBytes(imgBuffer + imgSize, toRead);
        imgSize += toRead;
      }
    }

    delay(100);
    while (SerialMain.available()) SerialMain.readStringUntil('\n');  // drain IMG_END

    SerialMon.println("[IMG] got " + String(imgSize) + "/" + String(expected));
    return (imgSize == expected);
  }
  return false;
}

// ─────────────────────────────────────────────────────────────
//  Notifications
// ─────────────────────────────────────────────────────────────
void handleAlert(String reason) {
  bool isPhotoRequest = (reason == "Photo Requested");
  SerialMon.println(isPhotoRequest ? "\n[PHOTO_REQ]" : "\n[ALERT] " + reason);
  digitalWrite(LED_PIN, HIGH);

  // 1) Receive image from Main if it sends one. Must come FIRST so that
  //    Serial2 RX buffer doesn't overflow during multi-second AT operations.
  bool hasImage = receiveImage();

  // 2) Update GPS
  updateGPS();

#if USE_NATIVE_SMS
  // 3) SMS first — it's the fastest and most reliable notification channel.
  //    Skip SMS for user-initiated photo requests.
  if (!isPhotoRequest) {
    String smsBody = "ALERT: " + reason;
    if (gpsMapsLink.length() > 0) smsBody += "\n" + gpsMapsLink;
    sendSMS(OWNER_PHONE, smsBody);
  }
#endif

  // 4) ntfy text notification (real alerts forwarded to WhatsApp by Worker)
  String body, headers;
  if (isPhotoRequest) {
    body = "Photo captured on request";
    headers = "Title: Photo Capture\r\nPriority: low\r\nTags: camera";
  } else {
    body = "ALERT: " + reason;
    headers = "Title: Anti-Theft Alert\r\nPriority: urgent\r\nTags: rotating_light";
    if (gpsMapsLink.length() > 0) headers += "\r\nClick: " + gpsMapsLink;
  }
  String ts = getModemTime();
  if (ts.length() > 0)         body += "\nTime: " + ts;
  if (gpsLat.length() == 0)
    SerialMon.println("[GPS] GPS not yet acquired - sent without location");
  if (gpsLat.length() > 0)     body += "\nLocation: " + gpsLat + "," + gpsLon + "\n" + gpsMapsLink;

  bool ntfyOk = httpPostTextRetry(NTFY_TOPIC, headers, body);

  // 4b) Fast path: POST directly to Worker for immediate WhatsApp delivery.
  //     Single attempt, no retry — cron safety net handles failures.
  //     Only for real alerts (photo requests don't need WhatsApp).
  if (!isPhotoRequest) {
    int ingestRc = httpPostDirect(WORKER_INGEST_URL, body);
    SerialMon.println(ingestRc == 200
      ? "[INGEST] Worker accepted"
      : "[INGEST] Worker failed (cron safety net will handle)");
  }

  // 5) ntfy photo (if we have one)
  if (hasImage && imgSize > 0) {
    String imgHdrs = isPhotoRequest
      ? "Title: Requested Photo\r\nFilename: photo.jpg"
      : "Title: Photo Evidence\r\nFilename: alert.jpg";
    httpPostBinaryRetry(NTFY_TOPIC, imgHdrs, imgBuffer, imgSize);
  }

  // 6) Acknowledge to Main
  SerialMain.println(ntfyOk ? "LILYGO_OK: notified" : "LILYGO_ERROR");

  digitalWrite(LED_PIN, LOW);
  SerialMon.println(isPhotoRequest ? "[PHOTO_REQ] complete\n" : "[ALERT] complete\n");
}

void handleStatus(String status) {
  systemArmed = (status == "ARMED");
  SerialMon.println("[STATUS] " + status);

  String body = "System " + status;
  if (gpsLat.length() > 0) body += "\nLocation: " + gpsLat + "," + gpsLon;

  String h = "Title: System " + status + "\r\nPriority: low\r\nTags: ";
  h += (status == "ARMED") ? "lock" : "unlock";
  httpPostText(NTFY_TOPIC, h, body);
}

void sendHeartbeat() {
  SerialMon.println("[HEARTBEAT]");
  updateGPS();

  String body = "System online.";
  String ts = getModemTime();
  if (ts.length() > 0)     body += "\nTime: " + ts;
  if (gpsLat.length() > 0) body += "\nLocation: " + gpsLat + "," + gpsLon;

  httpPostText(NTFY_TOPIC, "Title: Heartbeat\r\nPriority: min\r\nTags: green_circle", body);
}

// ─────────────────────────────────────────────────────────────
//  Setup
// ─────────────────────────────────────────────────────────────
void setup() {
  SerialMon.begin(115200);
  delay(1000);
  SerialMon.println("\n=================================");
  SerialMon.println(" Anti-Theft Cellular Gateway");
  SerialMon.println("   APN: " + String(APN));
  SerialMon.println("=================================\n");

  imgBuffer = (uint8_t*)malloc(IMG_BUF_SIZE);
  SerialMon.println(imgBuffer ? "Image buffer OK" : "Image buffer FAIL");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  SerialMain.setRxBufferSize(16384);
  SerialMain.begin(115200, SERIAL_8N1, MAIN_RX, MAIN_TX);
  SerialMon.println("UART2 -> Main ESP32");

  // Power on modem
  pinMode(MODEM_FLIGHT, OUTPUT); digitalWrite(MODEM_FLIGHT, HIGH);
  pinMode(MODEM_PWRKEY, OUTPUT); digitalWrite(MODEM_PWRKEY, HIGH);
  delay(300); digitalWrite(MODEM_PWRKEY, LOW);

  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  SerialMon.println("Waiting for modem...");
  delay(6000);

  bool online = false;
  for (int i = 0; i < 10; i++) {
    if (sendAT("AT", "OK", 1000).indexOf("OK") >= 0) { online = true; break; }
    delay(1000);
  }
  if (!online) {
    SerialMon.println("Modem failed to respond");
    SerialMain.println("LILYGO_ERROR");
    return;
  }
  SerialMon.println("Modem online");

  // Echo off (critical: prevents AT+CMGS body from being echoed into the SMS)
  sendAT("ATE0", "OK", 1000);

  // Verbose error reporting — turns generic "ERROR" into "+CME ERROR: <code>"
  // so we can see exactly why the modem refuses a command.
  sendAT("AT+CMEE=2", "OK", 1000);

  // Set APN with IPv4 PDP type. Hologram provides native IPv4.
  // Radio cycle (CFUN=0/1) forces re-attach with the new profile.
  String cfun0 = sendAT("AT+CFUN=0", "OK", 10000);
  if (cfun0.indexOf("OK") < 0) SerialMon.println("[SETUP] CFUN=0 failed: " + cfun0);
  delay(2000);
  String apnR = sendAT("AT+CGDCONT=1,\"IP\",\"" + String(APN) + "\"", "OK", 3000);
  if (apnR.indexOf("OK") < 0) SerialMon.println("[SETUP] APN config failed: " + apnR);
  sendAT("AT+CGAUTH=1,0,\"\",\"\"", "OK", 3000);
  String cfun1 = sendAT("AT+CFUN=1", "OK", 15000);
  if (cfun1.indexOf("OK") < 0) SerialMon.println("[SETUP] CFUN=1 failed: " + cfun1);
  delay(3000);

  // Wait for data registration
  SerialMon.print("Registering");
  for (int i = 0; i < 30; i++) {
    String r = sendAT("AT+CGREG?", "OK", 2000);
    if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) {
      networkReady = true;
      SerialMon.println(" OK");
      break;
    }
    SerialMon.print(".");
    delay(2000);
  }
  if (!networkReady) SerialMon.println(" FAILED");

  // Force packet-switched attach (CGREG=1 only confirms registration,
  // not actual data attach. CGATT=1 is the explicit "give me data" command).
  String cgatt = sendAT("AT+CGATT=1", "OK", 10000);
  if (cgatt.indexOf("OK") < 0) SerialMon.println("[SETUP] PS attach failed: " + cgatt);

  // Activate PDP context (HTTPINIT will use whichever cid is active)
  String cgact = sendAT("AT+CGACT=1,1", "OK", 10000);
  if (cgact.indexOf("OK") < 0) SerialMon.println("[SETUP] PDP activate failed: " + cgact);

  // Log diagnostic info: PDP profile, addresses (v4 and v6 if dual-stack
  // worked), full context profile with DNS. Together these tell us
  // whether the bearer is actually up and what the network gave us.
  String csq      = sendAT("AT+CSQ",         "OK", 2000);
  String cgattQuery = sendAT("AT+CGATT?",     "OK", 2000);
  String cgdcont  = sendAT("AT+CGDCONT?",    "OK", 2000);
  String cgaddr   = sendAT("AT+CGPADDR=1",   "OK", 2000);
  String contrdp  = sendAT("AT+CGCONTRDP=1", "OK", 3000);
  SerialMon.println("Signal: "    + csq.substring(csq.indexOf("+CSQ:")));
  SerialMon.println("PS attach: " + cgattQuery.substring(cgattQuery.indexOf("+CGATT:")));
  SerialMon.println("PDP type:   " + cgdcont.substring(cgdcont.indexOf("+CGDCONT:")));
  SerialMon.println("Address:   "  + cgaddr.substring(cgaddr.indexOf("+CGPADDR:")));
  SerialMon.println("Profile:   "  + contrdp.substring(contrdp.indexOf("+CGCONTRDP:")));

  // Extra diagnostics
  String cpsi = sendAT("AT+CPSI?", "OK", 2000);
  String ceer = sendAT("AT+CEER", "OK", 2000);
  SerialMon.println("SysInfo:   " + cpsi.substring(cpsi.indexOf("+CPSI:")));
  SerialMon.println("LastErr:   " + ceer.substring(ceer.indexOf("+CEER:")));

  // GPS on
  String gpsResp = sendAT("AT+CGPS=1", "OK", 2000);
  if (gpsResp.indexOf("OK") >= 0) SerialMon.println("[GPS] AT+CGPS=1 OK - GPS started");
  else if (gpsResp.indexOf("ERROR") >= 0) SerialMon.println("[GPS] AT+CGPS=1 FAILED: " + gpsResp);
  else SerialMon.println("[GPS] AT+CGPS=1 unexpected: " + gpsResp);

#if USE_NATIVE_SMS
  // SMS configuration (native AT+CMGS path)
  sendAT("AT+CMGF=1",                  "OK", 2000);  // text mode
  sendAT("AT+CSCS=\"GSM\"",            "OK", 2000);  // GSM character set
  sendAT("AT+CPMS=\"ME\",\"ME\",\"ME\"", "OK", 3000); // store in modem memory
  sendAT("AT+CNMI=2,1,0,0,0",          "OK", 2000);  // +CMTI URC on new SMS
  sendAT("AT+CMGD=1,4",                "OK", 5000);  // delete any stale SMS
#endif

  // NVS — persist last ntfy command ID across reboots
  preferences.begin("antitheft", false);
  lastCmdId = preferences.getString("lastCmdId", "0");
  // Validate: ntfy poll=1 requires a numeric timestamp or message ID (alphanumeric).
  // "now" (old default) causes HTTP 400. Reset any invalid value.
  if (lastCmdId == "now" || lastCmdId.length() == 0) {
    lastCmdId = "0";
    preferences.putString("lastCmdId", lastCmdId);
  }
  SerialMon.println("Last cmd ID: " + lastCmdId);

  SerialMon.println("Setup complete\n");
  SerialMain.println("LILYGO_READY");

  // Startup notification
  delay(2000);
  updateGPS();
  String startBody = "System online.";
  String ts = getModemTime();
  if (ts.length() > 0) startBody += "\nTime: " + ts;

  if (networkReady) {
    int rc = httpPostText(NTFY_TOPIC, "Title: Startup\r\nPriority: low\r\nTags: rocket", startBody);
    SerialMon.println(rc == 200 ? "Startup HTTP OK" : "Startup HTTP failed (continuing)");
  }
#if USE_NATIVE_SMS
  sendSMS(OWNER_PHONE, "Anti-theft system online.");
#endif

  digitalWrite(LED_PIN, LOW);
  lastHeartbeat = millis();
  lastCmdPoll   = millis();
  lastGPS       = millis();
#if USE_NATIVE_SMS
  lastSMSPoll   = millis();
#endif
}

// ─────────────────────────────────────────────────────────────
//  Main loop
// ─────────────────────────────────────────────────────────────
void loop() {
  // 1) Commands from Main ESP32
  if (SerialMain.available()) {
    String cmd = SerialMain.readStringUntil('\n');
    cmd.trim();
    if      (cmd.startsWith("ALERT:"))      handleAlert(cmd.substring(6));
    else if (cmd.startsWith("STATUS:"))     handleStatus(cmd.substring(7));
    else if (cmd.startsWith("SMS_REPLY:")) {
      String reply = cmd.substring(10);
      SerialMon.println("[REPLY] " + reply);
      httpPostText(NTFY_TOPIC, "Title: Command Reply\r\nPriority: default\r\nTags: speech_balloon", "SMS_REPLY: " + reply);
    }
  }

#if USE_NATIVE_SMS
  // 2) SMS URC notifications (native SMS path)
  if (SerialAT.available()) {
    String urc = SerialAT.readStringUntil('\n');
    urc.trim();
    if (urc.indexOf("+CMTI:") >= 0) pollIncomingSMS();
  }
#endif

  unsigned long now = millis();

  // 3) Poll ntfy command topic (Twilio SMS → Worker → ntfy → here)
  if (now - lastCmdPoll > NTFY_CMD_POLL_MS) {
    SerialMon.println("[CMD] poll (every " + String(NTFY_CMD_POLL_MS / 1000) + "s)");
    pollNtfyCommands();
    lastCmdPoll = now;
  }

#if USE_NATIVE_SMS
  // 4) Periodic SMS poll (catches URCs eaten by other AT calls)
  if (now - lastSMSPoll > SMS_POLL_MS) {
    pollIncomingSMS();
    lastSMSPoll = now;
  }
#endif

  // 5) Periodic GPS update
  if (now - lastGPS > GPS_UPDATE_MS) {
    updateGPS();
    lastGPS = now;
  }

  // 6) Periodic heartbeat
  if (now - lastHeartbeat > HEARTBEAT_MS) {
    sendHeartbeat();
    lastHeartbeat = now;
  }

  delay(50);
}
