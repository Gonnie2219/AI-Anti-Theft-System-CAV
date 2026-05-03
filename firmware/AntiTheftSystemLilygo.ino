/*
 * ANTI-THEFT SYSTEM — LILYGO T-SIM7600G-H
 * Cellular gateway: receives alert + photo from Main ESP32, uploads to ntfy.sh.
 * SMS channel: sends alert SMS, receives and processes inbound SMS commands.
 * Board: ESP32 Dev Module
 *
 * Serial  (UART0) = USB debug
 * Serial1 (UART1) = SIM7600 modem (GPIO26 RX / GPIO27 TX)
 * Serial2 (UART2) = Main ESP32    (GPIO21 RX / GPIO19 TX)
 *
 * Wiring: Main GPIO27 TX -> LILYGO GPIO21 RX
 *         Main GPIO26 RX <- LILYGO GPIO19 TX
 *         GND <-> GND
 *
 * SpeedTalk migration (May 2026):
 * - APN switched from "hologram" to "Wholesale" (SpeedTalk's MVNO APN)
 * - PDP context now IPV4V6 (SpeedTalk is IPv6-only; modem may negotiate
 *   464XLAT to give us transparent IPv4 reachability — if not, we fall
 *   back to NAT64 to reach ntfy.sh's IPv4 address)
 * - SMS send/receive ENABLED (previously stubbed out for Hologram).
 *   Outbound: alarm SMS with reason + GPS + photo URL.
 *   Inbound: ARM, DISARM, STATUS, PHOTO, GPS, HELP via +CMTI URC plus
 *            AT+CMGL safety-net poll. Whitelist by last-10-digits.
 *            Rate limit: SMS_MAX_PER_HOUR outbound messages per rolling hour.
 *
 * Power optimizations (May 2026):
 * - ntfy command poll: 5s -> 60s (12x reduction in cellular wake-ups)
 * - Heartbeat: 2 min -> 30 min (15x reduction in HTTP POSTs)
 * - GPS update period: 30s -> 5 min (modem GPS keeps running; this just
 *   throttles how often we read coordinates over the AT bus)
 * - Boot startup TCP test removed (the startup notification IS the test)
 * - Alert handler no longer redundantly re-updates GPS (periodic loop just
 *   refreshed it; saves one AT command on the critical alarm path)
 */

#define MODEM_TX        27
#define MODEM_RX        26
#define MODEM_PWRKEY     4
#define MODEM_DTR       32
#define MODEM_FLIGHT    25
#define MODEM_STATUS    34
#define LED_PIN         12
#define MAIN_RX         21
#define MAIN_TX         19

#define SerialMon   Serial
#define SerialAT    Serial1

// ── Configuration ────────────────────────────────────────────
const char APN[]        = "Wholesale";              // SpeedTalk MVNO APN
const char NTFY_TOPIC[] = "antitheft-gonnie-2219";
const char CMD_TOPIC[]  = "antitheft-gonnie-2219-cmd";

#define OWNER_PHONE   "+16093589220"
#define SYSTEM_PHONE  "+13132081968"   // SpeedTalk SIM (informational, for STATUS replies)

// ntfy.sh endpoints — try IPv4 first (works if modem does 464XLAT
// transparently), then fall back to well-known NAT64 prefix
// (RFC 6052 64:ff9b::/96 mapped onto 159.203.148.75 = 9f.cb.94.4b).
const char NTFY_IPV4[]  = "159.203.148.75";
const char NTFY_NAT64[] = "64:ff9b::9fcb:944b";

// SMS rate limiting (rolling-hour window)
#define SMS_MAX_PER_HOUR 10
int           smsCount = 0;
unsigned long smsWindowStart = 0;

// Image transfer buffer (sized for VGA JPEG @ quality 10)
#define IMG_BUF_SIZE 51200
uint8_t* imgBuffer = NULL;
size_t   imgSize = 0;

// State
bool          networkReady   = false;
bool          systemArmed    = false;          // tracked from STATUS: messages
String        gpsLat = "", gpsLon = "", gpsMapsLink = "";
String        batteryPercent = "", batteryVoltage = "";
String        lastPollId = "0";
bool          smsPhotoRequested = false;

// Timers — see the "power optimizations" header for rationale
unsigned long lastGPSUpdate   = 0;
unsigned long lastHeartbeat   = 0;
unsigned long lastPoll        = 0;
unsigned long lastSMSCheck    = 0;
#define GPS_UPDATE_MS    300000UL   // 5 min
#define HEARTBEAT_MS    1800000UL   // 30 min
#define POLL_INTERVAL_MS  60000UL   // 1 min (ntfy command poll)
#define SMS_CHECK_MS      30000UL   // 30 s (CMGL safety-net for missed +CMTI)

// Forward declarations
String sendAT(String cmd, String exp, unsigned long t);
String sendATWait(String cmd, unsigned long t);
void   ensureNetwork();
bool   tcpConnect();
bool   cipSend(const uint8_t* data, size_t len);
bool   cipSend(const String& data);
String tcpReceiveData(unsigned long timeout);
int    parseHttpStatus(const String& resp);
bool   sendHttpText(String path, String ntfyHeaders, String body);
bool   sendHttpBinary(String path, String ntfyHeaders, const uint8_t* data, size_t len);
String sendHttpGet(String path);
String getModemTime();
int    voltageToPercent(float v);
void   getBatteryLevel();
String getBatteryString();
bool   receiveImage();
bool   sendWithImage(String reason);
bool   sendTextOnly(String reason);
bool   sendStatusNotification(String status);
bool   sendHeartbeat();
void   pollCommands();
bool   sendCommandAck(String msg);
bool   sendGPSResponse(String body);
void   updateGPS();
bool   sendSMS(String to, String body);
void   sendAlertSMS(String reason);
void   checkSMSNotifications();
void   checkUnreadSMS();
void   readAndProcessSMS(int index);
bool   isAuthorizedSender(String number);
void   handleSMSCommand(String cmd);
String waitForMainReply(unsigned long timeout);

