/*
 * ANTI-THEFT SYSTEM — LILYGO T-SIM7600G-H
 * Cellular gateway: receives alert + photo from Main ESP32, uploads to ntfy.sh.
 * Board: ESP32 Dev Module
 *
 * Serial  (UART0) = USB debug
 * Serial1 (UART1) = SIM7600 modem (GPIO26 RX / GPIO27 TX)
 * Serial2 (UART2) = Main ESP32    (GPIO21 RX / GPIO19 TX)
 *
 * Wiring: Main GPIO27 TX -> LILYGO GPIO21 RX
 *         Main GPIO26 RX <- LILYGO GPIO19 TX
 *         GND <-> GND
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

const char APN[]        = "hologram";
const char NTFY_TOPIC[] = "antitheft-gonnie-2219";
const char CMD_TOPIC[]  = "antitheft-gonnie-2219-cmd";
const char NTFY_IP[]    = "159.203.148.75";

#define IMG_BUF_SIZE 51200
uint8_t* imgBuffer = NULL;
size_t imgSize = 0;

bool networkReady = false;
String gpsLat = "", gpsLon = "", gpsMapsLink = "";
unsigned long lastGPSUpdate = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastPoll = 0;
String lastPollId = "0";
String batteryPercent = "";
String batteryVoltage = "";
#define HEARTBEAT_MS 21600000  // 6 hours
#define POLL_INTERVAL_MS 5000

// ── AT Helpers ───────────────────────────────────────────────
String sendAT(String cmd, String exp, unsigned long t) {
  SerialAT.println(cmd);
  unsigned long s = millis(); String r = "";
  while (millis()-s < t) {
    while (SerialAT.available()) r += (char)SerialAT.read();
    if (r.indexOf(exp) >= 0) break; delay(10);
  }
  return r;
}

String sendATWait(String cmd, unsigned long t) {
  SerialAT.println(cmd);
  unsigned long s = millis(); String r = "";
  while (millis()-s < t) {
    while (SerialAT.available()) r += (char)SerialAT.read();
    if (r.indexOf("OK") >= 0 || r.indexOf("ERROR") >= 0) break; delay(10);
  }
  return r;
}

// ── Network Recovery ─────────────────────────────────────────
void ensureNetwork() {
  String r = sendAT("AT+CREG?", "OK", 2000);
  if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) return;  // registered

  SerialMon.println("Network lost - re-registering...");
  networkReady = false;
  sendATWait("AT+CGACT=1,1", 10000);
  sendATWait("AT+NETOPEN", 15000);
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
void getBatteryLevel() {
  String r = sendAT("AT+CBC", "OK", 2000);
  int i = r.indexOf("+CBC:");
  if (i >= 0) {
    // Format: +CBC: <bcs>,<bcl>,<voltage>
    int c1 = r.indexOf(',', i);
    int c2 = r.indexOf(',', c1 + 1);
    if (c1 > 0 && c2 > c1) {
      batteryPercent = r.substring(c1 + 1, c2);
      batteryPercent.trim();
      String mv = r.substring(c2 + 1);
      // Trim to just digits (remove OK, newlines, etc)
      String clean = "";
      for (int j = 0; j < mv.length(); j++) {
        if (isDigit(mv.charAt(j))) clean += mv.charAt(j);
        else if (clean.length() > 0) break;
      }
      if (clean.length() > 0) {
        float volts = clean.toFloat() / 1000.0;
        batteryVoltage = String(volts, 1);
      }
    }
  }
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
  SerialMon.println("====================================\n");

  imgBuffer = (uint8_t*)malloc(IMG_BUF_SIZE);
  SerialMon.println(imgBuffer ? "Image buffer allocated (50 KB)" : "Image buffer allocation FAILED");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  Serial2.setRxBufferSize(1024);
  Serial2.begin(115200, SERIAL_8N1, MAIN_RX, MAIN_TX);
  SerialMon.println("UART2  ->  Main ESP32");

  SerialMon.println("Powering on modem...");
  pinMode(MODEM_FLIGHT, OUTPUT); digitalWrite(MODEM_FLIGHT, HIGH);
  pinMode(MODEM_DTR, OUTPUT); digitalWrite(MODEM_DTR, LOW);
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
  if (!ok) { SerialMon.println("Modem not responding"); Serial2.println("LILYGO_ERROR"); return; }
  SerialMon.println("Modem online");

  sendAT("ATE0","OK",1000);
  sendAT("AT+CGDCONT=1,\"IP\",\"" + String(APN) + "\"","OK",3000);
  SerialMon.print("  Registering");
  for (int i = 0; i < 15; i++) {
    String r = sendAT("AT+CREG?","OK",2000);
    if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) {
      SerialMon.println(" registered");
      sendATWait("AT+CGACT=1,1", 10000);
      sendATWait("AT+NETOPEN", 15000);
      delay(2000); while (SerialAT.available()) SerialAT.read();
      networkReady = true;
      SerialMon.println("  Network ready");
      break;
    }
    SerialMon.print("."); delay(2000);
  }

  sendATWait("AT+CGPS=1", 3000);

  Serial2.println("LILYGO_READY");
  SerialMon.println("\nSystem ready - awaiting alerts\n");
  digitalWrite(LED_PIN, LOW);

  // Send startup notification with battery level
  getBatteryLevel();
  updateGPS();
  String startBody = "System powered on and ready.";
  String ts = getModemTime();
  if (ts.length() > 0) startBody += "\nTime: " + ts;
  if (gpsLat.length() > 0) startBody += "\nLocation: " + gpsLat + "," + gpsLon;
  startBody += getBatteryString();

  String startReq = "POST /" + String(NTFY_TOPIC) + " HTTP/1.1\r\n";
  startReq += "Host: ntfy.sh\r\nTitle: System Startup\r\n";
  startReq += "Priority: low\r\nTags: rocket\r\n";
  startReq += "Content-Type: text/plain\r\n";
  startReq += "Content-Length: " + String(startBody.length()) + "\r\n";
  startReq += "Connection: close\r\n\r\n" + startBody;

  if (tcpConnect()) {
    if (cipSend(startReq)) waitForResponse();
    delay(500);
    sendATWait("AT+CIPCLOSE=0", 5000);
  }
  lastHeartbeat = millis();
}

// ── Main Loop ────────────────────────────────────────────────
void loop() {
  if (Serial2.available()) {
    String cmd = Serial2.readStringUntil('\n'); cmd.trim();

    if (cmd.startsWith("ALERT:")) {
      String reason = cmd.substring(6);
      SerialMon.println("*** ALERT: " + reason + " ***");
      digitalWrite(LED_PIN, HIGH);

      // Wait for image data or NOIMG from Main ESP32
      bool hasImage = receiveImage();

      updateGPS();

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
      digitalWrite(LED_PIN, LOW);

    } else if (cmd.startsWith("STATUS:")) {
      String status = cmd.substring(7);
      SerialMon.println("State change: " + status);
      sendStatusNotification(status);
    }
  }

  if (millis() - lastGPSUpdate > 30000) { updateGPS(); lastGPSUpdate = millis(); }
  if (millis() - lastHeartbeat > HEARTBEAT_MS) { sendHeartbeat(); lastHeartbeat = millis(); }
  if (millis() - lastPoll > POLL_INTERVAL_MS) { pollCommands(); lastPoll = millis(); }
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

  // === NOTIFICATION 1: Text alert with clickable Maps link ===
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

  String req = "POST /" + String(NTFY_TOPIC) + " HTTP/1.1\r\n";
  req += "Host: ntfy.sh\r\n";
  req += "Title: " + String(isRequested ? "Requested Photo" : "Anti-Theft ALERT") + "\r\n";
  req += "Priority: " + String(isRequested ? "default" : "urgent") + "\r\n";
  req += "Tags: " + String(isRequested ? "camera" : "rotating_light") + "\r\n";
  req += "Markdown: yes\r\n";
  if (gpsMapsLink.length() > 0) req += "Click: " + gpsMapsLink + "\r\n";
  req += "Content-Length: " + String(body.length()) + "\r\n";
  req += "Connection: close\r\n\r\n" + body;

  // Connect and send text notification
  if (!tcpConnect()) return false;
  if (!cipSend(req)) { sendATWait("AT+CIPCLOSE=0",5000); return false; }
  waitForResponse();
  delay(1000);
  sendATWait("AT+CIPCLOSE=0", 5000);
  delay(500);
  while (SerialAT.available()) SerialAT.read();  // Drain remaining AT responses
  SerialMon.println("  Battery recovery delay");
  delay(5000);  // Wait for 18650 battery voltage recovery before image upload

  // === NOTIFICATION 2: Image-only with minimal headers ===
  SerialMon.println("  [2/2] Photo upload");

  // Keep headers minimal - let ntfy auto-detect the image
  String hdr = "PUT /" + String(NTFY_TOPIC) + " HTTP/1.1\r\n";
  hdr += "Host: ntfy.sh\r\n";
  hdr += "Title: " + String(isRequested ? "Requested Photo" : "Photo Evidence") + "\r\n";
  hdr += "Filename: alert.jpg\r\n";
  hdr += "Content-Length: " + String(imgSize) + "\r\n";
  hdr += "Connection: close\r\n\r\n";

  // Connect
  if (!tcpConnect()) return false;

  // Send headers
  if (!cipSend(hdr)) { sendATWait("AT+CIPCLOSE=0",5000); return false; }

  // Send image in 1KB chunks
  SerialMon.println("  Uploading " + String(imgSize) + " bytes");
  size_t sent = 0;
  while (sent < imgSize) {
    size_t chunk = imgSize - sent;
    if (chunk > 1024) chunk = 1024;

    SerialAT.println("AT+CIPSEND=0," + String(chunk));
    unsigned long s = millis(); String r = "";
    while (millis()-s < 5000) {
      while (SerialAT.available()) r += (char)SerialAT.read();
      if (r.indexOf(">") >= 0) break;
      if (r.indexOf("ERROR") >= 0) { sendATWait("AT+CIPCLOSE=0",5000); return false; }
      delay(10);
    }
    if (r.indexOf(">") < 0) { sendATWait("AT+CIPCLOSE=0",5000); return false; }

    SerialAT.write(imgBuffer + sent, chunk);
    sent += chunk;

    s = millis(); r = "";
    while (millis()-s < 5000) {
      while (SerialAT.available()) r += (char)SerialAT.read();
      if (r.indexOf("+CIPSEND:") >= 0 || r.indexOf("ERROR") >= 0) break;
      delay(10);
    }

    if (sent % 4096 < 1024 || sent >= imgSize)
      SerialMon.println("  " + String(sent) + "/" + String(imgSize));
  }

  // Wait for response
  bool ok = waitForResponse();
  if (!ok && sent >= imgSize) ok = true;
  delay(500);
  sendATWait("AT+CIPCLOSE=0", 5000);

  return ok;
}

// Helper: open TCP connection to ntfy.sh
bool tcpConnect() {
  while (SerialAT.available()) SerialAT.read();  // Drain stale AT buffer
  ensureNetwork();
  if (!networkReady) return false;
  SerialMon.println("  Connecting to ntfy.sh");
  SerialAT.println("AT+CIPOPEN=0,\"TCP\",\"" + String(NTFY_IP) + "\",80");
  unsigned long s = millis(); String r = "";
  while (millis()-s < 15000) {
    while (SerialAT.available()) r += (char)SerialAT.read();
    if (r.indexOf("+CIPOPEN: 0,0") >= 0) return true;
    if (r.indexOf("+CIPOPEN: 0,") >= 0 && r.indexOf("+CIPOPEN: 0,0") < 0) break;
    delay(10);
  }
  SerialMon.println("  Connect failed");
  return false;
}

// Helper: wait for HTTP response
bool waitForResponse() {
  unsigned long s = millis(); String r = "";
  while (millis()-s < 20000) {
    while (SerialAT.available()) r += (char)SerialAT.read();
    if (r.indexOf("200 OK") >= 0) {
      SerialMon.println("  HTTP OK");
      return true;
    }
    if (r.indexOf("HTTP/") >= 0 && r.indexOf(" 200") >= 0) {
      SerialMon.println("  HTTP OK");
      return true;
    }
    if (r.indexOf("+IPCLOSE") >= 0 || r.indexOf("+CIPCLOSE") >= 0) break;
    delay(100);
  }
  // Drain remaining response data for 2 more seconds
  unsigned long drain = millis();
  while (millis() - drain < 2000) {
    while (SerialAT.available()) r += (char)SerialAT.read();
    delay(50);
  }
  SerialMon.println("  HTTP failed or timed out");
  return false;
}

// Send string data via CIPSEND
bool cipSend(String data) {
  SerialAT.println("AT+CIPSEND=0," + String(data.length()));
  unsigned long s = millis(); String r = "";
  while (millis()-s < 5000) {
    while (SerialAT.available()) r += (char)SerialAT.read();
    if (r.indexOf(">") >= 0) break;
    if (r.indexOf("ERROR") >= 0) return false;
    delay(10);
  }
  if (r.indexOf(">") < 0) return false;
  SerialAT.print(data);
  s = millis(); r = "";
  while (millis()-s < 5000) {
    while (SerialAT.available()) r += (char)SerialAT.read();
    if (r.indexOf("+CIPSEND:") >= 0) return true;
    if (r.indexOf("ERROR") >= 0) return false;
    delay(10);
  }
  return true;
}

// ── Text-Only Notification (fallback) ────────────────────────
bool sendTextOnly(String reason) {
  if (gpsLat.length() == 0) updateGPS();
  bool isRequested = (reason == "Photo Requested");

  getBatteryLevel();
  String timestamp = getModemTime();
  String body = isRequested ? "**Requested photo** (no image available)." : ("**ALERT:** " + reason);
  if (timestamp.length() > 0) body += "\nTime: " + timestamp;
  if (gpsLat.length() > 0) body += "\n\nLocation: " + gpsLat + "," + gpsLon + "\n\n[View Location on Google Maps](" + gpsMapsLink + ")";
  body += getBatteryString();

  String req = "POST /" + String(NTFY_TOPIC) + " HTTP/1.1\r\n";
  req += "Host: ntfy.sh\r\n";
  req += "Title: " + String(isRequested ? "Requested Photo" : "Anti-Theft ALERT") + "\r\n";
  req += "Priority: " + String(isRequested ? "default" : "urgent") + "\r\n";
  req += "Tags: " + String(isRequested ? "camera" : "rotating_light") + "\r\n";
  req += "Markdown: yes\r\n";
  if (gpsMapsLink.length() > 0) req += "Click: " + gpsMapsLink + "\r\n";
  req += "Content-Type: text/markdown\r\nContent-Length: " + String(body.length()) + "\r\nConnection: close\r\n\r\n" + body;

  if (!tcpConnect()) return false;
  if (!cipSend(req)) { sendATWait("AT+CIPCLOSE=0",5000); return false; }
  bool ok = waitForResponse();
  delay(500);
  sendATWait("AT+CIPCLOSE=0", 5000);

  return ok;
}

// ── Status Notification ─────────────────────────────────────
bool sendStatusNotification(String status) {
  updateGPS();

  getBatteryLevel();
  String body = "System " + status;
  if (gpsLat.length() > 0) body += "\nLocation: " + gpsLat + "," + gpsLon;
  body += getBatteryString();

  String req = "POST /" + String(NTFY_TOPIC) + " HTTP/1.1\r\n";
  req += "Host: ntfy.sh\r\nTitle: System " + status + "\r\n";
  req += "Priority: low\r\n";
  req += "Tags: " + String(status == "ARMED" ? "lock" : "unlock") + "\r\n";
  req += "Content-Type: text/plain\r\n";
  req += "Content-Length: " + String(body.length()) + "\r\n";
  req += "Connection: close\r\n\r\n" + body;

  bool ntfyOk = false;
  if (tcpConnect()) {
    if (cipSend(req)) ntfyOk = waitForResponse();
    delay(500);
    sendATWait("AT+CIPCLOSE=0", 5000);
  }

  return ntfyOk;
}

// ── Heartbeat ───────────────────────────────────────────────
bool sendHeartbeat() {
  updateGPS();
  getBatteryLevel();
  String timestamp = getModemTime();
  String body = "System online and monitoring.";
  if (timestamp.length() > 0) body += "\nTime: " + timestamp;
  if (gpsLat.length() > 0) body += "\nLocation: " + gpsLat + "," + gpsLon;
  body += getBatteryString();

  String req = "POST /" + String(NTFY_TOPIC) + " HTTP/1.1\r\n";
  req += "Host: ntfy.sh\r\nTitle: Heartbeat - System OK\r\n";
  req += "Priority: min\r\n";
  req += "Tags: green_circle\r\n";
  req += "Content-Type: text/plain\r\n";
  req += "Content-Length: " + String(body.length()) + "\r\n";
  req += "Connection: close\r\n\r\n" + body;

  if (!tcpConnect()) return false;
  if (!cipSend(req)) { sendATWait("AT+CIPCLOSE=0",5000); return false; }
  bool ok = waitForResponse();
  delay(500);
  sendATWait("AT+CIPCLOSE=0", 5000);
  SerialMon.println(ok ? "Heartbeat sent" : "Heartbeat failed");
  return ok;
}

// ── Command Polling ──────────────────────────────────────────
void pollCommands() {
  if (!networkReady) return;
  if (!tcpConnect()) return;

  String req = "GET /" + String(CMD_TOPIC) + "/json?poll=1&since=" + lastPollId + " HTTP/1.1\r\n";
  req += "Host: ntfy.sh\r\nConnection: close\r\n\r\n";

  if (!cipSend(req)) { sendATWait("AT+CIPCLOSE=0", 5000); return; }

  // Read full response (wait for close or 10s timeout)
  unsigned long s = millis(); String resp = "";
  while (millis() - s < 10000) {
    while (SerialAT.available()) resp += (char)SerialAT.read();
    if (resp.indexOf("+IPCLOSE") >= 0 || resp.indexOf("+CIPCLOSE") >= 0) break;
    delay(50);
  }
  sendATWait("AT+CIPCLOSE=0", 5000);

  // Parse JSON lines — look for "event":"message" lines
  int searchFrom = 0;
  while (true) {
    int evtIdx = resp.indexOf("\"event\":\"message\"", searchFrom);
    if (evtIdx < 0) break;

    // Find message field near this event
    int msgIdx = resp.indexOf("\"message\":\"", evtIdx);
    if (msgIdx < 0) break;
    int msgStart = msgIdx + 11;
    int msgEnd = resp.indexOf("\"", msgStart);
    if (msgEnd < 0) break;
    String message = resp.substring(msgStart, msgEnd);

    // Find id field near this event
    int idIdx = resp.indexOf("\"id\":\"", evtIdx);
    if (idIdx >= 0) {
      int idStart = idIdx + 6;
      int idEnd = resp.indexOf("\"", idStart);
      if (idEnd > idStart) lastPollId = resp.substring(idStart, idEnd);
    }

    message.trim();
    message.toUpperCase();
    SerialMon.println("CMD: " + message);

    if (message == "ARM") {
      Serial2.println("REMOTE_ARM");
      sendCommandAck("ARM command sent");
    } else if (message == "DISARM") {
      Serial2.println("REMOTE_DISARM");
      sendCommandAck("DISARM command sent");
    } else if (message == "GPS") {
      updateGPS();
      if (gpsLat.length() > 0) {
        sendGPSResponse("Location: " + gpsLat + "," + gpsLon + "\n\n[View on Google Maps](" + gpsMapsLink + ")");
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

// ── Command Acknowledgement ─────────────────────────────────
bool sendCommandAck(String msg) {
  String req = "POST /" + String(NTFY_TOPIC) + " HTTP/1.1\r\n";
  req += "Host: ntfy.sh\r\n";
  req += "Title: Command Acknowledged\r\n";
  req += "Priority: low\r\n";
  req += "Tags: white_check_mark\r\n";
  req += "Content-Length: " + String(msg.length()) + "\r\n";
  req += "Connection: close\r\n\r\n" + msg;

  if (!tcpConnect()) return false;
  if (!cipSend(req)) { sendATWait("AT+CIPCLOSE=0", 5000); return false; }
  bool ok = waitForResponse();
  delay(500);
  sendATWait("AT+CIPCLOSE=0", 5000);
  return ok;
}

// ── GPS Response ─────────────────────────────────────────────
bool sendGPSResponse(String body) {
  String req = "POST /" + String(NTFY_TOPIC) + " HTTP/1.1\r\n";
  req += "Host: ntfy.sh\r\n";
  req += "Title: GPS Location\r\n";
  req += "Priority: low\r\n";
  req += "Tags: round_pushpin\r\n";
  req += "Markdown: yes\r\n";
  if (gpsMapsLink.length() > 0) req += "Click: " + gpsMapsLink + "\r\n";
  req += "Content-Length: " + String(body.length()) + "\r\n";
  req += "Connection: close\r\n\r\n" + body;

  if (!tcpConnect()) return false;
  if (!cipSend(req)) { sendATWait("AT+CIPCLOSE=0", 5000); return false; }
  bool ok = waitForResponse();
  delay(500);
  sendATWait("AT+CIPCLOSE=0", 5000);
  return ok;
}

// ── GPS ──────────────────────────────────────────────────────
void updateGPS() {
  String r = sendAT("AT+CGPSINFO","OK",2000);
  if (r.indexOf("+CGPSINFO:") >= 0) {
    int i = r.indexOf("+CGPSINFO:"); String info = r.substring(i+11); info.trim();
    if (info.length() > 10 && info.charAt(0) != ',') {
      int c1=info.indexOf(','),c2=info.indexOf(',',c1+1),c3=info.indexOf(',',c2+1),c4=info.indexOf(',',c3+1);
      if (c4 > 0) {
        float lat = info.substring(0,2).toFloat()+info.substring(2,c1).toFloat()/60.0;
        if (info.substring(c1+1,c2)=="S") lat=-lat;
        float lon = info.substring(c2+1,c2+4).toFloat()+info.substring(c2+4,c3).toFloat()/60.0;
        if (info.substring(c3+1,c4)=="W") lon=-lon;
        gpsLat=String(lat,6); gpsLon=String(lon,6);
        gpsMapsLink="https://maps.google.com/?q="+gpsLat+","+gpsLon;
      }
    }
  }
  lastGPSUpdate = millis();
}
