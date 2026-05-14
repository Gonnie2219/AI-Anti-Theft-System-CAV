/*
 * SpeedTalk Full System Diagnostic (SMS out + SMS in + Data + HTTP)
 *
 * One-flash standalone test for the LILYGO T-SIM7600G-H with SpeedTalk
 * TopValue T3 SIM. Validates all four capabilities the anti-theft system
 * needs:  outbound SMS, inbound SMS, IPv4 PDP activation, HTTP POST.
 *
 * No involvement from Main ESP32 or ESP32-CAM.
 * Build target: ESP32 Dev Module
 */

#include <Arduino.h>

// ── Pins (verbatim from AntiTheftSystemLilygo.ino) ──────────
#define MODEM_TX        27
#define MODEM_RX        26
#define MODEM_PWRKEY     4
#define MODEM_DTR       32
#define MODEM_FLIGHT    25
#define MODEM_STATUS    34

// ── Aliases ──────────────────────────────────────────────────
#define SerialMon  Serial
#define SerialAT   Serial1

// ── Hardcoded test constants ─────────────────────────────────
const char* DEVICE_NUMBER = "+13132081968";   // SpeedTalk SIM number (log only)
const char* USER_PHONE    = "+16093589220";   // target for outbound SMS
const char* NTFY_TOPIC    = "antitheft-gonnie-2219";
const char* NTFY_IP       = "159.203.148.75";

// ── APN list ─────────────────────────────────────────────────
const char* apnList[] = { "Wholesale", "wholesale", "stkmobi", "fast.t-mobile.com" };
const int   apnCount  = 4;

// ── SMS inbound tracking ─────────────────────────────────────
int    smsReceivedCount = 0;
String lastSmsSender    = "";
String lastSmsBody      = "";

// ── Result tracking ──────────────────────────────────────────
String initialOperator  = "";
String resetOperator    = "";
bool   smsOutSuccess    = false;
String smsOutRef        = "";
bool   dataWorked       = false;
String winningApn       = "";
String winningIp        = "";
int    httpStatus       = -999;  // -999=skipped, -1=fail, 200=ok, etc.

// ─────────────────────────────────────────────────────────────
//  AT command helper (verbatim from AntiTheftSystemLilygo.ino)
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
//  Formatting helpers
// ─────────────────────────────────────────────────────────────
void ts() {
  SerialMon.print("[");
  SerialMon.print(millis());
  SerialMon.print(" ms] ");
}

void printSection(const char* title) {
  SerialMon.println();
  SerialMon.println("=============================================");
  ts(); SerialMon.println(title);
  SerialMon.println("=============================================");
}

void runAndPrint(const char* label, String cmd, unsigned long timeout) {
  String r = sendAT(cmd, "OK", timeout);
  r.trim();
  SerialMon.print("  ");
  SerialMon.print(label);
  SerialMon.print(": ");
  SerialMon.println(r);
}

// ─────────────────────────────────────────────────────────────
//  SMS URC scanner — call frequently during long waits
//
//  With AT+CNMI=2,2 the modem delivers inbound SMS as a URC:
//    +CMT: "<sender>","","<timestamp>"\r\n<body>\r\n
//  We scan whatever is in SerialAT and parse any +CMT: lines.
// ─────────────────────────────────────────────────────────────
void scanForSmsURC() {
  while (SerialAT.available()) {
    String line = SerialAT.readStringUntil('\n');
    line.trim();
    if (line.indexOf("+CMT:") >= 0) {
      // Parse sender from +CMT: "<sender>","","<timestamp>"
      int q1 = line.indexOf('"');
      int q2 = (q1 >= 0) ? line.indexOf('"', q1 + 1) : -1;
      String sender = (q1 >= 0 && q2 > q1) ? line.substring(q1 + 1, q2) : "unknown";

      // Next line is the body
      String body = "";
      unsigned long bodyWait = millis();
      while (millis() - bodyWait < 2000) {
        if (SerialAT.available()) {
          body = SerialAT.readStringUntil('\n');
          body.trim();
          break;
        }
        delay(10);
      }

      smsReceivedCount++;
      lastSmsSender = sender;
      lastSmsBody   = body;

      ts();
      SerialMon.print("INBOUND SMS #");
      SerialMon.print(smsReceivedCount);
      SerialMon.print(" from ");
      SerialMon.print(sender);
      SerialMon.print(": ");
      SerialMon.println(body);
    }
  }
}