// ── AT Helpers ───────────────────────────────────────────────
String sendAT(String cmd, String exp, unsigned long t) {
  SerialAT.println(cmd);
  unsigned long s = millis(); String r = "";
  while (millis()-s < t) {
    while (SerialAT.available()) r += (char)SerialAT.read();
    if (r.indexOf(exp) >= 0) break;
    delay(10);
  }
  return r;
}

String sendATWait(String cmd, unsigned long t) {
  SerialAT.println(cmd);
  unsigned long s = millis(); String r = "";
  while (millis()-s < t) {
    while (SerialAT.available()) r += (char)SerialAT.read();
    if (r.indexOf("OK") >= 0 || r.indexOf("ERROR") >= 0) break;
    delay(10);
  }
  return r;
}

// ── Network Recovery ─────────────────────────────────────────
void ensureNetwork() {
  String r = sendAT("AT+CREG?", "OK", 2000);
  if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) { networkReady = true; return; }

  SerialMon.println("Network lost - re-registering...");
  networkReady = false;
  sendATWait("AT+CGACT=1,1", 10000);
  sendAT("AT+CGAUTH=1,0,\"\",\"\"","OK",3000);
  sendATWait("AT+NETOPEN", 15000);
  sendAT("AT+CIPRXGET=1", "OK", 3000);
  delay(2000);
  while (SerialAT.available()) SerialAT.read();

  r = sendAT("AT+CREG?", "OK", 2000);
  if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) {
    networkReady = true;
    SerialMon.println("Network recovered");
  } else {
    SerialMon.println("Network recovery failed");
  }
}

// ── TCP/IP Helpers (raw CIPOPEN socket) ──────────────────────
bool tcpConnect() {
  SerialMon.println("[TCP] Closing any prior connection");
  sendATWait("AT+CIPCLOSE=0", 3000);
  while (SerialAT.available()) SerialAT.read();

  // Attempt A: IPv4 (works if 464XLAT is transparent on this carrier)
  SerialMon.println("[TCP] Connecting to " + String(NTFY_IPV4) + ":80");
  String r = sendAT("AT+CIPOPEN=0,\"TCP\",\"" + String(NTFY_IPV4) + "\",80",
                    "+CIPOPEN: 0,0", 15000);
  if (r.indexOf("+CIPOPEN: 0,0") >= 0) {
    SerialMon.println("[TCP] Connected via IPv4");
    return true;
  }
  SerialMon.println("[TCP] IPv4 failed: " + r.substring(0, min((int)r.length(), 200)));

  sendATWait("AT+CIPCLOSE=0", 3000);
  while (SerialAT.available()) SerialAT.read();

  // Attempt B: NAT64 (well-known prefix; works on networks that advertise it)
  SerialMon.println("[TCP] Connecting to " + String(NTFY_NAT64) + ":80");
  r = sendAT("AT+CIPOPEN=0,\"TCP\",\"" + String(NTFY_NAT64) + "\",80",
             "+CIPOPEN: 0,0", 15000);
  if (r.indexOf("+CIPOPEN: 0,0") >= 0) {
    SerialMon.println("[TCP] Connected via NAT64");
    return true;
  }
  SerialMon.println("[TCP] NAT64 failed: " + r.substring(0, min((int)r.length(), 200)));

  return false;
}

bool cipSend(const uint8_t* data, size_t len) {
  String cmd = "AT+CIPSEND=0," + String(len);
  SerialMon.println("[TCP] > " + cmd);
  String r = sendAT(cmd, ">", 5000);
  if (r.indexOf(">") < 0) {
    SerialMon.println("[TCP] No > prompt: " + r.substring(0, min((int)r.length(), 200)));
    return false;
  }

  SerialAT.write(data, len);

  // Wait for +CIPSEND: 0, confirmation
  unsigned long s = millis();
  r = "";
  while (millis() - s < 10000) {
    while (SerialAT.available()) r += (char)SerialAT.read();
    if (r.indexOf("+CIPSEND: 0,") >= 0) {
      SerialMon.println("[TCP] Send confirmed (" + String(len) + " bytes)");
      return true;
    }
    if (r.indexOf("ERROR") >= 0) break;
    delay(10);
  }
  SerialMon.println("[TCP] Send failed: " + r.substring(0, min((int)r.length(), 200)));
  return false;
}

bool cipSend(const String& data) {
  return cipSend((const uint8_t*)data.c_str(), data.length());
}

String tcpReceiveData(unsigned long timeout) {
  // Wait for +CIPRXGET: 1,0 URC (data available)
  unsigned long s = millis();
  String urc = "";
  while (millis() - s < timeout) {
    while (SerialAT.available()) urc += (char)SerialAT.read();
    if (urc.indexOf("+CIPRXGET: 1,0") >= 0) break;
    delay(10);
  }
  if (urc.indexOf("+CIPRXGET: 1,0") < 0) {
    SerialMon.println("[TCP] No CIPRXGET URC (timeout)");
    return "";
  }

  delay(500);  // Let initial data buffer on modem

  // Multi-read loop: request chunks until modem buffer is empty
  String data = "";
  unsigned long loopStart = millis();
  while (millis() - loopStart < 10000) {
    while (SerialAT.available()) SerialAT.read();
    SerialAT.println("AT+CIPRXGET=2,0,1024");

    String raw = "";
    unsigned long readStart = millis();
    int hdrIdx = -1;
    int cnfLen = -1;
    int dataStart = -1;
    while (millis() - readStart < 5000) {
      while (SerialAT.available()) raw += (char)SerialAT.read();
      if (hdrIdx < 0) {
        hdrIdx = raw.indexOf("+CIPRXGET: 2,0,");
        if (hdrIdx >= 0) {
          int comma1 = raw.indexOf(',', hdrIdx + 15);
          if (comma1 >= 0) {
            cnfLen = raw.substring(hdrIdx + 15, comma1).toInt();
            dataStart = raw.indexOf('\n', hdrIdx) + 1;
          }
        }
      }
      if (dataStart > 0 && cnfLen >= 0 && (int)raw.length() >= dataStart + cnfLen) break;
      delay(10);
    }

    if (hdrIdx < 0 || dataStart <= 0) {
      SerialMon.println("[TCP] CIPRXGET parse failed");
      break;
    }
    if (cnfLen == 0) break;

    data += raw.substring(dataStart, dataStart + cnfLen);

    int comma1 = raw.indexOf(',', hdrIdx + 15);
    int remaining = 0;
    if (comma1 >= 0) {
      int nlAfterHdr = raw.indexOf('\n', comma1);
      if (nlAfterHdr > comma1) {
        remaining = raw.substring(comma1 + 1, nlAfterHdr).toInt();
      }
    }
    if (remaining == 0) break;
    delay(100);
  }

  SerialMon.println("[TCP] received " + String(data.length()) + " bytes");
  return data;
}

