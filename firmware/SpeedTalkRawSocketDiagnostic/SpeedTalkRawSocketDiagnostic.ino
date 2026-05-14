/*
 * SpeedTalk Raw Socket Diagnostic
 *
 * Tests whether the SIM7600G-H's lower-level socket commands
 * (AT+NETOPEN, AT+CIPOPEN, AT+CIPSEND) work over an IPv6 PDP even
 * though HTTPINIT rejects it. If raw TCP works, we can implement
 * HTTP/1.1 manually and keep SpeedTalk.
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
const char* APN_TO_USE       = "mobilenet";
const char* PDP_TYPE         = "IPV4V6";
const char* NTFY_HOSTNAME    = "ntfy.sh";
const char* NTFY_LITERAL_IP6 = "2606:4700:3034::ac43:bea2";
const int   HTTP_PORT        = 80;
const char* HTTP_PATH        = "/antitheft-gonnie-2219";

// ── Result tracking ──────────────────────────────────────────
String carrier          = "";
String pdpAddrRaw       = "";
String pdpAddrCanonical = "";
String pdpFamily        = "NONE";   // IPv4, IPv6_HEX, IPv6_DECIMAL, NONE, UNKNOWN
bool   netopenOK        = false;
bool   cipopenHostOK    = false;
bool   httpHostOK        = false;
bool   cipopenLiteralOK = false;
bool   httpLiteralOK     = false;

// ── Struct for address parsing (must precede function declarations) ──
struct AddrResult {
  String family;     // "IPv4", "IPv6_HEX", "IPv6_DECIMAL", "UNKNOWN", "NONE"
  String canonical;  // standardized address string
};

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
//  parseCgpaddr — handles classic IPv6, dotted-decimal IPv6,
//  and normal IPv4.
//
//  Dotted-decimal IPv6: the SIM7600 sometimes returns IPv6
//  addresses as 16 dot-separated decimal octets, e.g.
//    38.7.251.145.23.237.128.26.172.57.193.184.156.216.181.229
//  which is 2607:FB91:17ED:801A:AC39:C1B8:9CD8:B5E5
// ─────────────────────────────────────────────────────────────
AddrResult parseCgpaddr(const String& resp) {
  AddrResult result;
  result.family = "NONE";
  result.canonical = "";

  int idx = resp.indexOf("+CGPADDR:");
  if (idx < 0) return result;

  // Find the address portion after "+CGPADDR: 1,"
  int comma = resp.indexOf(',', idx);
  if (comma < 0) return result;
  String tail = resp.substring(comma + 1);

  // Trim trailing OK/whitespace/quotes
  int okIdx = tail.indexOf("\r\nOK");
  if (okIdx < 0) okIdx = tail.indexOf("\nOK");
  if (okIdx > 0) tail = tail.substring(0, okIdx);
  tail.trim();
  tail.replace("\"", "");
  tail.trim();

  if (tail.length() == 0 || tail == "0.0.0.0") return result;

  // For dual-stack responses, take the second address (the IPv6 one).
  // Format: "10.x.x.x","2607:..." or "10.x.x.x","38.7.251..."
  // We'll check if there's a second quoted section or comma-separated addr.
  // Simple approach: if we see a second address after a comma that isn't
  // part of a dotted number, handle it. For now, use the LAST address
  // (IPv6 is what we care about).
  // Actually, let's just analyze the whole string for our classification.

  // Count dots in the entire tail
  int dotCount = 0;
  for (int i = 0; i < (int)tail.length(); i++) {
    if (tail.charAt(i) == '.') dotCount++;
  }

  bool hasColon = (tail.indexOf(':') >= 0);

  // Case 1: Contains colons -> IPv6 hex notation (possibly with IPv4 prefix for dual)
  if (hasColon) {
    // Extract just the IPv6 part (portion with colons)
    // If dual-stack, there may be a comma between IPv4 and IPv6
    String v6part = tail;
    // Find the portion containing colons
    int lastComma = tail.lastIndexOf(',');
    if (lastComma >= 0) {
      String after = tail.substring(lastComma + 1);
      after.trim();
      after.replace("\"", "");
      if (after.indexOf(':') >= 0) v6part = after;
    }
    result.family = "IPv6_HEX";
    result.canonical = v6part;
    return result;
  }

  // Case 2: Exactly 3 dots -> IPv4
  if (dotCount == 3) {
    result.family = "IPv4";
    result.canonical = tail;
    return result;
  }

  // Case 3: Exactly 15 dots -> 16 octets -> dotted-decimal IPv6
  if (dotCount == 15) {
    result.family = "IPv6_DECIMAL";
    // Convert 16 decimal octets to standard IPv6 hex notation
    // e.g. 38.7.251.145.23.237.128.26.172.57.193.184.156.216.181.229
    //   -> 2607:FB91:17ED:801A:AC39:C1B8:9CD8:B5E5
    uint8_t octets[16];
    String working = tail;
    bool parseOK = true;
    for (int i = 0; i < 16; i++) {
      int dot = working.indexOf('.');
      String part;
      if (i < 15) {
        if (dot < 0) { parseOK = false; break; }
        part = working.substring(0, dot);
        working = working.substring(dot + 1);
      } else {
        part = working;
      }
      int val = part.toInt();
      if (val < 0 || val > 255) { parseOK = false; break; }
      octets[i] = (uint8_t)val;
    }

    if (parseOK) {
      // Format as 8 groups of 4 hex digits
      char hex[40];
      snprintf(hex, sizeof(hex),
        "%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X",
        octets[0], octets[1], octets[2], octets[3],
        octets[4], octets[5], octets[6], octets[7],
        octets[8], octets[9], octets[10], octets[11],
        octets[12], octets[13], octets[14], octets[15]);
      result.canonical = String(hex);
    } else {
      result.family = "UNKNOWN";
      result.canonical = tail;
    }
    return result;
  }

  // Case 4: Dual-stack with dotted-decimal IPv6 (3 dots for IPv4 + comma + 15 dots for IPv6 = 19 dots total)
  // Look for a comma separating two addresses
  if (dotCount == 19 || dotCount > 15) {
    // Try to split on comma boundary between two addresses
    int splitComma = -1;
    for (int i = 0; i < (int)tail.length(); i++) {
      if (tail.charAt(i) == ',') {
        // Check if what follows has 15 dots
        String after = tail.substring(i + 1);
        after.trim();
        after.replace("\"", "");
        int afterDots = 0;
        for (int j = 0; j < (int)after.length(); j++) {
          if (after.charAt(j) == '.') afterDots++;
        }
        if (afterDots == 15) { splitComma = i; break; }
      }
    }
    if (splitComma >= 0) {
      String v6raw = tail.substring(splitComma + 1);
      v6raw.trim();
      v6raw.replace("\"", "");
      // Re-parse just the IPv6 portion
      AddrResult v6result;
      // Build a fake +CGPADDR response to recurse
      // Instead, just inline the 16-octet parse
      uint8_t octets[16];
      String working = v6raw;
      bool parseOK = true;
      for (int i = 0; i < 16; i++) {
        int dot = working.indexOf('.');
        String part;
        if (i < 15) {
          if (dot < 0) { parseOK = false; break; }
          part = working.substring(0, dot);
          working = working.substring(dot + 1);
        } else {
          part = working;
        }
        int val = part.toInt();
        if (val < 0 || val > 255) { parseOK = false; break; }
        octets[i] = (uint8_t)val;
      }
      if (parseOK) {
        result.family = "IPv6_DECIMAL";
        char hex[40];
        snprintf(hex, sizeof(hex),
          "%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X",
          octets[0], octets[1], octets[2], octets[3],
          octets[4], octets[5], octets[6], octets[7],
          octets[8], octets[9], octets[10], octets[11],
          octets[12], octets[13], octets[14], octets[15]);
        result.canonical = String(hex);
        return result;
      }
    }
  }

  // Fallback
  result.family = "UNKNOWN";
  result.canonical = tail;
  return result;
}

// ─────────────────────────────────────────────────────────────
//  Send raw HTTP GET over an open CIPOPEN socket and capture
//  the first ~200 bytes of the response.
//  Returns true if we saw "HTTP/" in the response.
// ─────────────────────────────────────────────────────────────
bool sendHttpGetAndCapture(const char* host) {
  String req = "GET " + String(HTTP_PATH) + " HTTP/1.1\r\n"
               "Host: " + String(host) + "\r\n"
               "User-Agent: SpeedTalkDiag\r\n"
               "Connection: close\r\n\r\n";

  int reqLen = req.length();
  ts(); SerialMon.println("Sending " + String(reqLen) + " byte HTTP GET via CIPSEND...");

  // Drain before CIPSEND
  while (SerialAT.available()) SerialAT.read();

  SerialAT.println("AT+CIPSEND=0," + String(reqLen));

  // Wait for ">" prompt (up to 5s)
  unsigned long pStart = millis();
  String pResp;
  bool gotPrompt = false;
  while (millis() - pStart < 5000) {
    while (SerialAT.available()) pResp += (char)SerialAT.read();
    if (pResp.indexOf(">") >= 0) { gotPrompt = true; break; }
    if (pResp.indexOf("ERROR") >= 0) break;
    delay(10);
  }

  if (!gotPrompt) {
    ts(); SerialMon.println("CIPSEND: no '>' prompt. Raw: " + pResp);
    return false;
  }

  ts(); SerialMon.println("Got '>' prompt, writing HTTP request...");
  SerialAT.print(req);

  // Wait for +CIPSEND confirmation (up to 10s)
  unsigned long sStart = millis();
  String sResp;
  bool sendOK = false;
  while (millis() - sStart < 10000) {
    while (SerialAT.available()) sResp += (char)SerialAT.read();
    if (sResp.indexOf("+CIPSEND:") >= 0) { sendOK = true; break; }
    if (sResp.indexOf("ERROR") >= 0) break;
    delay(10);
  }

  ts(); SerialMon.println("CIPSEND response: " + sResp);

  if (!sendOK) {
    ts(); SerialMon.println("CIPSEND failed");
    return false;
  }

  // Wait for response data: +IPD, HTTP/ bytes, or +IPCLOSE (up to 15s)
  ts(); SerialMon.println("Waiting for HTTP response (up to 15s)...");
  unsigned long rStart = millis();
  String rResp;
  while (millis() - rStart < 15000) {
    while (SerialAT.available()) rResp += (char)SerialAT.read();
    // Stop early if we got a good chunk or connection closed
    if (rResp.length() > 200) break;
    if (rResp.indexOf("+IPCLOSE:") >= 0) break;
    if (rResp.indexOf("CLOSED") >= 0) break;
    delay(20);
  }

  // Print first 200 chars of received data
  SerialMon.println();
  SerialMon.println("--- Received data (first 200 chars) ---");
  if (rResp.length() > 200) {
    SerialMon.println(rResp.substring(0, 200));
    SerialMon.println("... (" + String(rResp.length()) + " total bytes captured)");
  } else {
    SerialMon.println(rResp);
  }
  SerialMon.println("--- End received data ---");

  bool sawHTTP = (rResp.indexOf("HTTP/") >= 0);
  ts(); SerialMon.println(sawHTTP ? "HTTP response detected!" : "No HTTP response detected.");
  return sawHTTP;
}

// =============================================================
//  SETUP — runs the entire diagnostic sequence
// =============================================================
void setup() {
  SerialMon.begin(115200);
  delay(1000);

  // ═══════════════════════════════════════════════════════════
  //  STEP 0: BOOT, ID, REGISTER
  // ═══════════════════════════════════════════════════════════
  printSection("STEP 0: BOOT, ID, REGISTER");
  SerialMon.println("SpeedTalk Raw Socket Diagnostic");
  SerialMon.println("Purpose: test AT+NETOPEN / AT+CIPOPEN over IPv6 PDP");

  // Power on modem (same sequence as AntiTheftSystemLilygo.ino)
  ts(); SerialMon.println("Powering on modem...");
  pinMode(MODEM_FLIGHT, OUTPUT); digitalWrite(MODEM_FLIGHT, HIGH);
  pinMode(MODEM_PWRKEY, OUTPUT); digitalWrite(MODEM_PWRKEY, HIGH);
  delay(300); digitalWrite(MODEM_PWRKEY, LOW);

  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  ts(); SerialMon.println("Waiting 6s for modem boot...");
  delay(6000);

  // Wait for AT (10 retries)
  ts(); SerialMon.println("AT handshake...");
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
    ts(); SerialMon.println("FATAL: Modem did not respond.");
    return;
  }

  sendAT("ATE0", "OK", 1000);
  ts(); SerialMon.println("Echo off (ATE0)");
  sendAT("AT+CMEE=2", "OK", 1000);

  // ID block
  SerialMon.println();
  SerialMon.println("--- Modem Identification ---");
  runAndPrint("ATI (model)     ", "ATI", 3000);
  runAndPrint("AT+CGMR (fw)    ", "AT+CGMR", 3000);
  runAndPrint("AT+CIMI (IMSI)  ", "AT+CIMI", 3000);
  runAndPrint("AT+CCID (ICCID) ", "AT+CCID", 3000);
  runAndPrint("AT+CSQ  (signal)", "AT+CSQ", 3000);

  // Registration
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
    ts(); SerialMon.println("WARNING: Not registered after 60s. Continuing...");
  }

  String copsResp = sendAT("AT+COPS?", "OK", 3000);
  copsResp.trim();
  int cq1 = copsResp.indexOf('"');
  int cq2 = (cq1 >= 0) ? copsResp.indexOf('"', cq1 + 1) : -1;
  carrier = (cq1 >= 0 && cq2 > cq1) ? copsResp.substring(cq1 + 1, cq2) : copsResp;
  ts(); SerialMon.println("Carrier (AT+COPS?): " + copsResp);

  String cgregResp = sendAT("AT+CGREG?", "OK", 2000);
  cgregResp.trim();
  ts(); SerialMon.println("PS registration (AT+CGREG?): " + cgregResp);

  // ═══════════════════════════════════════════════════════════
  //  STEP 1: ESTABLISH IPV6 PDP
  // ═══════════════════════════════════════════════════════════
  printSection("STEP 1: ESTABLISH IPV6 PDP");
  SerialMon.println("Activating " + String(PDP_TYPE) + " PDP on " + String(APN_TO_USE) + " APN");

  ts(); SerialMon.println("AT+CGACT=0,1 (deactivate)...");
  sendAT("AT+CGACT=0,1", "OK", 5000);

  String cgdcontCmd = "AT+CGDCONT=1,\"" + String(PDP_TYPE) + "\",\"" + String(APN_TO_USE) + "\"";
  ts(); SerialMon.println(cgdcontCmd);
  String cgdcontResp = sendAT(cgdcontCmd, "OK", 3000);
  ts(); SerialMon.println("Response: " + cgdcontResp);

  ts(); SerialMon.println("AT+CGACT=1,1 (activate, up to 15s)...");
  String cgactResp = sendAT("AT+CGACT=1,1", "OK", 15000);
  cgactResp.trim();
  ts(); SerialMon.println("Response: " + cgactResp);

  if (cgactResp.indexOf("OK") < 0) {
    ts(); SerialMon.println("ABORT: PDP did not activate.");
    pdpFamily = "NONE";
    // Fall through to summary
  } else {
    ts(); SerialMon.println("AT+CGPADDR=1...");
    String addrResp = sendAT("AT+CGPADDR=1", "OK", 5000);
    addrResp.trim();
    ts(); SerialMon.println("CGPADDR raw: " + addrResp);
    pdpAddrRaw = addrResp;

    AddrResult ar = parseCgpaddr(addrResp);
    pdpFamily = ar.family;
    pdpAddrCanonical = ar.canonical;

    ts(); SerialMon.println("Family:    " + pdpFamily);
    ts(); SerialMon.println("Canonical: " + pdpAddrCanonical);

    if (pdpFamily == "NONE") {
      ts(); SerialMon.println("ABORT: No address obtained from PDP.");
    }
  }

  // ═══════════════════════════════════════════════════════════
  //  STEP 2: OPEN TCP/IP STACK
  // ═══════════════════════════════════════════════════════════
  if (pdpFamily != "NONE") {
    printSection("STEP 2: OPEN TCP/IP STACK");

    ts(); SerialMon.println("AT+NETSTATUS (info only)...");
    String netstat = sendAT("AT+NETSTATUS", "OK", 3000);
    netstat.trim();
    ts(); SerialMon.println("NETSTATUS: " + netstat);

    ts(); SerialMon.println("AT+NETOPEN (up to 20s)...");
    String netopenResp = sendAT("AT+NETOPEN", "OK", 20000);
    netopenResp.trim();
    ts(); SerialMon.println("NETOPEN response: " + netopenResp);

    netopenOK = (netopenResp.indexOf("OK") >= 0)
             || (netopenResp.indexOf("already opened") >= 0)
             || (netopenResp.indexOf("+NETOPEN: 0") >= 0);

    // Also wait for async +NETOPEN URC if not already in response
    if (!netopenOK) {
      ts(); SerialMon.println("Waiting 5s for +NETOPEN URC...");
      unsigned long urcWait = millis();
      String urcResp;
      while (millis() - urcWait < 5000) {
        while (SerialAT.available()) urcResp += (char)SerialAT.read();
        if (urcResp.indexOf("+NETOPEN: 0") >= 0) { netopenOK = true; break; }
        delay(20);
      }
      if (urcResp.length() > 0) {
        ts(); SerialMon.println("URC: " + urcResp);
      }
    }

    ts(); SerialMon.println("NETOPEN: " + String(netopenOK ? "OK" : "FAILED"));

    // Log stack IP address
    ts(); SerialMon.println("AT+IPADDR...");
    String ipaddr = sendAT("AT+IPADDR", "OK", 3000);
    ipaddr.trim();
    ts(); SerialMon.println("IPADDR: " + ipaddr);

    if (!netopenOK) {
      ts(); SerialMon.println("NETOPEN FAILED -- lower-level stack rejects this PDP.");
      ts(); SerialMon.println("Skipping socket tests.");
    }
  }

  // ═══════════════════════════════════════════════════════════
  //  STEP 3: RAW TCP SOCKET (CIPOPEN by HOSTNAME)
  // ═══════════════════════════════════════════════════════════
  if (pdpFamily != "NONE" && netopenOK) {
    printSection("STEP 3: RAW TCP SOCKET (CIPOPEN by HOSTNAME)");

    String cipCmd = "AT+CIPOPEN=0,\"TCP\",\"" + String(NTFY_HOSTNAME) + "\"," + String(HTTP_PORT);
    ts(); SerialMon.println(cipCmd);

    // Send and wait for +CIPOPEN URC (up to 30s)
    while (SerialAT.available()) SerialAT.read();
    SerialAT.println(cipCmd);

    unsigned long coStart = millis();
    String coResp;
    while (millis() - coStart < 30000) {
      while (SerialAT.available()) coResp += (char)SerialAT.read();
      if (coResp.indexOf("+CIPOPEN: 0,0") >= 0) { cipopenHostOK = true; break; }
      if (coResp.indexOf("+CIPOPEN: 0,") >= 0) break;  // error code
      if (coResp.indexOf("ERROR") >= 0) break;
      delay(20);
    }
    ts(); SerialMon.println("CIPOPEN response: " + coResp);
    ts(); SerialMon.println("CIPOPEN hostname: " + String(cipopenHostOK ? "OK" : "FAILED"));

    if (cipopenHostOK) {
      httpHostOK = sendHttpGetAndCapture(NTFY_HOSTNAME);
      // Close socket
      ts(); SerialMon.println("AT+CIPCLOSE=0...");
      sendAT("AT+CIPCLOSE=0", "OK", 5000);
    } else {
      ts(); SerialMon.println("CIPOPEN by hostname failed -- DNS6 lookup may not work");
    }
  }

  // ═══════════════════════════════════════════════════════════
  //  STEP 4: RAW TCP SOCKET (CIPOPEN by literal IPv6)
  // ═══════════════════════════════════════════════════════════
  if (pdpFamily != "NONE" && netopenOK) {
    printSection("STEP 4: RAW TCP SOCKET (CIPOPEN by literal IPv6)");

    String cipCmd = "AT+CIPOPEN=0,\"TCP\",\"" + String(NTFY_LITERAL_IP6) + "\"," + String(HTTP_PORT);
    ts(); SerialMon.println(cipCmd);

    while (SerialAT.available()) SerialAT.read();
    SerialAT.println(cipCmd);

    unsigned long coStart = millis();
    String coResp;
    while (millis() - coStart < 30000) {
      while (SerialAT.available()) coResp += (char)SerialAT.read();
      if (coResp.indexOf("+CIPOPEN: 0,0") >= 0) { cipopenLiteralOK = true; break; }
      if (coResp.indexOf("+CIPOPEN: 0,") >= 0) break;
      if (coResp.indexOf("ERROR") >= 0) break;
      delay(20);
    }
    ts(); SerialMon.println("CIPOPEN response: " + coResp);
    ts(); SerialMon.println("CIPOPEN IPv6 literal: " + String(cipopenLiteralOK ? "OK" : "FAILED"));

    if (cipopenLiteralOK) {
      httpLiteralOK = sendHttpGetAndCapture(NTFY_HOSTNAME);  // Host header must say ntfy.sh
      ts(); SerialMon.println("AT+CIPCLOSE=0...");
      sendAT("AT+CIPCLOSE=0", "OK", 5000);
    } else {
      ts(); SerialMon.println("CIPOPEN by IPv6 literal failed -- TCP-layer IPv6 not supported");
    }
  }

  // ═══════════════════════════════════════════════════════════
  //  STEP 5: CLEANUP
  // ═══════════════════════════════════════════════════════════
  if (pdpFamily != "NONE" && netopenOK) {
    printSection("STEP 5: CLEANUP");
    ts(); SerialMon.println("AT+NETCLOSE...");
    String ncResp = sendAT("AT+NETCLOSE", "OK", 5000);
    ts(); SerialMon.println("NETCLOSE: " + ncResp);
  }
  ts(); SerialMon.println("AT+CGACT=0,1...");
  sendAT("AT+CGACT=0,1", "OK", 5000);

  // ═══════════════════════════════════════════════════════════
  //  FINAL SUMMARY + INTERPRETATION
  // ═══════════════════════════════════════════════════════════
  SerialMon.println();
  SerialMon.println();
  SerialMon.println("================================================");
  SerialMon.println("              FINAL SUMMARY");
  SerialMon.println("================================================");
  SerialMon.println("  Carrier:                 " + carrier);
  SerialMon.println("  PDP family:              " + pdpFamily +
    (pdpAddrCanonical.length() > 0 ? " (canonical: " + pdpAddrCanonical + ")" : ""));
  SerialMon.println("  NETOPEN:                 " +
    String(pdpFamily == "NONE" ? "NOT_ATTEMPTED" : (netopenOK ? "OK" : "FAILED")));
  SerialMon.println("  CIPOPEN hostname:        " +
    String(!netopenOK ? "NOT_ATTEMPTED" : (cipopenHostOK ? "OK" : "FAILED")));
  SerialMon.println("  HTTP via hostname:       " +
    String(!cipopenHostOK ? "NOT_ATTEMPTED" : (httpHostOK ? "RESPONSE_OK" : "NO_RESPONSE")));
  SerialMon.println("  CIPOPEN IPv6 literal:    " +
    String(!netopenOK ? "NOT_ATTEMPTED" : (cipopenLiteralOK ? "OK" : "FAILED")));
  SerialMon.println("  HTTP via IPv6 literal:   " +
    String(!cipopenLiteralOK ? "NOT_ATTEMPTED" : (httpLiteralOK ? "RESPONSE_OK" : "NO_RESPONSE")));
  SerialMon.println("================================================");

  // Interpretation
  SerialMon.println();
  SerialMon.println("INTERPRETATION:");

  if (pdpFamily == "NONE") {
    SerialMon.println("  PDP did not activate. Cannot test sockets.");
    SerialMon.println("  Retry or check carrier provisioning.");
  } else if (!netopenOK) {
    SerialMon.println("  *** SpeedTalk DEFINITIVELY DEAD for data ***");
    SerialMon.println("  Modem's TCP/IP stack refuses to open over the IPv6 PDP at the");
    SerialMon.println("  NETOPEN layer. No socket-level workaround possible.");
    SerialMon.println("  Next action: order AT&T-backed MVNO SIM.");
  } else if (!cipopenHostOK && !cipopenLiteralOK) {
    SerialMon.println("  *** TCP-layer IPv6 not supported by this firmware ***");
    SerialMon.println("  NETOPEN succeeded but CIPOPEN fails on both hostname and literal.");
    SerialMon.println("  No workaround possible. Next action: AT&T MVNO.");
  } else if (cipopenLiteralOK && !cipopenHostOK && !httpLiteralOK) {
    SerialMon.println("  Socket opened but no HTTP response received. TCP works but data");
    SerialMon.println("  transport is broken (firewall, routing, MTU). Borderline case --");
    SerialMon.println("  worth one targeted retry with smaller MTU or different endpoint");
    SerialMon.println("  before giving up.");
  } else if (httpLiteralOK || httpHostOK) {
    SerialMon.println("  *** BREAKTHROUGH *** Raw TCP works over IPv6 PDP. SpeedTalk");
    SerialMon.println("  architecture is viable -- requires manual HTTP/1.1 implementation");
    SerialMon.println("  instead of HTTPINIT. Next action: design raw-socket photo upload");
    SerialMon.println("  path.");
  } else {
    SerialMon.println("  Results don't match a known pattern. See raw data above.");
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
