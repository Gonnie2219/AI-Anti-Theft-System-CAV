/*
 * SpeedTalk IPv6 Data Diagnostic
 *
 * Tests IPV6 and IPV4V6 PDP types across multiple APNs on the LILYGO
 * T-SIM7600G-H with SpeedTalk TopValue T3 SIM. If any PDP activates,
 * tests whether the SIM7600's HTTP stack can actually use it for a
 * real GET request.
 *
 * Context: "IP" PDP type failed AT+CGACT for all APNs in prior test.
 * This fills the gap by explicitly requesting IPv6 and dual-stack.
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

// ── Test constants ───────────────────────────────────────────
const char* APN_LIST[]  = { "Wholesale", "wholesale", "stkmobi", "mnet", "fast.t-mobile.com" };
const int   APN_COUNT   = 5;
const char* PDP_TYPES[] = { "IPV4V6", "IPV6" };
const int   PDP_COUNT   = 2;
const char* TEST_URL    = "http://example.com/";

// ── Result tracking ──────────────────────────────────────────
String winningPdpType = "";
String winningApn     = "";
String winningAddr    = "";
String addrFamily     = "NONE";
bool   httpInitOK     = false;
int    httpStatus     = -1;
String httpRawResult  = "";
String carrier        = "";

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
//  Address classification helpers
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

bool looksLikeIPv6(const String& s) {
  return s.indexOf(':') >= 0;
}

// Extract everything after "+CGPADDR: 1," from the response.
// The modem may return:
//   +CGPADDR: 1,"10.0.0.1"                     (single IPv4)
//   +CGPADDR: 1,"2001:db8::1"                   (single IPv6)
//   +CGPADDR: 1,"10.0.0.1","2001:db8::1"        (dual-stack)
//   +CGPADDR: 1,10.0.0.1                         (no quotes, some FW)
String extractAddrRaw(const String& resp) {
  int idx = resp.indexOf("+CGPADDR:");
  if (idx < 0) return "";
  int comma = resp.indexOf(',', idx);
  if (comma < 0) return "";
  String tail = resp.substring(comma + 1);
  // Trim trailing OK/whitespace
  int okIdx = tail.indexOf("\r\nOK");
  if (okIdx < 0) okIdx = tail.indexOf("\nOK");
  if (okIdx > 0) tail = tail.substring(0, okIdx);
  tail.trim();
  return tail;
}

// Classify the raw address string from +CGPADDR.
// Sets addrFamily and returns a cleaned display string.
String classifyAddr(const String& raw, String& family) {
  if (raw.length() == 0) { family = "NONE"; return ""; }

  // Strip all quotes for analysis
  String clean = raw;
  clean.replace("\"", "");
  clean.trim();

  if (clean.length() == 0 || clean == "0.0.0.0") {
    family = "NONE";
    return "";
  }

  bool hasV4 = looksLikeIPv4(clean);
  bool hasV6 = looksLikeIPv6(clean);

  if (hasV4 && hasV6)      family = "DUAL";
  else if (hasV4)           family = "IPv4";
  else if (hasV6)           family = "IPv6";
  else                      family = "NONE";

  return clean;
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
  SerialMon.println("SpeedTalk IPv6 Data Diagnostic");
  SerialMon.println("Purpose: test IPV6 and IPV4V6 PDP types (IP already known to fail)");
  SerialMon.println("Board:   LILYGO T-SIM7600G-H");

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

  // Network registration
  SerialMon.println();
  SerialMon.println("--- Network Registration ---");
  ts(); SerialMon.print("Polling AT+CREG?");
  bool registered = false;
  for (int i = 0; i < 30; i++) {
    String r = sendAT("AT+CREG?", "OK", 2000);
    if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) {
      registered = true;
      SerialMon.println();
      ts(); SerialMon.println("Registered! Raw: " + r);
      break;
    }
    SerialMon.print(".");
    delay(2000);
  }
  if (!registered) {
    SerialMon.println();
    ts(); SerialMon.println("WARNING: Not registered after 60s. Continuing anyway...");
  }

  String copsResp = sendAT("AT+COPS?", "OK", 3000);
  copsResp.trim();
  // Extract operator name from quotes
  int q1 = copsResp.indexOf('"');
  int q2 = (q1 >= 0) ? copsResp.indexOf('"', q1 + 1) : -1;
  carrier = (q1 >= 0 && q2 > q1) ? copsResp.substring(q1 + 1, q2) : copsResp;
  ts(); SerialMon.println("Carrier (AT+COPS?): " + copsResp);

  String cgregResp = sendAT("AT+CGREG?", "OK", 2000);
  cgregResp.trim();
  ts(); SerialMon.println("PS registration (AT+CGREG?): " + cgregResp);

  // ═══════════════════════════════════════════════════════════
  //  STEP 1: PDP MATRIX TEST
  // ═══════════════════════════════════════════════════════════
  printSection("STEP 1: PDP MATRIX TEST");
  SerialMon.println("Testing IPV6 and IPV4V6 PDP types across APNs");
  SerialMon.println("(IP type skipped -- known to fail from prior diagnostic)");

  bool found = false;
  for (int p = 0; p < PDP_COUNT && !found; p++) {
    const char* pdpType = PDP_TYPES[p];
    for (int a = 0; a < APN_COUNT && !found; a++) {
      const char* apn = APN_LIST[a];
      SerialMon.println();
      SerialMon.print("--- PDP=");
      SerialMon.print(pdpType);
      SerialMon.print(", APN=");
      SerialMon.print(apn);
      SerialMon.println(" ---");

      // Deactivate existing PDP
      ts(); SerialMon.println("AT+CGACT=0,1 (deactivate)...");
      sendAT("AT+CGACT=0,1", "OK", 5000);

      // Set context
      String cgdcontCmd = "AT+CGDCONT=1,\"" + String(pdpType) + "\",\"" + String(apn) + "\"";
      ts(); SerialMon.println(cgdcontCmd);
      String cgdcontResp = sendAT(cgdcontCmd, "OK", 3000);
      cgdcontResp.trim();
      ts(); SerialMon.println("CGDCONT response: " + cgdcontResp);

      // Activate
      ts(); SerialMon.println("AT+CGACT=1,1 (activate, up to 15s)...");
      String cgactResp = sendAT("AT+CGACT=1,1", "OK", 15000);
      cgactResp.trim();
      ts(); SerialMon.println("CGACT response: " + cgactResp);

      if (cgactResp.indexOf("OK") < 0) {
        ts(); SerialMon.println("PDP activation FAILED");
        continue;
      }

      // Query address
      ts(); SerialMon.println("AT+CGPADDR=1...");
      String addrResp = sendAT("AT+CGPADDR=1", "OK", 5000);
      addrResp.trim();
      ts(); SerialMon.println("CGPADDR raw: " + addrResp);

      String raw = extractAddrRaw(addrResp);
      ts(); SerialMon.println("Extracted raw: '" + raw + "'");

      String family;
      String cleaned = classifyAddr(raw, family);

      if (family == "NONE") {
        ts(); SerialMon.println("NO ADDRESS");
        continue;
      }

      if (family == "IPv4") {
        ts(); SerialMon.println("IPv4 ONLY: " + cleaned);
      } else if (family == "IPv6") {
        ts(); SerialMon.println("IPv6 ONLY: " + cleaned);
      } else if (family == "DUAL") {
        ts(); SerialMon.println("DUAL: " + cleaned);
      }

      // We got an address -- save and break
      SerialMon.println();
      SerialMon.println("**************************************************");
      ts(); SerialMon.println("*** ADDRESS OBTAINED *** PDP=" + String(pdpType) + " APN=" + String(apn));
      ts(); SerialMon.println("    Family: " + family + "  Addr: " + cleaned);
      SerialMon.println("**************************************************");

      winningPdpType = String(pdpType);
      winningApn     = String(apn);
      winningAddr    = cleaned;
      addrFamily     = family;
      found = true;
    }
  }

  if (!found) {
    SerialMon.println();
    ts(); SerialMon.println("No PDP/APN combination yielded an address.");
    ts(); SerialMon.println("Skipping Steps 2 and 3.");
    // Jump to final summary (fall through)
  }

  // ═══════════════════════════════════════════════════════════
  //  STEP 2: HTTP STACK INITIALIZATION
  // ═══════════════════════════════════════════════════════════
  if (addrFamily != "NONE") {
    printSection("STEP 2: HTTP STACK INITIALIZATION");
    SerialMon.println("Testing HTTPINIT over " + winningPdpType + " PDP (" + addrFamily + ")");

    // Clean up any prior session
    sendAT("AT+HTTPTERM", "OK", 1000);

    ts(); SerialMon.println("AT+HTTPINIT...");
    String initResp = sendAT("AT+HTTPINIT", "OK", 10000);
    initResp.trim();
    ts(); SerialMon.println("Response: " + initResp);

    if (initResp.indexOf("OK") >= 0) {
      httpInitOK = true;
      ts(); SerialMon.println("HTTPINIT: OK");
    } else {
      httpInitOK = false;
      ts(); SerialMon.println("HTTPINIT: FAILED (" + initResp + ")");
      ts(); SerialMon.println("This matches the May 3 signature; modem's HTTP stack rejects this PDP type");
    }
  }

  // ═══════════════════════════════════════════════════════════
  //  STEP 3: HTTP GET TO PUBLIC IPv6-CAPABLE ENDPOINT
  // ═══════════════════════════════════════════════════════════
  if (addrFamily != "NONE" && httpInitOK) {
    printSection("STEP 3: HTTP GET TO PUBLIC IPv6-CAPABLE ENDPOINT");
    SerialMon.println("Target: " + String(TEST_URL));
    SerialMon.println("Using hostname forces modem DNS resolution through its IP stack.");

    // Set URL
    String urlCmd = "AT+HTTPPARA=\"URL\",\"" + String(TEST_URL) + "\"";
    ts(); SerialMon.println(urlCmd);
    String urlResp = sendAT(urlCmd, "OK", 5000);
    ts(); SerialMon.println("HTTPPARA URL: " + urlResp);

    // Execute GET
    ts(); SerialMon.println("AT+HTTPACTION=0 (GET, up to 30s)...");
    while (SerialAT.available()) SerialAT.read();
    SerialAT.println("AT+HTTPACTION=0");

    // Wait for +HTTPACTION URC
    unsigned long actStart = millis();
    String actResp;
    bool gotAction = false;
    while (millis() - actStart < 30000) {
      while (SerialAT.available()) actResp += (char)SerialAT.read();
      int idx = actResp.indexOf("+HTTPACTION:");
      if (idx >= 0) {
        int eol = actResp.indexOf('\n', idx);
        if (eol > idx) {
          String urc = actResp.substring(idx, eol);
          urc.trim();
          ts(); SerialMon.println("URC: " + urc);
          httpRawResult = urc;

          // Parse: +HTTPACTION: 0,<status>,<len>
          int c1 = urc.indexOf(',');
          int c2 = urc.indexOf(',', c1 + 1);
          if (c1 > 0 && c2 > c1) {
            httpStatus = urc.substring(c1 + 1, c2).toInt();
          }
          gotAction = true;
          break;
        }
      }
      delay(20);
    }

    if (!gotAction) {
      ts(); SerialMon.println("HTTP GET TIMEOUT -- request did not complete within 30s");
      ts(); SerialMon.println("Accumulated response: " + actResp);
      httpStatus = -1;
      httpRawResult = "TIMEOUT";
    } else if (httpStatus == 200) {
      // Try to get the data length from URC
      int c2 = httpRawResult.lastIndexOf(',');
      String dataLen = (c2 > 0) ? httpRawResult.substring(c2 + 1) : "?";
      dataLen.trim();
      ts(); SerialMon.println("HTTP GET SUCCESS (200, " + dataLen + " bytes)");
    } else if (httpStatus == 301 || httpStatus == 302 || httpStatus == 308) {
      ts(); SerialMon.println("HTTP GET REDIRECT (" + String(httpStatus) + ") -- data path works, server wants HTTPS");
    } else if (httpStatus >= 400 && httpStatus < 600) {
      ts(); SerialMon.println("HTTP GET got " + String(httpStatus) + " -- data path works, server-side error");
    } else if (httpStatus >= 600) {
      ts(); SerialMon.println("HTTP INTERNAL ERROR " + String(httpStatus) + " -- modem-side failure, likely DNS or socket");
    }

    // Clean up
    sendAT("AT+HTTPTERM", "OK", 1000);
  } else if (addrFamily != "NONE" && !httpInitOK) {
    printSection("STEP 3: HTTP GET (SKIPPED)");
    SerialMon.println("Skipped because HTTPINIT failed in Step 2.");
  }

  // ═══════════════════════════════════════════════════════════
  //  FINAL SUMMARY
  // ═══════════════════════════════════════════════════════════
  SerialMon.println();
  SerialMon.println();
  SerialMon.println("================================================");
  SerialMon.println("              FINAL SUMMARY");
  SerialMon.println("================================================");
  SerialMon.println("  Carrier:                 " + carrier);
  SerialMon.println("  Winning PDP / APN:       " +
    (winningPdpType.length() > 0 ? winningPdpType + " / " + winningApn : "NONE"));
  SerialMon.println("  Address family:          " + addrFamily);
  SerialMon.println("  Address obtained:        " +
    (winningAddr.length() > 0 ? winningAddr : "-"));
  SerialMon.println("  HTTPINIT result:         " +
    String(addrFamily == "NONE" ? "NOT_ATTEMPTED" : (httpInitOK ? "OK" : "FAILED")));
  SerialMon.println("  HTTP GET status:         " +
    (addrFamily == "NONE" || !httpInitOK ? String("-") : String(httpStatus)));
  SerialMon.println("================================================");

  // Interpretation
  SerialMon.println();
  SerialMon.println("INTERPRETATION:");

  if (addrFamily == "NONE") {
    SerialMon.println("  Carrier rejected ALL PDP activation regardless of family.");
    SerialMon.println("  STKMOBI IoT routing has no PDP profile assignable to this");
    SerialMon.println("  SIM. SpeedTalk path is dead.");
    SerialMon.println("  Next: order AT&T-backed MVNO SIM.");
  } else if ((addrFamily == "IPv6" || addrFamily == "DUAL") && !httpInitOK) {
    SerialMon.println("  Modem obtained " + addrFamily + " PDP but its HTTP stack rejects it");
    SerialMon.println("  (HTTPINIT failed). This is the May 3 failure signature.");
    SerialMon.println("  SIM7600 firmware LE20B05SIM7600G22 cannot drive HTTP over IPv6.");
    SerialMon.println("  SpeedTalk path is dead. Next: AT&T MVNO.");
  } else if ((addrFamily == "IPv6" || addrFamily == "DUAL") && httpInitOK && httpStatus < 0) {
    SerialMon.println("  HTTP stack accepted the PDP but request never completed.");
    SerialMon.println("  Likely DNS6 resolution failure or upstream routing issue.");
    SerialMon.println("  Worth one more test with a literal IPv6 address before giving up.");
  } else if ((addrFamily == "IPv6" || addrFamily == "DUAL") && httpStatus >= 200 && httpStatus < 400) {
    SerialMon.println("  *** UNEXPECTED WIN *** IPv6 HTTP works on this modem on this SIM.");
    SerialMon.println("  Architecture pivot possible. Next: test HTTPS, then ntfy.sh");
    SerialMon.println("  end-to-end.");
  } else if (addrFamily == "IPv4" && httpInitOK && httpStatus >= 200 && httpStatus < 400) {
    SerialMon.println("  IPv4 obtained from IPV4V6/IPV6 request and HTTP works.");
    SerialMon.println("  Carrier gave us v4 even though we asked for v6.");
    SerialMon.println("  This is usable! Same as Hologram path.");
  } else {
    SerialMon.println("  Results don't match a known pattern. Raw data above.");
    SerialMon.println("  PDP=" + winningPdpType + " APN=" + winningApn);
    SerialMon.println("  Family=" + addrFamily + " HTTPINIT=" + String(httpInitOK ? "OK" : "FAIL"));
    SerialMon.println("  HTTP status=" + String(httpStatus));
  }

  SerialMon.println();
  SerialMon.println("================================================");
  ts(); SerialMon.println("Diagnostic complete. Output is stable.");
  SerialMon.println("================================================");
}

// ─────────────────────────────────────────────────────────────
//  Loop — do nothing, output stands still for easy capture
// ─────────────────────────────────────────────────────────────
void loop() {
  delay(1000);
}