int parseHttpStatus(const String& resp) {
  int idx = resp.indexOf("HTTP/1.");
  if (idx < 0) return 0;
  int spaceIdx = resp.indexOf(' ', idx);
  if (spaceIdx < 0) return 0;
  int code = resp.substring(spaceIdx + 1, spaceIdx + 4).toInt();
  SerialMon.println("[HTTP] parsed status: " + String(code));
  return code;
}

// POST text body to ntfy.sh, returns true on HTTP 200
bool sendHttpText(String path, String ntfyHeaders, String body) {
  ensureNetwork();
  if (!networkReady) return false;

  SerialMon.println("[HTTP] POST /" + path);
  if (!tcpConnect()) return false;

  String req = "POST /" + path + " HTTP/1.1\r\n";
  req += "Host: ntfy.sh\r\n";
  if (ntfyHeaders.length() > 0) req += ntfyHeaders + "\r\n";
  req += "Content-Length: " + String(body.length()) + "\r\n";
  req += "Connection: close\r\n\r\n";
  req += body;

  bool ok = cipSend(req);
  if (ok) {
    String resp = tcpReceiveData(30000);
    int status = parseHttpStatus(resp);
    SerialMon.println("[HTTP] Status: " + String(status));
    ok = (status == 200);
  }

  sendATWait("AT+CIPCLOSE=0", 3000);
  while (SerialAT.available()) SerialAT.read();
  SerialMon.println(ok ? "  HTTP OK" : "  HTTP failed");
  return ok;
}

// PUT binary body (photo) to ntfy.sh, returns true on HTTP 200
bool sendHttpBinary(String path, String ntfyHeaders, const uint8_t* data, size_t len) {
  ensureNetwork();
  if (!networkReady) return false;

  SerialMon.println("[HTTP] PUT /" + path + " (" + String(len) + " bytes)");
  if (!tcpConnect()) return false;

  String hdrs = "PUT /" + path + " HTTP/1.1\r\n";
  hdrs += "Host: ntfy.sh\r\n";
  hdrs += "Content-Type: application/octet-stream\r\n";
  if (ntfyHeaders.length() > 0) hdrs += ntfyHeaders + "\r\n";
  hdrs += "Content-Length: " + String(len) + "\r\n";
  hdrs += "Connection: close\r\n\r\n";

  if (!cipSend(hdrs)) {
    sendATWait("AT+CIPCLOSE=0", 3000);
    while (SerialAT.available()) SerialAT.read();
    return false;
  }

  size_t sent = 0;
  while (sent < len) {
    size_t chunk = len - sent;
    if (chunk > 1024) chunk = 1024;
    if (!cipSend(data + sent, chunk)) {
      sendATWait("AT+CIPCLOSE=0", 3000);
      while (SerialAT.available()) SerialAT.read();
      return false;
    }
    sent += chunk;
  }

  String resp = tcpReceiveData(60000);
  int status = parseHttpStatus(resp);
  SerialMon.println("[HTTP] Status: " + String(status));
  bool ok = (status == 200);

  sendATWait("AT+CIPCLOSE=0", 3000);
  while (SerialAT.available()) SerialAT.read();
  SerialMon.println(ok ? "  HTTP OK" : "  HTTP failed");
  return ok;
}

// GET from ntfy.sh, returns response body (empty on failure)
String sendHttpGet(String path) {
  ensureNetwork();
  if (!networkReady) return "";

  SerialMon.println("[HTTP] GET /" + path);
  if (!tcpConnect()) return "";

  String req = "GET /" + path + " HTTP/1.1\r\n";
  req += "Host: ntfy.sh\r\n";
  req += "Connection: close\r\n\r\n";

  if (!cipSend(req)) {
    sendATWait("AT+CIPCLOSE=0", 3000);
    while (SerialAT.available()) SerialAT.read();
    return "";
  }

  String resp = tcpReceiveData(30000);
  sendATWait("AT+CIPCLOSE=0", 3000);
  while (SerialAT.available()) SerialAT.read();

  int status = parseHttpStatus(resp);
  SerialMon.println("[HTTP] Status: " + String(status));

  int bodyIdx = resp.indexOf("\r\n\r\n");
  if (bodyIdx < 0) return "";
  String body = resp.substring(bodyIdx + 4);
  SerialMon.println("[HTTP] body length: " + String(body.length()));
  SerialMon.println("[HTTP] body preview: " + body.substring(0, min((int)body.length(), 200)));
  return body;
}

// ── Get Modem Time ──────────────────────────────────────────
String getModemTime() {
  String r = sendAT("AT+CCLK?", "OK", 2000);
  int i = r.indexOf("+CCLK: \"");
  if (i >= 0) {
    int j = r.indexOf("\"", i + 8);
    if (j > i) return r.substring(i + 8, j);
  }
  return "";
}

// ── Battery Level ───────────────────────────────────────────
int voltageToPercent(float v) {
  if (v >= 4.2) return 100;
  if (v >= 4.0) return 75 + (v - 4.0) / (4.2 - 4.0) * 25;
  if (v >= 3.8) return 50 + (v - 3.8) / (4.0 - 3.8) * 25;
  if (v >= 3.6) return 25 + (v - 3.6) / (3.8 - 3.6) * 25;
  if (v >= 3.3) return  5 + (v - 3.3) / (3.6 - 3.3) * 20;
  if (v >= 3.0) return      (v - 3.0) / (3.3 - 3.0) *  5;
  return 0;
}