// ─────────────────────────────────────────────────────────────
//  IPv4 / address helpers
// ─────────────────────────────────────────────────────────────
bool looksLikeIPv4(const String& s) {
  int dotCount = 0, digitRun = 0;
  for (int i = 0; i < (int)s.length(); i++) {
    char c = s.charAt(i);
    if (c >= '0' && c <= '9') {
      digitRun++;
    } else if (c == '.' && digitRun > 0) {
      dotCount++;
      digitRun = 0;
    } else {
      if (dotCount >= 3 && digitRun > 0) return true;
      dotCount = 0;
      digitRun = 0;
    }
  }
  return (dotCount >= 3 && digitRun > 0);
}

String extractAddr(const String& resp) {
  int idx = resp.indexOf("+CGPADDR:");
  if (idx < 0) return "";
  int q1 = resp.indexOf('"', idx);
  if (q1 < 0) {
    int comma = resp.indexOf(',', idx);
    if (comma < 0) return "";
    String tail = resp.substring(comma + 1);
    tail.trim();
    int okIdx = tail.indexOf("OK");
    if (okIdx > 0) tail = tail.substring(0, okIdx);
    tail.trim();
    return tail;
  }
  int q2 = resp.indexOf('"', q1 + 1);
  if (q2 < 0) return resp.substring(q1 + 1);
  return resp.substring(q1 + 1, q2);
}

// ─────────────────────────────────────────────────────────────
//  Wait for CREG registration with SMS URC scanning
// ─────────────────────────────────────────────────────────────
bool waitForRegistration(int maxSeconds) {
  ts(); SerialMon.print("Polling AT+CREG?");
  for (int i = 0; i < maxSeconds / 2; i++) {
    scanForSmsURC();
    String r = sendAT("AT+CREG?", "OK", 2000);
    if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) {
      SerialMon.println();
      ts(); SerialMon.println("Registered! Raw: " + r);
      return true;
    }
    SerialMon.print(".");
    delay(2000);
    scanForSmsURC();
  }
  SerialMon.println();
  ts(); SerialMon.println("WARNING: Not registered after " + String(maxSeconds) + "s");
  return false;
}

// ─────────────────────────────────────────────────────────────
//  Extract operator name from AT+COPS? response
// ─────────────────────────────────────────────────────────────
String extractOperator(const String& copsResp) {
  // Format: +COPS: 0,0,"T-Mobile",7
  int q1 = copsResp.indexOf('"');
  int q2 = (q1 >= 0) ? copsResp.indexOf('"', q1 + 1) : -1;
  if (q1 >= 0 && q2 > q1) return copsResp.substring(q1 + 1, q2);
  return copsResp;  // return raw if no quotes
}

