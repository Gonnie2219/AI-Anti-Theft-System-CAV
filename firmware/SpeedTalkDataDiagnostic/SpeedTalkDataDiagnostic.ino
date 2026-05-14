/*
 * SpeedTalk TopValue T3 Data Diagnostic
 *
 * One-shot diagnostic to determine whether SpeedTalk's TopValue T3 plan
 * provisions an IPv4 PDP context on the LILYGO T-SIM7600G-H, or whether
 * it remains IPv6-only like the old IoT Tracker Plan.
 *
 * Build target: ESP32 Dev Module
 * Board:        LILYGO T-SIM7600G-H
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

// ── APN list to test ─────────────────────────────────────────
const char* apnList[] = { "Wholesale", "wholesale", "stkmobi", "fast.t-mobile.com" };
const int   apnCount  = 4;

// ── AT command helper (verbatim from AntiTheftSystemLilygo.ino) ──
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

// ── Helpers ──────────────────────────────────────────────────
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

void printMinor(const char* title) {
  SerialMon.println();
  SerialMon.print("--- ");
  SerialMon.print(title);
  SerialMon.println(" ---");
}

void runAndPrint(const char* label, String cmd, unsigned long timeout) {
  String r = sendAT(cmd, "OK", timeout);
  r.trim();
  SerialMon.print("  ");
  SerialMon.print(label);
  SerialMon.print(": ");
  SerialMon.println(r);
}

// Check if a string looks like a dotted-quad IPv4 address
bool looksLikeIPv4(const String& s) {
  // Find any A.B.C.D pattern (digits separated by dots)
  int len = s.length();
  int dotCount = 0;
  int digitRun = 0;
  for (int i = 0; i < len; i++) {
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

// Extract the IP portion from +CGPADDR: 1,"<ip>" response
String extractAddr(const String& resp) {
  int idx = resp.indexOf("+CGPADDR:");
  if (idx < 0) return "";
  int q1 = resp.indexOf('"', idx);
  if (q1 < 0) {
    // Some firmware omits quotes: +CGPADDR: 1,10.x.x.x
    int comma = resp.indexOf(',', idx);
    if (comma < 0) return "";
    String tail = resp.substring(comma + 1);
    tail.trim();
    // Trim trailing OK
    int okIdx = tail.indexOf("OK");
    if (okIdx > 0) tail = tail.substring(0, okIdx);
    tail.trim();
    return tail;
  }
  int q2 = resp.indexOf('"', q1 + 1);
  if (q2 < 0) return resp.substring(q1 + 1);
  return resp.substring(q1 + 1, q2);
}

// ── Stored results ───────────────────────────────────────────
String winningApn = "";
String winningIp  = "";
String carrier    = "";
String fwVersion  = "";

// ─────────────────────────────────────────────────────────────
//  Setup — runs the entire diagnostic
// ─────────────────────────────────────────────────────────────
void setup() {
  SerialMon.begin(115200);
  delay(1000);

  // ── 1. Banner ──────────────────────────────────────────────
  printSection("SpeedTalk TopValue T3 Data Diagnostic");
  SerialMon.println("Purpose: determine if TopValue T3 provisions IPv4 PDP");
  SerialMon.println("Board:   LILYGO T-SIM7600G-H");

  // ── 2. Power on modem (same sequence as AntiTheftSystemLilygo.ino) ──
  printSection("Powering on modem");

  pinMode(MODEM_FLIGHT, OUTPUT); digitalWrite(MODEM_FLIGHT, HIGH);
  pinMode(MODEM_PWRKEY, OUTPUT); digitalWrite(MODEM_PWRKEY, HIGH);
  delay(300); digitalWrite(MODEM_PWRKEY, LOW);

  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  ts(); SerialMon.println("Waiting 6s for modem boot...");
  delay(6000);

  // ── 3. Wait for AT to respond ──────────────────────────────
  printSection("AT handshake (up to 10 retries)");
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
    SerialMon.println("Check wiring and power supply.");
    return;
  }

  // ── 4. ATE0 ────────────────────────────────────────────────
  sendAT("ATE0", "OK", 1000);
  ts(); SerialMon.println("Echo off (ATE0)");

  // Verbose errors
  sendAT("AT+CMEE=2", "OK", 1000);

  // ── 5. Modem identification block ──────────────────────────
  printSection("Modem Identification");
  runAndPrint("ATI (model)     ", "ATI", 3000);
  runAndPrint("AT+CGMR (fw)    ", "AT+CGMR", 3000);
  runAndPrint("AT+CIMI (IMSI)  ", "AT+CIMI", 3000);
  runAndPrint("AT+CCID (ICCID) ", "AT+CCID", 3000);
  runAndPrint("AT+CSQ  (signal)", "AT+CSQ", 3000);

  // Capture firmware for summary
  String fwResp = sendAT("AT+CGMR", "OK", 3000);
  fwVersion = fwResp;
  fwVersion.trim();

  // ── 6. Wait for network registration ──────────────────────
  printSection("Network Registration (up to 60s)");
  bool registered = false;
  ts(); SerialMon.print("Polling AT+CREG?");
  for (int i = 0; i < 30; i++) {
    String r = sendAT("AT+CREG?", "OK", 2000);
    if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) {
      registered = true;
      SerialMon.println();
      ts(); SerialMon.println("Registered!");
      ts(); SerialMon.println("Raw: " + r);
      break;
    }
    SerialMon.print(".");
    delay(2000);
  }
  if (!registered) {
    SerialMon.println();
    ts(); SerialMon.println("WARNING: Not registered after 60s. Continuing anyway...");
  }

  // Print carrier
  String copsResp = sendAT("AT+COPS?", "OK", 3000);
  copsResp.trim();
  ts(); SerialMon.println("Carrier (AT+COPS?): " + copsResp);
  carrier = copsResp;

  // Also check CGREG (packet-switched registration)
  String cgregResp = sendAT("AT+CGREG?", "OK", 2000);
  cgregResp.trim();
  ts(); SerialMon.println("PS reg (AT+CGREG?): " + cgregResp);

  // ── 7. Iterate APN list ────────────────────────────────────
  printSection("APN Testing");

  for (int a = 0; a < apnCount; a++) {
    const char* apn = apnList[a];
    SerialMon.println();
    SerialMon.print("--- Testing APN: ");
    SerialMon.print(apn);
    SerialMon.println(" ---");

    // 7a. Deactivate existing PDP
    ts(); SerialMon.println("Deactivating PDP (AT+CGACT=0,1)...");
    sendAT("AT+CGACT=0,1", "OK", 5000);  // ignore failures

    // 7b. Set context
    String cgdcontCmd = "AT+CGDCONT=1,\"IP\",\"" + String(apn) + "\"";
    ts(); SerialMon.println("Setting context: " + cgdcontCmd);
    String cgdcontResp = sendAT(cgdcontCmd, "OK", 3000);
    ts(); SerialMon.println("Response: " + cgdcontResp);

    // 7c. Activate
    ts(); SerialMon.println("Activating PDP (AT+CGACT=1,1)...");
    String cgactResp = sendAT("AT+CGACT=1,1", "OK", 15000);
    cgactResp.trim();
    ts(); SerialMon.println("Response: " + cgactResp);

    if (cgactResp.indexOf("OK") < 0) {
      ts(); SerialMon.println("ERROR: PDP activation failed for APN '" + String(apn) + "'");
      ts(); SerialMon.println("Raw: " + cgactResp);
      continue;
    }

    // 7d. Query address
    ts(); SerialMon.println("Querying address (AT+CGPADDR=1)...");
    String addrResp = sendAT("AT+CGPADDR=1", "OK", 5000);
    addrResp.trim();
    ts(); SerialMon.println("Raw response: " + addrResp);

    String addr = extractAddr(addrResp);
    ts(); SerialMon.println("Extracted address: '" + addr + "'");

    // 7e. Classify
    if (addr.length() == 0) {
      ts(); SerialMon.println("ERROR: No address returned for APN '" + String(apn) + "'");
      ts(); SerialMon.println("ERROR: " + addrResp);
      continue;
    }

    if (looksLikeIPv4(addr)) {
      SerialMon.println();
      SerialMon.println("**************************************************");
      ts(); SerialMon.println("*** SUCCESS *** APN '" + String(apn) + "' returned IPv4: " + addr);
      SerialMon.println("**************************************************");
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

  // ── 8. Final summary ──────────────────────────────────────
  printSection("FINAL SUMMARY");

  if (winningApn.length() > 0) {
    SerialMon.println("  Result:   IPv4 PDP OBTAINED");
    SerialMon.println("  APN:      " + winningApn);
    SerialMon.println("  IPv4:     " + winningIp);
    SerialMon.println("  Carrier:  " + carrier);
    SerialMon.println("  Firmware: " + fwVersion);
    SerialMon.println();
    SerialMon.println("  Next step: HTTP connectivity test (ntfy.sh POST).");
  } else {
    // Check if we got any IPv6
    bool hadError = false;
    // Re-check: were there activations that succeeded but gave IPv6?
    // We can't re-test here, so give both messages.
    SerialMon.println("  Result:   NO IPv4 OBTAINED");
    SerialMon.println();
    SerialMon.println("  All APNs returned IPv6-only or errored.");
    SerialMon.println("  Consumer-tier provisioning did NOT fix the IPv6-only");
    SerialMon.println("  PDP problem, OR provisioning has not propagated yet.");
    SerialMon.println();
    SerialMon.println("  Next steps:");
    SerialMon.println("    1. Wait 24h and retry (provisioning propagation).");
    SerialMon.println("    2. Contact SpeedTalk support and ask for IPv4 PDP.");
    SerialMon.println("    3. Try AT&T-backed MVNO if SpeedTalk cannot provide IPv4.");
  }

  SerialMon.println();
  SerialMon.println("=============================================");
  ts(); SerialMon.println("Diagnostic complete. Serial output is stable.");
  SerialMon.println("=============================================");
}

// ─────────────────────────────────────────────────────────────
//  Loop — do nothing, output stands still for easy capture
// ─────────────────────────────────────────────────────────────
void loop() {
  delay(1000);
}