void getBatteryLevel() {
  String r = sendAT("AT+CBC", "OK", 2000);
  int i = r.indexOf("+CBC:");
  if (i < 0) return;

  String raw = r.substring(i + 5);
  String clean = "";
  for (int j = 0; j < (int)raw.length(); j++) {
    char c = raw.charAt(j);
    if (isDigit(c) || c == '.') clean += c;
    else if (clean.length() > 0) break;
  }
  if (clean.length() == 0) return;

  float volts = clean.toFloat();
  if (volts < 1.0 || volts > 5.0) return;  // sanity check

  batteryVoltage = String(volts, 2);
  batteryPercent = String(voltageToPercent(volts));
}

String getBatteryString() {
  if (batteryPercent.length() > 0 && batteryVoltage.length() > 0)
    return "\nBattery: " + batteryPercent + "% (" + batteryVoltage + "V)";
  return "";
}

// ── Setup ────────────────────────────────────────────────────
void setup() {
  SerialMon.begin(115200);
  delay(1000);
  SerialMon.println("\n====================================");
  SerialMon.println("   Anti-Theft System");
  SerialMon.println("   LILYGO - Cellular Gateway");
  SerialMon.println("   APN: " + String(APN));
  SerialMon.println("====================================\n");

  imgBuffer = (uint8_t*)malloc(IMG_BUF_SIZE);
  SerialMon.println(imgBuffer ? "Image buffer allocated (50 KB)"
                              : "Image buffer allocation FAILED");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  Serial2.setRxBufferSize(16384);
  Serial2.begin(115200, SERIAL_8N1, MAIN_RX, MAIN_TX);
  SerialMon.println("UART2  ->  Main ESP32");

  SerialMon.println("Powering on modem...");
  pinMode(MODEM_FLIGHT, OUTPUT); digitalWrite(MODEM_FLIGHT, HIGH);
  pinMode(MODEM_DTR, OUTPUT);    digitalWrite(MODEM_DTR, LOW);
  pinMode(MODEM_PWRKEY, OUTPUT); digitalWrite(MODEM_PWRKEY, HIGH);
  delay(300); digitalWrite(MODEM_PWRKEY, LOW);
  pinMode(MODEM_STATUS, INPUT);

  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  SerialMon.println("Waiting for modem...");
  delay(6000);

  bool ok = false;
  for (int i = 0; i < 10; i++) {
    if (sendAT("AT","OK",1000).indexOf("OK") >= 0) { ok = true; break; }
    delay(1000);
  }
  if (!ok) {
    SerialMon.println("Modem not responding");
    Serial2.println("LILYGO_ERROR");
    return;
  }
  SerialMon.println("Modem online");

  // Echo off — critical: prevents AT+CMGS body from being echoed back into
  // the SMS payload (cause of the May 1 echo bug)
  sendAT("ATE0","OK",1000);

  // Apply APN with IPV4V6 PDP context. SpeedTalk says they're IPv6-only,
  // but the modem will negotiate whatever the network actually offers
  // (may still get a v4 address if 464XLAT is transparent on this device).
  SerialMon.println("  Cycling radio to apply APN...");
  sendATWait("AT+CFUN=0", 10000);
  delay(1000);
  sendAT("AT+CGDCONT=1,\"IPV4V6\",\"" + String(APN) + "\"","OK",3000);
  sendAT("AT+CGAUTH=1,0,\"\",\"\"","OK",3000);
  sendATWait("AT+CFUN=1", 10000);
  delay(2000);

  SerialMon.print("  Re-registering");
  for (int i = 0; i < 20; i++) {
    String r  = sendAT("AT+CREG?",  "OK", 2000);
    String r2 = sendAT("AT+CGREG?", "OK", 2000);
    bool voiceReg = (r.indexOf(",1")  >= 0 || r.indexOf(",5")  >= 0);
    bool dataReg  = (r2.indexOf(",1") >= 0 || r2.indexOf(",5") >= 0);
    if (voiceReg && dataReg) {
      SerialMon.println(" registered (voice+data)");
      break;
    }
    SerialMon.print("."); delay(2000);
  }

  // Diagnostic: log what IP type(s) we actually got
  String contextResp = sendAT("AT+CGCONTRDP=1", "OK", 5000);
  SerialMon.println("  CGCONTRDP: " + contextResp);

  String cgactResp = sendATWait("AT+CGACT=1,1", 10000);
  SerialMon.println("  CGACT: " + cgactResp);

  String netopenResp = sendATWait("AT+NETOPEN", 15000);
  SerialMon.println("  NETOPEN: " + netopenResp);
  delay(2000); while (SerialAT.available()) SerialAT.read();

  String ipResp = sendAT("AT+CGPADDR=1", "OK", 3000);
  SerialMon.println("  CGPADDR: " + ipResp);
  networkReady = true;
  SerialMon.println("  Network ready");

  // Manual TCP receive mode
  sendAT("AT+CIPRXGET=1", "OK", 3000);

  // ── SMS configuration (RE-ENABLED for SpeedTalk) ──
  SerialMon.println("  Configuring SMS...");
  sendAT("AT+CMGF=1",                "OK", 2000);   // Text mode
  sendAT("AT+CSCS=\"GSM\"",          "OK", 2000);   // GSM character set
  sendAT("AT+CPMS=\"ME\",\"ME\",\"ME\"", "OK", 3000); // Store SMS in modem memory (faster than SIM)
  sendAT("AT+CNMI=2,1,0,0,0",        "OK", 2000);   // +CMTI URC on new SMS
  // Clear any stale SMS from previous boots
  sendATWait("AT+CMGD=1,4", 5000);
  SerialMon.println("  SMS ready");

  // GPS on
  sendATWait("AT+CGPS=1", 3000);

  // Boot TCP test removed — the startup notification below IS the test.

  Serial2.println("LILYGO_READY");
  SerialMon.println("\nSystem ready - awaiting alerts\n");
  digitalWrite(LED_PIN, LOW);

  // Startup notification (also doubles as connectivity self-test).
  // If this fails, we just log and continue — the system still works
  // on the SMS path even with no internet.
  getBatteryLevel();
  updateGPS();
  String startBody = "System powered on and ready.";
  String ts = getModemTime();
  if (ts.length() > 0)            startBody += "\nTime: " + ts;
  if (gpsLat.length() > 0)        startBody += "\nLocation: " + gpsLat + "," + gpsLon;
  startBody += getBatteryString();

  String startHdrs = "Title: System Startup\r\nPriority: low\r\nTags: rocket";
  bool startupHttpOk = sendHttpText(String(NTFY_TOPIC), startHdrs, startBody);
  SerialMon.println(startupHttpOk ? "Startup HTTP: OK"
                                  : "Startup HTTP: FAILED (will retry on next event)");

  // Startup SMS — kept short to fit in one SMS
  String smsBody = "Anti-theft system online.";
  if (gpsMapsLink.length() > 0) smsBody += "\n" + gpsMapsLink;
  smsBody += getBatteryString();
  sendSMS(OWNER_PHONE, smsBody);

  lastHeartbeat = millis();
  lastGPSUpdate = millis();
  lastPoll      = millis();
  lastSMSCheck  = millis();
}