// =============================================================
//  SETUP — runs the entire diagnostic sequence
// =============================================================
void setup() {
  SerialMon.begin(115200);
  delay(1000);

  // ═══════════════════════════════════════════════════════════
  //  STEP 0: BOOT & ID
  // ═══════════════════════════════════════════════════════════
  printSection("STEP 0: BOOT & ID");
  SerialMon.println("SpeedTalk Full System Diagnostic (SMS out + SMS in + Data + HTTP)");
  SerialMon.println("Device SIM: " + String(DEVICE_NUMBER));
  SerialMon.println("Target:     " + String(USER_PHONE));

  // Power on modem (same sequence as AntiTheftSystemLilygo.ino)
  ts(); SerialMon.println("Powering on modem...");
  pinMode(MODEM_FLIGHT, OUTPUT); digitalWrite(MODEM_FLIGHT, HIGH);
  pinMode(MODEM_PWRKEY, OUTPUT); digitalWrite(MODEM_PWRKEY, HIGH);
  delay(300); digitalWrite(MODEM_PWRKEY, LOW);

  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  ts(); SerialMon.println("Waiting 6s for modem boot...");
  delay(6000);

  // Wait for AT to respond, retry up to 10 times
  ts(); SerialMon.println("AT handshake (up to 10 retries)...");
  bool online = false;
  for (int i = 0; i < 10; i++) {
    ts(); SerialMon.print("AT attempt "); SerialMon.println(i + 1);
    if (sendAT("AT", "OK", 1000).indexOf("OK") >= 0) {
      online = true;
      ts(); SerialMon.println("Modem responded OK");
      break;
    }
    delay(1000);
  }
  if (!online) {
    ts(); SerialMon.println("FATAL: Modem did not respond after 10 attempts.");
    return;
  }

  // ATE0
  sendAT("ATE0", "OK", 1000);
  ts(); SerialMon.println("Echo off (ATE0)");

  // Verbose errors
  sendAT("AT+CMEE=2", "OK", 1000);

  // Modem ID block
  SerialMon.println();
  SerialMon.println("--- Modem Identification ---");
  runAndPrint("ATI (model)     ", "ATI", 3000);
  runAndPrint("AT+CGMR (fw)    ", "AT+CGMR", 3000);
  runAndPrint("AT+CIMI (IMSI)  ", "AT+CIMI", 3000);
  runAndPrint("AT+CCID (ICCID) ", "AT+CCID", 3000);
  runAndPrint("AT+CSQ  (signal)", "AT+CSQ", 3000);

  // SMS configuration
  SerialMon.println();
  SerialMon.println("--- SMS Configuration ---");
  String cmgf = sendAT("AT+CMGF=1", "OK", 2000);
  ts(); SerialMon.println("AT+CMGF=1 (text mode): " + String(cmgf.indexOf("OK") >= 0 ? "OK" : "FAIL"));

  String cnmi = sendAT("AT+CNMI=2,2,0,0,0", "OK", 2000);
  ts(); SerialMon.println("AT+CNMI=2,2,0,0,0 (URC delivery): " + String(cnmi.indexOf("OK") >= 0 ? "OK" : "FAIL"));

  String cpms = sendAT("AT+CPMS=\"ME\",\"ME\",\"ME\"", "OK", 3000);
  ts(); SerialMon.println("AT+CPMS (modem storage): " + String(cpms.indexOf("OK") >= 0 ? "OK" : "FAIL"));

  sendAT("AT+CMGD=1,4", "OK", 5000);
  ts(); SerialMon.println("AT+CMGD=1,4 (cleared stale SMS)");

  // Network registration
  SerialMon.println();
  SerialMon.println("--- Network Registration ---");
  waitForRegistration(60);

  String copsResp = sendAT("AT+COPS?", "OK", 3000);
  copsResp.trim();
  initialOperator = extractOperator(copsResp);
  ts(); SerialMon.println("Initial operator (AT+COPS?): " + copsResp);

  String cgregResp = sendAT("AT+CGREG?", "OK", 2000);
  cgregResp.trim();
  ts(); SerialMon.println("PS registration (AT+CGREG?): " + cgregResp);

  scanForSmsURC();

  // ═══════════════════════════════════════════════════════════
  //  STEP 1: FORCE OPERATOR RESET
  // ═══════════════════════════════════════════════════════════
  printSection("STEP 1: FORCE OPERATOR RESET");
  SerialMon.println("Deregister + auto-reselect to see if we move off STKMOBI");

  ts(); SerialMon.println("AT+COPS=2 (deregister)...");
  String dereg = sendAT("AT+COPS=2", "OK", 15000);
  ts(); SerialMon.println("Response: " + dereg);
  delay(3000);
  scanForSmsURC();

  ts(); SerialMon.println("AT+COPS=0 (auto-reselect, may take 30+s)...");
  String autoreg = sendAT("AT+COPS=0", "OK", 45000);
  ts(); SerialMon.println("Response: " + autoreg);
  delay(5000);
  scanForSmsURC();

  // Re-registration poll
  waitForRegistration(60);

  copsResp = sendAT("AT+COPS?", "OK", 3000);
  copsResp.trim();
  resetOperator = extractOperator(copsResp);
  ts(); SerialMon.println("After reset (AT+COPS?): " + copsResp);

  if (resetOperator.indexOf("STKMOBI") >= 0 || resetOperator.indexOf("stkmobi") >= 0) {
    ts(); SerialMon.println("OPERATOR STILL LOCKED TO STKMOBI");
  } else {
    ts(); SerialMon.println("OPERATOR UNLOCKED: " + resetOperator);
  }

  cgregResp = sendAT("AT+CGREG?", "OK", 2000);
  cgregResp.trim();
  ts(); SerialMon.println("PS re-registration (AT+CGREG?): " + cgregResp);

  scanForSmsURC();

  // ═══════════════════════════════════════════════════════════
  //  STEP 2: SMS OUTBOUND TEST
  // ═══════════════════════════════════════════════════════════
  printSection("STEP 2: SMS OUTBOUND TEST");
  SerialMon.println("Sending test SMS to " + String(USER_PHONE));

  String smsBody = "SpeedTalk diagnostic test. Time: " + String(millis()) + ". Reply within 90s to test inbound.";
  ts(); SerialMon.println("Body: " + smsBody);

  // Drain SerialAT before starting SMS send
  while (SerialAT.available()) SerialAT.read();

  // Send AT+CMGS command
  SerialAT.print("AT+CMGS=\"");
  SerialAT.print(USER_PHONE);
  SerialAT.println("\"");

  // Wait for ">" prompt (up to 10s)
  unsigned long promptStart = millis();
  String promptResp;
  bool gotPrompt = false;
  while (millis() - promptStart < 10000) {
    while (SerialAT.available()) promptResp += (char)SerialAT.read();
    if (promptResp.indexOf(">") >= 0) { gotPrompt = true; break; }
    delay(20);
  }

  if (!gotPrompt) {
    ts(); SerialMon.println("SMS_OUT: FAIL - no '>' prompt received");
    ts(); SerialMon.println("Raw: " + promptResp);
    smsOutSuccess = false;
  } else {
    ts(); SerialMon.println("Got '>' prompt, writing body...");

    // Write body and terminate with Ctrl+Z
    SerialAT.print(smsBody);
    SerialAT.write((uint8_t)0x1A);  // Ctrl+Z

    // Wait for +CMGS response (up to 30s)
    unsigned long sendStart = millis();
    String sendResp;
    bool sendDone = false;
    while (millis() - sendStart < 30000) {
      while (SerialAT.available()) sendResp += (char)SerialAT.read();
      if (sendResp.indexOf("+CMGS:") >= 0 && sendResp.indexOf("OK") >= 0) {
        sendDone = true;
        break;
      }
      if (sendResp.indexOf("ERROR") >= 0) break;
      delay(50);
    }

    if (sendDone) {
      // Extract reference number
      int refIdx = sendResp.indexOf("+CMGS:");
      String refNum = sendResp.substring(refIdx);
      refNum.trim();
      smsOutRef = refNum;
      smsOutSuccess = true;
      ts(); SerialMon.println("SMS_OUT: SUCCESS (" + refNum + ")");
    } else {
      smsOutSuccess = false;
      ts(); SerialMon.println("SMS_OUT: FAIL");
      ts(); SerialMon.println("Raw: " + sendResp);
    }
  }

  scanForSmsURC();

  // ═══════════════════════════════════════════════════════════
  //  STEP 3: SMS INBOUND LISTENING WINDOW
  // ═══════════════════════════════════════════════════════════
  printSection("STEP 3: SMS INBOUND LISTENING WINDOW");
  SerialMon.println();
  SerialMon.println(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>");
  SerialMon.println(">>> USER ACTION REQUIRED: Text any message to");
  SerialMon.println(">>> " + String(DEVICE_NUMBER) + " from your phone NOW");
  SerialMon.println(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>");
  SerialMon.println();

  int countBefore = smsReceivedCount;
  unsigned long listenStart = millis();
  int lastSafetyNet = 0;

  while (millis() - listenStart < 90000UL) {
    scanForSmsURC();

    // Safety-net CMGL poll every 30 seconds
    int elapsed = (millis() - listenStart) / 1000;
    int safetyBucket = elapsed / 30;
    if (safetyBucket > lastSafetyNet) {
      lastSafetyNet = safetyBucket;
      ts(); SerialMon.println("Safety-net: AT+CMGL=\"REC UNREAD\"");
      String cmgl = sendAT("AT+CMGL=\"REC UNREAD\"", "OK", 5000);
      cmgl.trim();
      if (cmgl.indexOf("+CMGL:") >= 0) {
        ts(); SerialMon.println("Safety-net found unread SMS:");
        SerialMon.println(cmgl);
        smsReceivedCount++;
        // Clear them
        sendAT("AT+CMGD=1,4", "OK", 5000);
      }
    }

    // Progress every 10 seconds
    if (elapsed % 10 == 0 && elapsed > 0) {
      static int lastPrinted = 0;
      if (elapsed != lastPrinted) {
        lastPrinted = elapsed;
        ts(); SerialMon.print("Listening... ");
        SerialMon.print(90 - elapsed);
        SerialMon.print("s remaining, ");
        SerialMon.print(smsReceivedCount - countBefore);
        SerialMon.println(" SMS received so far");
      }
    }

    delay(100);
  }

  int inboundCount = smsReceivedCount - countBefore;
  SerialMon.println();
  ts(); SerialMon.print("SMS_IN: ");
  SerialMon.print(smsReceivedCount);
  SerialMon.print(" total message(s) received (");
  SerialMon.print(inboundCount);
  SerialMon.println(" during listening window)");

  // ═══════════════════════════════════════════════════════════
  //  STEP 4: DATA PDP ACTIVATION
  // ═══════════════════════════════════════════════════════════
  printSection("STEP 4: DATA PDP ACTIVATION");
  SerialMon.println("Testing APNs for IPv4 PDP...");

  for (int a = 0; a < apnCount; a++) {
    const char* apn = apnList[a];
    SerialMon.println();
    SerialMon.print("--- Testing APN: ");
    SerialMon.print(apn);
    SerialMon.println(" ---");

    scanForSmsURC();

    // Deactivate existing PDP
    ts(); SerialMon.println("AT+CGACT=0,1 (deactivate)...");
    sendAT("AT+CGACT=0,1", "OK", 5000);

    // Set context
    String cgdcontCmd = "AT+CGDCONT=1,\"IP\",\"" + String(apn) + "\"";
    ts(); SerialMon.println(cgdcontCmd);
    String cgdcontResp = sendAT(cgdcontCmd, "OK", 3000);
    ts(); SerialMon.println("Response: " + cgdcontResp);

    // Activate
    ts(); SerialMon.println("AT+CGACT=1,1 (activate, up to 15s)...");
    String cgactResp = sendAT("AT+CGACT=1,1", "OK", 15000);
    cgactResp.trim();
    ts(); SerialMon.println("Response: " + cgactResp);

    if (cgactResp.indexOf("OK") < 0) {
      ts(); SerialMon.println("ERROR: PDP activation failed for APN '" + String(apn) + "'");
      continue;
    }

    scanForSmsURC();

    // Query address
    ts(); SerialMon.println("AT+CGPADDR=1...");
    String addrResp = sendAT("AT+CGPADDR=1", "OK", 5000);
    addrResp.trim();
    ts(); SerialMon.println("Raw: " + addrResp);

    String addr = extractAddr(addrResp);
    ts(); SerialMon.println("Extracted address: '" + addr + "'");

    if (addr.length() == 0) {
      ts(); SerialMon.println("ERROR: No address returned");
      continue;
    }

    if (looksLikeIPv4(addr)) {
      SerialMon.println();
      SerialMon.println("**************************************************");
      ts(); SerialMon.println("*** SUCCESS *** APN '" + String(apn) + "' returned IPv4: " + addr);
      SerialMon.println("**************************************************");
      dataWorked = true;
      winningApn = String(apn);
      winningIp  = addr;
      break;
    }

    if (addr.indexOf(':') >= 0) {
      ts(); SerialMon.println("IPv6 only: " + addr);
      continue;
    }

    ts(); SerialMon.println("ERROR: unrecognized address format: " + addr);
  }

  if (!dataWorked) {
    SerialMon.println();
    ts(); SerialMon.println("No APN returned IPv4.");
  }

  scanForSmsURC();

  // ═══════════════════════════════════════════════════════════
  //  STEP 5: HTTP CONNECTIVITY
  // ═══════════════════════════════════════════════════════════
  printSection("STEP 5: HTTP CONNECTIVITY");

  if (!dataWorked) {
    SerialMon.println("HTTP SKIPPED (no IPv4 from Step 4)");
    httpStatus = -999;
  } else {
    String httpBody = "Diagnostic POST from SpeedTalk TopValue T3 - millis=" + String(millis());
    String url = "http://" + String(NTFY_IP) + "/" + String(NTFY_TOPIC);
    ts(); SerialMon.println("POST to: " + url);
    ts(); SerialMon.println("Body: " + httpBody);

    // Clean up any prior HTTP session
    sendAT("AT+HTTPTERM", "OK", 1000);

    // HTTPINIT
    String initResp = sendAT("AT+HTTPINIT", "OK", 5000);
    ts(); SerialMon.println("HTTPINIT: " + initResp);

    if (initResp.indexOf("OK") < 0) {
      ts(); SerialMon.println("HTTP FAIL: HTTPINIT failed");
      httpStatus = -1;
    } else {
      // Configure URL
      String urlCmd = "AT+HTTPPARA=\"URL\",\"" + url + "\"";
      String urlResp = sendAT(urlCmd, "OK", 5000);
      ts(); SerialMon.println("HTTPPARA URL: " + urlResp);

      // Content type
      String ctResp = sendAT("AT+HTTPPARA=\"CONTENT\",\"text/plain\"", "OK", 5000);
      ts(); SerialMon.println("HTTPPARA CONTENT: " + ctResp);

      // Host header (required when using IP address directly)
      String hostResp = sendAT("AT+HTTPPARA=\"USERDATA\",\"Host: ntfy.sh\"", "OK", 3000);
      ts(); SerialMon.println("HTTPPARA USERDATA (Host): " + hostResp);

      // Upload body
      String dataCmd = "AT+HTTPDATA=" + String(httpBody.length()) + ",10000";
      ts(); SerialMon.println(dataCmd);
      while (SerialAT.available()) SerialAT.read();
      SerialAT.println(dataCmd);

      // Wait for DOWNLOAD prompt
      unsigned long dlStart = millis();
      String dlResp;
      bool gotDownload = false;
      while (millis() - dlStart < 5000) {
        while (SerialAT.available()) dlResp += (char)SerialAT.read();
        if (dlResp.indexOf("DOWNLOAD") >= 0) { gotDownload = true; break; }
        if (dlResp.indexOf("ERROR") >= 0) break;
        delay(10);
      }

      if (!gotDownload) {
        ts(); SerialMon.println("HTTP FAIL: no DOWNLOAD prompt. Raw: " + dlResp);
        sendAT("AT+HTTPTERM", "OK", 1000);
        httpStatus = -1;
      } else {
        ts(); SerialMon.println("Got DOWNLOAD prompt, writing body...");
        SerialAT.print(httpBody);

        // Wait for OK after data upload
        unsigned long okStart = millis();
        String okResp;
        bool gotOk = false;
        while (millis() - okStart < 10000) {
          while (SerialAT.available()) okResp += (char)SerialAT.read();
          if (okResp.indexOf("OK") >= 0) { gotOk = true; break; }
          if (okResp.indexOf("ERROR") >= 0) break;
          delay(10);
        }

        if (!gotOk) {
          ts(); SerialMon.println("HTTP FAIL: no OK after data upload. Raw: " + okResp);
          sendAT("AT+HTTPTERM", "OK", 1000);
          httpStatus = -1;
        } else {
          ts(); SerialMon.println("Body uploaded OK");

          // Execute POST
          ts(); SerialMon.println("AT+HTTPACTION=1 (POST)...");
          sendAT("AT+HTTPACTION=1", "OK", 3000);

          // Wait for +HTTPACTION URC (up to 20s)
          unsigned long actStart = millis();
          String actResp;
          httpStatus = -1;
          while (millis() - actStart < 20000) {
            while (SerialAT.available()) actResp += (char)SerialAT.read();
            int idx = actResp.indexOf("+HTTPACTION:");
            if (idx >= 0) {
              int eol = actResp.indexOf('\n', idx);
              if (eol > idx) {
                String urc = actResp.substring(idx, eol);
                ts(); SerialMon.println("URC: " + urc);
                // Parse: +HTTPACTION: 1,<status>,<len>
                int c1 = urc.indexOf(',');
                int c2 = urc.indexOf(',', c1 + 1);
                if (c1 > 0 && c2 > c1) {
                  httpStatus = urc.substring(c1 + 1, c2).toInt();
                }
                break;
              }
            }
            delay(20);
          }

          ts(); SerialMon.println("HTTP_RESULT: " + String(httpStatus));

          sendAT("AT+HTTPTERM", "OK", 1000);
        }
      }
    }
  }

  scanForSmsURC();

  // ═══════════════════════════════════════════════════════════
  //  FINAL SUMMARY
  // ═══════════════════════════════════════════════════════════
  SerialMon.println();
  SerialMon.println();
  SerialMon.println("================================================");
  SerialMon.println("              FINAL SUMMARY");
  SerialMon.println("================================================");
  SerialMon.println("  Initial operator:  " + initialOperator);
  SerialMon.println("  After COPS reset:  " + resetOperator);
  SerialMon.println("  SMS_OUT:           " + String(smsOutSuccess ? "SUCCESS" : "FAIL")
                     + (smsOutRef.length() > 0 ? " (" + smsOutRef + ")" : ""));
  SerialMon.println("  SMS_IN:            " + String(smsReceivedCount) + " message(s) received");

  if (dataWorked)
    SerialMon.println("  DATA:              SUCCESS (IPv4 " + winningIp + " on APN " + winningApn + ")");
  else
    SerialMon.println("  DATA:              NO IPv4 OBTAINED");

  if (httpStatus == -999)
    SerialMon.println("  HTTP:              SKIPPED");
  else if (httpStatus == 200)
    SerialMon.println("  HTTP:              SUCCESS (200)");
  else
    SerialMon.println("  HTTP:              FAIL (" + String(httpStatus) + ")");

  SerialMon.println("================================================");
  SerialMon.println();
  ts(); SerialMon.println("Diagnostic complete. Output is stable.");
}

// ─────────────────────────────────────────────────────────────
//  Loop — do nothing, output stands still for easy capture
// ─────────────────────────────────────────────────────────────
void loop() {
  delay(1000);
}