// ── Main Loop ────────────────────────────────────────────────
void loop() {
  // 1) Check for SMS-arrival URCs that landed asynchronously
  checkSMSNotifications();

  // 2) Safety-net poll for unread SMS (catches any +CMTI we missed
  //    while busy with TCP)
  if (millis() - lastSMSCheck > SMS_CHECK_MS) {
    checkUnreadSMS();
    lastSMSCheck = millis();
  }

  // 3) USB debug: type "TESTSMS" to fire a test message to OWNER_PHONE
  if (SerialMon.available()) {
    String dbg = SerialMon.readStringUntil('\n'); dbg.trim();
    if (dbg == "TESTSMS") {
      sendSMS(OWNER_PHONE, "Test SMS from anti-theft system");
    }
  }

  // 4) Handle messages from Main ESP32
  if (Serial2.available()) {
    String cmd = Serial2.readStringUntil('\n'); cmd.trim();

    if (cmd.startsWith("ALERT:")) {
      String reason = cmd.substring(6);
      SerialMon.println("*** ALERT: " + reason + " ***");
      digitalWrite(LED_PIN, HIGH);

      // GPS may have just been refreshed by the periodic loop; only
      // re-update if we don't have a fix yet.
      if (gpsLat.length() == 0) updateGPS();

      // Drain photo from Serial2 BEFORE blocking on SMS — the 50KB image
      // overflows the 16KB Serial2 RX buffer if we block here for 5-30s.
      bool hasImage = receiveImage();

      // SMS next — most carrier-resilient path, still sent before HTTP
      sendAlertSMS(reason);

      // HTTP upload — best effort; SMS already went out
      bool success;
      if (hasImage && imgSize > 0) {
        SerialMon.println("Sending alert + photo (" + String(imgSize) + " bytes)");
        success = sendWithImage(reason);
      } else {
        SerialMon.println("Sending text-only alert");
        success = sendTextOnly(reason);
      }

      // Retry once on failure
      if (!success) {
        SerialMon.println("Retrying in 3 seconds");
        delay(3000);
        success = (hasImage && imgSize > 0) ? sendWithImage(reason) : sendTextOnly(reason);
      }

      Serial2.println(success ? "LILYGO_OK: Notification sent" : "LILYGO_ERROR");
      SerialMon.println(success ? "Notification sent" : "Notification failed");

      // If photo was requested via SMS, send reply with link
      if (smsPhotoRequested && reason == "Photo Requested") {
        smsPhotoRequested = false;
        if (success) {
          sendSMS(OWNER_PHONE, "Photo uploaded:\nhttps://ntfy.sh/" + String(NTFY_TOPIC));
        } else {
          sendSMS(OWNER_PHONE, "Photo upload failed");
        }
      }

      digitalWrite(LED_PIN, LOW);

    } else if (cmd.startsWith("STATUS:")) {
      String status = cmd.substring(7);
      SerialMon.println("State change: " + status);
      systemArmed = (status == "ARMED");
      sendStatusNotification(status);
    }
  }

  // 5) Periodic background tasks
  if (millis() - lastGPSUpdate > GPS_UPDATE_MS) {
    updateGPS();
    lastGPSUpdate = millis();
  }
  if (millis() - lastHeartbeat > HEARTBEAT_MS) {
    sendHeartbeat();
    lastHeartbeat = millis();
  }
  if (millis() - lastPoll > POLL_INTERVAL_MS) {
    pollCommands();
    lastPoll = millis();
  }
}

// ── Receive Image from Main ESP32 ───────────────────────────
bool receiveImage() {
  imgSize = 0;
  unsigned long timeout = millis() + 30000;

  while (millis() < timeout) {
    if (Serial2.available()) {
      String line = Serial2.readStringUntil('\n'); line.trim();

      if (line == "NOIMG") { SerialMon.println("  No photo - text only"); return false; }

      if (line.startsWith("IMG:")) {
        size_t expected = line.substring(4).toInt();
        SerialMon.println("  Receiving " + String(expected) + " bytes...");

        if (expected == 0 || expected > IMG_BUF_SIZE || !imgBuffer) return false;

        unsigned long bt = millis() + 15000;
        while (imgSize < expected && millis() < bt) {
          size_t avail = Serial2.available();
          if (avail > 0) {
            size_t toRead = expected - imgSize;
            if (avail < toRead) toRead = avail;
            Serial2.readBytes(imgBuffer + imgSize, toRead);
            imgSize += toRead;
          }
        }

        // Consume IMG_END
        delay(100);
        while (Serial2.available()) Serial2.readStringUntil('\n');

        SerialMon.println("  Received " + String(imgSize) + "/" + String(expected) + " bytes");
        return (imgSize == expected);
      }
    }
  }
  SerialMon.println("  Photo receive timeout");
  return false;
}

// ── Send with Image (two notifications) ─────────────────────
bool sendWithImage(String reason) {
  if (gpsLat.length() == 0) updateGPS();

  bool isRequested = (reason == "Photo Requested");
  SerialMon.println("  [1/2] Text alert");

  getBatteryLevel();
  String timestamp = getModemTime();
  String body = isRequested ? "**Requested photo incoming.**" : ("**ALERT:** " + reason);
  if (timestamp.length() > 0) body += "\nTime: " + timestamp;
  if (gpsLat.length() > 0) {
    body += "\n\nLocation: " + gpsLat + "," + gpsLon;
    body += "\n\n[View Location on Google Maps](" + gpsMapsLink + ")";
  }
  body += "\n\nPhoto captured - see next notification.";
  body += getBatteryString();

  String hdrs = "Title: " + String(isRequested ? "Requested Photo" : "Anti-Theft ALERT");
  hdrs += "\r\nPriority: " + String(isRequested ? "default" : "urgent");
  hdrs += "\r\nTags: " + String(isRequested ? "camera" : "rotating_light");
  hdrs += "\r\nMarkdown: yes";
  if (gpsMapsLink.length() > 0) hdrs += "\r\nClick: " + gpsMapsLink;

  if (!sendHttpText(String(NTFY_TOPIC), hdrs, body)) return false;

  // Wait for 18650 voltage recovery before image upload (high-current burst)
  SerialMon.println("  Battery recovery delay");
  delay(5000);

  SerialMon.println("  [2/2] Photo upload");
  String imgHdrs = "Title: " + String(isRequested ? "Requested Photo" : "Photo Evidence");
  imgHdrs += "\r\nFilename: alert.jpg";

  return sendHttpBinary(String(NTFY_TOPIC), imgHdrs, imgBuffer, imgSize);
}

// ── Text-Only Notification (fallback) ────────────────────────
bool sendTextOnly(String reason) {
  if (gpsLat.length() == 0) updateGPS();
  bool isRequested = (reason == "Photo Requested");

  getBatteryLevel();
  String timestamp = getModemTime();
  String body = isRequested ? "**Requested photo** (no image available)."
                            : ("**ALERT:** " + reason);
  if (timestamp.length() > 0) body += "\nTime: " + timestamp;
  if (gpsLat.length() > 0)
    body += "\n\nLocation: " + gpsLat + "," + gpsLon
          + "\n\n[View Location on Google Maps](" + gpsMapsLink + ")";
  body += getBatteryString();

  String hdrs = "Title: " + String(isRequested ? "Requested Photo" : "Anti-Theft ALERT");
  hdrs += "\r\nPriority: " + String(isRequested ? "default" : "urgent");
  hdrs += "\r\nTags: " + String(isRequested ? "camera" : "rotating_light");
  hdrs += "\r\nMarkdown: yes";
  if (gpsMapsLink.length() > 0) hdrs += "\r\nClick: " + gpsMapsLink;

  return sendHttpText(String(NTFY_TOPIC), hdrs, body);
}

// ── Status Notification ─────────────────────────────────────
bool sendStatusNotification(String status) {
  // Don't force a GPS update here — periodic loop has fresh data
  getBatteryLevel();
  String body = "System " + status;
  if (gpsLat.length() > 0) body += "\nLocation: " + gpsLat + "," + gpsLon;
  body += getBatteryString();

  String hdrs = "Title: System " + status;
  hdrs += "\r\nPriority: low";
  hdrs += "\r\nTags: " + String(status == "ARMED" ? "lock" : "unlock");

  return sendHttpText(String(NTFY_TOPIC), hdrs, body);
}

// ── Heartbeat ───────────────────────────────────────────────
bool sendHeartbeat() {
  // Don't force a GPS update — periodic loop has fresh data
  getBatteryLevel();
  String timestamp = getModemTime();
  String body = "System online and monitoring.";
  if (timestamp.length() > 0) body += "\nTime: " + timestamp;
  if (gpsLat.length() > 0)    body += "\nLocation: " + gpsLat + "," + gpsLon;
  body += getBatteryString();

  String hdrs = "Title: Heartbeat - System OK\r\nPriority: min\r\nTags: green_circle";
  bool ok = sendHttpText(String(NTFY_TOPIC), hdrs, body);
  SerialMon.println(ok ? "Heartbeat sent" : "Heartbeat failed");
  return ok;
}

// ── Command Polling (ntfy dashboard -> system) ───────────────
void pollCommands() {
  if (!networkReady) return;

  String resp = sendHttpGet(String(CMD_TOPIC) + "/json?poll=1&since=" + lastPollId);
  if (resp.length() == 0) return;

  int searchFrom = 0;
  while (true) {
    int evtIdx = resp.indexOf("\"event\":\"message\"", searchFrom);
    if (evtIdx < 0) break;

    int msgIdx = resp.indexOf("\"message\":\"", evtIdx);
    if (msgIdx < 0) break;
    int msgStart = msgIdx + 11;
    int msgEnd = resp.indexOf("\"", msgStart);
    if (msgEnd < 0) break;
    String message = resp.substring(msgStart, msgEnd);

    int idIdx = resp.indexOf("\"id\":\"", evtIdx);
    if (idIdx >= 0) {
      int idStart = idIdx + 6;
      int idEnd = resp.indexOf("\"", idStart);
      if (idEnd > idStart) lastPollId = resp.substring(idStart, idEnd);
    }

    message.trim();
    message.toUpperCase();
    SerialMon.println("[POLL] Got command: " + message + " (id: " + lastPollId + ")");

    if (message == "ARM") {
      Serial2.println("REMOTE_ARM");
      SerialMon.println("[POLL] Forwarded REMOTE_ARM to Main ESP32");
      sendCommandAck("ARM command sent");
    } else if (message == "DISARM") {
      Serial2.println("REMOTE_DISARM");
      SerialMon.println("[POLL] Forwarded REMOTE_DISARM to Main ESP32");
      sendCommandAck("DISARM command sent");
    } else if (message == "GPS") {
      updateGPS();
      if (gpsLat.length() > 0) {
        sendGPSResponse("Location: " + gpsLat + "," + gpsLon
                      + "\n\n[View on Google Maps](" + gpsMapsLink + ")");
      } else {
        sendCommandAck("GPS fix not available");
      }
    } else if (message == "PHOTO") {
      sendCommandAck("Photo request sent");
      Serial2.println("REQUEST_PHOTO");
    }

    searchFrom = msgEnd + 1;
  }
}

bool sendCommandAck(String msg) {
  String hdrs = "Title: Command Acknowledged\r\nPriority: low\r\nTags: white_check_mark";
  return sendHttpText(String(NTFY_TOPIC), hdrs, msg);
}

bool sendGPSResponse(String body) {
  String hdrs = "Title: GPS Location\r\nPriority: low\r\nTags: round_pushpin\r\nMarkdown: yes";
  if (gpsMapsLink.length() > 0) hdrs += "\r\nClick: " + gpsMapsLink;
  return sendHttpText(String(NTFY_TOPIC), hdrs, body);
}

// ── GPS ──────────────────────────────────────────────────────
void updateGPS() {
  String r = sendAT("AT+CGPSINFO","OK",2000);
  if (r.indexOf("+CGPSINFO:") >= 0) {
    int i = r.indexOf("+CGPSINFO:"); String info = r.substring(i+11); info.trim();
    if (info.length() > 10 && info.charAt(0) != ',') {
      int c1=info.indexOf(','),
          c2=info.indexOf(',',c1+1),
          c3=info.indexOf(',',c2+1),
          c4=info.indexOf(',',c3+1);
      if (c4 > 0) {
        float lat = info.substring(0,2).toFloat() + info.substring(2,c1).toFloat()/60.0;
        if (info.substring(c1+1,c2)=="S") lat = -lat;
        float lon = info.substring(c2+1,c2+4).toFloat() + info.substring(c2+4,c3).toFloat()/60.0;
        if (info.substring(c3+1,c4)=="W") lon = -lon;
        gpsLat = String(lat,6);
        gpsLon = String(lon,6);
        gpsMapsLink = "https://maps.google.com/?q=" + gpsLat + "," + gpsLon;
      }
    }
  }
  lastGPSUpdate = millis();
}

// ─────────────────────────────────────────────────────────────
// SMS — outbound
// ─────────────────────────────────────────────────────────────

// Send an SMS via AT+CMGS in text mode. Returns true on +CMGS confirmation.
// Enforces SMS_MAX_PER_HOUR rolling rate limit. Body is truncated to 320
// chars (two concatenated SMS worth) as a sanity cap.
bool sendSMS(String to, String body) {
  // ── Rate limit ──
  unsigned long now = millis();
  if (smsWindowStart == 0 || now - smsWindowStart > 3600000UL) {
    smsCount = 0;
    smsWindowStart = now;
  }
  if (smsCount >= SMS_MAX_PER_HOUR) {
    SerialMon.println("[SMS] Rate limited (" + String(smsCount) + "/" + String(SMS_MAX_PER_HOUR) + " this hour)");
    return false;
  }

  // Sanity cap on length
  if (body.length() > 320) body = body.substring(0, 320);

  SerialMon.println("[SMS] Sending to " + to + " (" + String(body.length()) + " chars)");

  // Make sure modem is in text mode (cheap to re-set; some firmware drops it on CFUN cycle)
  sendAT("AT+CMGF=1", "OK", 1000);
  sendAT("AT+CSCS=\"GSM\"", "OK", 1000);

  // Drain any stale modem output before issuing CMGS
  while (SerialAT.available()) SerialAT.read();

  // Begin AT+CMGS — this will return a `>` prompt awaiting body
  SerialAT.print("AT+CMGS=\"");
  SerialAT.print(to);
  SerialAT.println("\"");

  unsigned long s = millis();
  bool gotPrompt = false;
  String pre = "";
  while (millis() - s < 5000) {
    while (SerialAT.available()) {
      char c = (char)SerialAT.read();
      pre += c;
      if (c == '>') { gotPrompt = true; break; }
    }
    if (gotPrompt) break;
    if (pre.indexOf("ERROR") >= 0) break;
    delay(5);
  }
  if (!gotPrompt) {
    SerialMon.println("[SMS] No > prompt: " + pre.substring(0, min((int)pre.length(), 200)));
    return false;
  }

  // Body. Print as raw bytes — no extra newline. Then Ctrl+Z to commit.
  SerialAT.print(body);
  SerialAT.write((uint8_t)26);   // Ctrl+Z

  // Wait for +CMGS response (up to 30 s — modem retries internally)
  s = millis();
  String r = "";
  while (millis() - s < 30000) {
    while (SerialAT.available()) r += (char)SerialAT.read();
    if (r.indexOf("+CMGS:") >= 0 && r.indexOf("OK") >= 0) {
      smsCount++;
      SerialMon.println("[SMS] Sent OK (" + String(smsCount) + "/" + String(SMS_MAX_PER_HOUR) + " this hour)");
      return true;
    }
    if (r.indexOf("+CMS ERROR") >= 0 || r.indexOf("+CME ERROR") >= 0) {
      SerialMon.println("[SMS] Modem reported error: " + r.substring(0, min((int)r.length(), 200)));
      return false;
    }
    delay(50);
  }
  SerialMon.println("[SMS] Timeout: " + r.substring(0, min((int)r.length(), 200)));
  return false;
}

// Compose and send the alarm SMS. Kept short so it fits in one SMS where
// possible (the GPS Maps URL can push it over 160 GSM-7 chars; the modem
// will then send a 2-part concatenated SMS, which is fine).
void sendAlertSMS(String reason) {
  if (reason == "Photo Requested") return;  // handled by smsPhotoRequested flow
  String body = "ALERT: " + reason;
  String ts = getModemTime();
  if (ts.length() > 0)            body += "\n" + ts;
  if (gpsMapsLink.length() > 0)   body += "\n" + gpsMapsLink;
  body += "\nPhoto: ntfy.sh/" + String(NTFY_TOPIC);
  sendSMS(OWNER_PHONE, body);
}

// ─────────────────────────────────────────────────────────────
// SMS — inbound
// ─────────────────────────────────────────────────────────────

// Scan SerialAT for unsolicited +CMTI: "ME",N URCs and process each one.
// This is the primary path for inbound SMS but is best-effort: a +CMTI
// can be eaten by a sendAT() that's running concurrently. checkUnreadSMS()
// is the safety net.
void checkSMSNotifications() {
  // Only look at lines that are clearly waiting (don't block)
  if (!SerialAT.available()) return;

  // Non-blocking peek: read up to 256 chars currently in buffer
  String buf = "";
  unsigned long s = millis();
  while (SerialAT.available() && millis() - s < 200) {
    buf += (char)SerialAT.read();
    if (buf.length() > 512) break;
  }
  if (buf.length() == 0) return;

  // Scan for +CMTI URCs in whatever we read
  int searchFrom = 0;
  while (true) {
    int cmtiIdx = buf.indexOf("+CMTI:", searchFrom);
    if (cmtiIdx < 0) break;
    int comma = buf.indexOf(',', cmtiIdx);
    int eol = buf.indexOf('\n', cmtiIdx);
    if (comma < 0 || eol < 0 || comma > eol) break;
    int idx = buf.substring(comma + 1, eol).toInt();
    if (idx > 0) {
      SerialMon.println("[SMS] +CMTI URC at index " + String(idx));
      readAndProcessSMS(idx);
    }
    searchFrom = eol + 1;
  }
  // Other content in `buf` (e.g. tail of an AT response) is intentionally
  // discarded — sendAT() polls SerialAT freshly each call and is robust to
  // missed bytes outside its window.
}

// Safety-net: list any unread messages and process them. Catches anything
// that arrived while we were busy (the +CMTI URC may have been swallowed
// by an in-flight AT command's read loop).
void checkUnreadSMS() {
  String r = sendAT("AT+CMGL=\"REC UNREAD\"", "OK", 5000);

  int searchFrom = 0;
  while (true) {
    int cmglIdx = r.indexOf("+CMGL:", searchFrom);
    if (cmglIdx < 0) break;

    // Extract index — first integer after "+CMGL:"
    int p = cmglIdx + 6;
    while (p < (int)r.length() && r[p] == ' ') p++;
    int commaIdx = r.indexOf(',', p);
    if (commaIdx < 0) break;

    int idx = r.substring(p, commaIdx).toInt();
    if (idx > 0) {
      SerialMon.println("[SMS] CMGL safety-net found unread at index " + String(idx));
      readAndProcessSMS(idx);
    }
    searchFrom = commaIdx;
  }
}

// Read SMS at given index, validate sender, dispatch to handler.
void readAndProcessSMS(int index) {
  String resp = sendAT("AT+CMGR=" + String(index), "OK", 5000);

  int cmgrIdx = resp.indexOf("+CMGR:");
  if (cmgrIdx < 0) {
    sendATWait("AT+CMGD=" + String(index), 3000);
    return;
  }

  // Extract sender: header is +CMGR: "STATUS","SENDER",...
  int q1 = resp.indexOf('"', cmgrIdx);     // open status
  int q2 = resp.indexOf('"', q1 + 1);      // close status
  int q3 = resp.indexOf('"', q2 + 1);      // open sender
  int q4 = resp.indexOf('"', q3 + 1);      // close sender
  if (q4 < 0) {
    sendATWait("AT+CMGD=" + String(index), 3000);
    return;
  }
  String sender = resp.substring(q3 + 1, q4);

  // Body: text between header end and trailing OK
  int headerEnd = resp.indexOf('\n', q4);
  int okIdx = resp.lastIndexOf("\r\nOK");
  if (okIdx < 0) okIdx = resp.lastIndexOf("OK");
  String body = "";
  if (headerEnd >= 0 && okIdx > headerEnd) {
    body = resp.substring(headerEnd + 1, okIdx);
    body.trim();
  }

  // Always delete from storage (whether we accept or reject)
  sendATWait("AT+CMGD=" + String(index), 3000);

  SerialMon.println("[SMS] from: " + sender);
  SerialMon.println("[SMS] body: " + body);

  sender.trim();
  if (!isAuthorizedSender(sender)) {
    SerialMon.println("[SMS] rejected: unauthorized sender");
    return;
  }

  body.toUpperCase();
  handleSMSCommand(body);
}

bool isAuthorizedSender(String number) {
  number.trim();
  String owner = OWNER_PHONE;
  // Compare last 10 digits to handle +1 vs 1 vs raw
  if (number.length() >= 10 && owner.length() >= 10) {
    return number.substring(number.length() - 10) == owner.substring(owner.length() - 10);
  }
  return number == owner;
}

void handleSMSCommand(String cmd) {
  cmd.trim();
  SerialMon.println("[SMS] CMD: " + cmd);

  if (cmd == "ARM") {
    Serial2.println("SMS_CMD:ARM");
    String reply = waitForMainReply(5000);
    sendSMS(OWNER_PHONE, reply.length() > 0 ? reply
                                            : "ARM command sent (no confirmation from Main)");
  } else if (cmd == "DISARM") {
    Serial2.println("SMS_CMD:DISARM");
    String reply = waitForMainReply(5000);
    sendSMS(OWNER_PHONE, reply.length() > 0 ? reply
                                            : "DISARM command sent (no confirmation from Main)");
  } else if (cmd == "STATUS") {
    Serial2.println("SMS_CMD:STATUS");
    String reply = waitForMainReply(5000);
    getBatteryLevel();
    if (gpsLat.length() == 0) updateGPS();
    String smsBody = (reply.length() > 0) ? reply : "Status unknown";
    smsBody += getBatteryString();
    if (gpsMapsLink.length() > 0) smsBody += "\n" + gpsMapsLink;
    sendSMS(OWNER_PHONE, smsBody);
  } else if (cmd == "PHOTO") {
    smsPhotoRequested = true;
    Serial2.println("SMS_CMD:PHOTO");
    sendSMS(OWNER_PHONE, "Photo requested. Will reply with link when ready.");
  } else if (cmd == "GPS") {
    updateGPS();
    if (gpsMapsLink.length() > 0) {
      sendSMS(OWNER_PHONE, "Location:\n" + gpsMapsLink);
    } else {
      sendSMS(OWNER_PHONE, "GPS fix not available");
    }
  } else if (cmd == "HELP") {
    sendSMS(OWNER_PHONE, "Commands: ARM, DISARM, STATUS, PHOTO, GPS, HELP");
  } else {
    SerialMon.println("[SMS] Unknown command: " + cmd);
  }
}

String waitForMainReply(unsigned long timeout) {
  unsigned long start = millis();
  while (millis() - start < timeout) {
    if (Serial2.available()) {
      String line = Serial2.readStringUntil('\n');
      line.trim();
      if (line.startsWith("SMS_REPLY:")) return line.substring(10);
      if (line.length() > 0) SerialMon.println("Main (while waiting): " + line);
    }
    delay(10);
  }
  return "";
}
