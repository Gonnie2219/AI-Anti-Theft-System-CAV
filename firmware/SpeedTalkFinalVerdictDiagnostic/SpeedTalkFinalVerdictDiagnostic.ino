/*
 * SpeedTalk FINAL Verdict Diagnostic (24h propagation test)
 *
 * Exhaustive single-flash test to produce a categorical verdict on
 * SpeedTalk TopValue T3 path viability. Run ~26h after plan upgrade
 * (past SpeedTalk's stated 24h max propagation window).
 *
 * Tests: 3x3 APN x PDP_TYPE matrix, Non-IP probe, NETOPEN, CIPOPEN,
 * raw HTTP GET. Whatever it says, we commit to the indicated next step.
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
const char* APN_LIST[]   = { "Wholesale", "mobilenet", "stkmobi" };
const int   APN_COUNT    = 3;
const char* PDP_TYPES[]  = { "IP", "IPV4V6", "IPV6" };
const int   PDP_COUNT    = 3;

const char* NTFY_LITERAL_IP4 = "159.203.148.75";
const char* NTFY_LITERAL_IP6 = "2606:4700:3034::ac43:bea2";
const int   HTTP_PORT        = 80;
const char* HTTP_PATH        = "/antitheft-gonnie-2219";

// ── Struct definitions (BEFORE any function — Arduino gotcha) ──

struct AddrResult {
  String family;     // "IPv4", "IPv6_HEX", "IPv6_DECIMAL", "DUAL", "UNKNOWN", "NONE"
  String canonical;  // standardized address string
};

// Matrix cell result
struct CellResult {
  bool   tested;
  bool   activated;    // CGACT succeeded
  String family;       // from AddrResult
  String canonical;    // from AddrResult
};

// ── Global state ─────────────────────────────────────────────
String initialOperator  = "";
bool   nonIpPdpOK       = false;
String nonIpPdpRaw      = "";

CellResult matrix[3][3]; // [apn][pdp]

String winningApn       = "";
String winningPdp       = "";
String winningAddr      = "";
String winningFamily    = "NONE";
bool   gotIPv4          = false;

bool   netopenOK        = false;
bool   cipopenOK        = false;
bool   httpResponseRecv = false;

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

bool looksLikeIPv6Hex(const String& s) {
  return s.indexOf(':') >= 0;
}

int countDots(const String& s) {
  int n = 0;
  for (int i = 0; i < (int)s.length(); i++) {
    if (s.charAt(i) == '.') n++;
  }
  return n;
}

// Convert 16 dot-separated decimal octets to canonical IPv6 hex.
String decimalOctetsToIPv6(const String& s) {
  uint8_t octets[16];
  String working = s;
  for (int i = 0; i < 16; i++) {
    int dot = working.indexOf('.');
    String part;
    if (i < 15) {
      if (dot < 0) return s;  // parse error, return raw
      part = working.substring(0, dot);
      working = working.substring(dot + 1);
    } else {
      part = working;
    }
    int val = part.toInt();
    if (val < 0 || val > 255) return s;
    octets[i] = (uint8_t)val;
  }
  char hex[40];
  snprintf(hex, sizeof(hex),
    "%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X",
    octets[0], octets[1], octets[2], octets[3],
    octets[4], octets[5], octets[6], octets[7],
    octets[8], octets[9], octets[10], octets[11],
    octets[12], octets[13], octets[14], octets[15]);
  return String(hex);
}

// ─────────────────────────────────────────────────────────────
//  parseCgpaddr — handles classic IPv4, classic IPv6 (colons),
//  dotted-decimal IPv6 (15 dots / 16 octets), dual-stack (19 dots).
// ─────────────────────────────────────────────────────────────
AddrResult parseCgpaddr(const String& resp) {
  AddrResult result;
  result.family = "NONE";
  result.canonical = "";

  int idx = resp.indexOf("+CGPADDR:");
  if (idx < 0) return result;

  int comma = resp.indexOf(',', idx);
  if (comma < 0) return result;
  String tail = resp.substring(comma + 1);

  int okIdx = tail.indexOf("\r\nOK");
  if (okIdx < 0) okIdx = tail.indexOf("\nOK");
  if (okIdx > 0) tail = tail.substring(0, okIdx);
  tail.trim();
  tail.replace("\"", "");
  tail.trim();

  if (tail.length() == 0 || tail == "0.0.0.0") return result;

  int dots = countDots(tail);
  bool hasColon = (tail.indexOf(':') >= 0);

  // Classic IPv6 with colons
  if (hasColon) {
    // Might be dual-stack: "10.x.x.x,2607:..."
    int commaPos = tail.indexOf(',');
    if (commaPos > 0) {
      String first = tail.substring(0, commaPos);
      first.trim();
      String second = tail.substring(commaPos + 1);
      second.trim();
      if (looksLikeIPv4(first) && looksLikeIPv6Hex(second)) {
        result.family = "DUAL";
        result.canonical = first + " | " + second;
        return result;
      }
    }
    // Single IPv6 hex
    result.family = "IPv6_HEX";
    result.canonical = tail;
    return result;
  }

  // Exactly 3 dots -> IPv4
  if (dots == 3) {
    result.family = "IPv4";
    result.canonical = tail;
    return result;
  }

  // 15 dots -> 16 octets -> dotted-decimal IPv6
  if (dots == 15) {
    result.family = "IPv6_DECIMAL";
    result.canonical = decimalOctetsToIPv6(tail);
    return result;
  }

  // 19 dots -> dual-stack: 4 IPv4 octets + comma + 16 IPv6 octets
  // or generally > 15 dots with a comma boundary
  if (dots >= 18) {
    // Find comma that separates two addresses
    // IPv4 portion has 3 dots, so scan for comma after first A.B.C.D
    int splitComma = -1;
    int d = 0;
    for (int i = 0; i < (int)tail.length(); i++) {
      if (tail.charAt(i) == '.') d++;
      if (tail.charAt(i) == ',' && d >= 3) { splitComma = i; break; }
    }
    if (splitComma > 0) {
      String v4part = tail.substring(0, splitComma);
      v4part.trim();
      String v6raw = tail.substring(splitComma + 1);
      v6raw.trim();
      int v6dots = countDots(v6raw);
      if (v6dots == 15) {
        String v6hex = decimalOctetsToIPv6(v6raw);
        result.family = "DUAL";
        result.canonical = v4part + " | " + v6hex;
        return result;
      }
    }
  }

  result.family = "UNKNOWN";
  result.canonical = tail;
  return result;
}

// ─────────────────────────────────────────────────────────────
//  Extract operator name from AT+COPS? response
// ─────────────────────────────────────────────────────────────
String extractOperator(const String& resp) {
  int q1 = resp.indexOf('"');
  int q2 = (q1 >= 0) ? resp.indexOf('"', q1 + 1) : -1;
  if (q1 >= 0 && q2 > q1) return resp.substring(q1 + 1, q2);
  return resp;
}

// ─────────────────────────────────────────────────────────────
//  Short label for matrix display (max 13 chars)
// ─────────────────────────────────────────────────────────────
String cellLabel(const CellResult& c) {
  if (!c.tested)    return "NotTested";
  if (!c.activated) return "FAILED";
  if (c.family == "NONE") return "FAILED";
  if (c.family == "IPv4") return "IPv4";
  if (c.family == "IPv6_HEX" || c.family == "IPv6_DECIMAL") return "IPv6";
  if (c.family == "DUAL") return "DUAL";
  return c.family;
}

// Pad string to width with trailing spaces
String pad(const String& s, int width) {
  String out = s;
  while ((int)out.length() < width) out += ' ';
  return out;
}

// =============================================================
//  SETUP — runs the entire diagnostic sequence
// =============================================================
void setup() {
  SerialMon.begin(115200);
  delay(1000);

  // Init matrix
  for (int a = 0; a < APN_COUNT; a++)
    for (int p = 0; p < PDP_COUNT; p++)
      matrix[a][p] = { false, false, "NONE", "" };

  // ═══════════════════════════════════════════════════════════
  //  STEP 0: BOOT, ID, OPERATOR CAPTURE
  // ═══════════════════════════════════════════════════════════
  printSection("STEP 0: BOOT, ID, OPERATOR CAPTURE");
  SerialMon.println("SpeedTalk FINAL Verdict Diagnostic (24h propagation test)");
  SerialMon.println("Purpose: categorical verdict on SpeedTalk path viability");
  SerialMon.println("Board:   LILYGO T-SIM7600G-H");

  // Power on modem (same sequence as AntiTheftSystemLilygo.ino)
  ts(); SerialMon.println("Powering on modem...");
  pinMode(MODEM_FLIGHT, OUTPUT); digitalWrite(MODEM_FLIGHT, HIGH);
  pinMode(MODEM_PWRKEY, OUTPUT); digitalWrite(MODEM_PWRKEY, HIGH);
  delay(300); digitalWrite(MODEM_PWRKEY, LOW);

  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  ts(); SerialMon.println("Waiting 6s for modem boot...");
  delay(6000);

  // AT handshake
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

  // Operator capture
  SerialMon.println();
  SerialMon.println("=== OPERATOR REGISTRATION (CRITICAL) ===");
  String copsResp = sendAT("AT+COPS?", "OK", 3000);
  copsResp.trim();
  SerialMon.println("  AT+COPS?:   " + copsResp);
  initialOperator = extractOperator(copsResp);

  String cgregResp = sendAT("AT+CGREG?", "OK", 2000);
  cgregResp.trim();
  SerialMon.println("  AT+CGREG?:  " + cgregResp);

  String cpsiResp = sendAT("AT+CPSI?", "OK", 2000);
  cpsiResp.trim();
  SerialMon.println("  AT+CPSI?:   " + cpsiResp);
  SerialMon.println("==========================================");

  // ═══════════════════════════════════════════════════════════
  //  STEP 1: ADVANCED DIAGNOSTIC PROBES
  // ═══════════════════════════════════════════════════════════
  printSection("STEP 1: ADVANCED DIAGNOSTIC PROBES");

  runAndPrint("AT+CGDCONT?  (PDP contexts) ", "AT+CGDCONT?", 3000);
  runAndPrint("AT+CGEQOS?   (QoS profile)  ", "AT+CGEQOS?", 3000);
  runAndPrint("AT+CIPCCFG?  (TCP/IP cfg)   ", "AT+CIPCCFG?", 3000);
  runAndPrint("AT+CIPMODE?  (TCP/IP mode)  ", "AT+CIPMODE?", 3000);
  runAndPrint("AT+CGAUTH=1,0 (clear auth)  ", "AT+CGAUTH=1,0", 3000);
  runAndPrint("AT+NETSTATUS (net state)    ", "AT+NETSTATUS", 3000);
  runAndPrint("AT+CGPADDR   (existing addr)", "AT+CGPADDR", 3000);

  // ═══════════════════════════════════════════════════════════
  //  STEP 2: NON-IP PDP PROBE
  // ═══════════════════════════════════════════════════════════
  printSection("STEP 2: NON-IP PDP PROBE");
  SerialMon.println("Testing Non-IP PDP (rules out IoT-only provisioning)");

  sendAT("AT+CGACT=0,1", "OK", 5000);

  String nonIpCgd = sendAT("AT+CGDCONT=1,\"Non-IP\",\"Wholesale\"", "OK", 3000);
  nonIpCgd.trim();
  ts(); SerialMon.println("CGDCONT Non-IP: " + nonIpCgd);

  String nonIpAct = sendAT("AT+CGACT=1,1", "OK", 15000);
  nonIpAct.trim();
  ts(); SerialMon.println("CGACT Non-IP: " + nonIpAct);
  nonIpPdpRaw = nonIpAct;
  nonIpPdpOK = (nonIpAct.indexOf("OK") >= 0);
  ts(); SerialMon.println("Non-IP PDP: " + String(nonIpPdpOK ? "SUCCEEDED" : "FAILED"));

  if (nonIpPdpOK) {
    ts(); SerialMon.println("WARNING: Non-IP succeeded -> SIM may be IoT-only provisioned");
  }

  // Deactivate
  sendAT("AT+CGACT=0,1", "OK", 5000);

  // ═══════════════════════════════════════════════════════════
  //  STEP 3: 3x3 APN x PDP_TYPE MATRIX
  // ═══════════════════════════════════════════════════════════
  printSection("STEP 3: 3x3 APN x PDP_TYPE MATRIX");

  String lastIPv6Apn  = "";
  String lastIPv6Pdp  = "";
  String lastIPv6Addr = "";
  String lastIPv6Fam  = "";

  for (int a = 0; a < APN_COUNT && !gotIPv4; a++) {
    for (int p = 0; p < PDP_COUNT && !gotIPv4; p++) {
      const char* apn     = APN_LIST[a];
      const char* pdpType = PDP_TYPES[p];

      SerialMon.println();
      SerialMon.print("--- APN=");
      SerialMon.print(apn);
      SerialMon.print(", PDP=");
      SerialMon.print(pdpType);
      SerialMon.println(" ---");

      matrix[a][p].tested = true;

      // Deactivate
      sendAT("AT+CGACT=0,1", "OK", 5000);

      // Set context
      String cgdCmd = "AT+CGDCONT=1,\"" + String(pdpType) + "\",\"" + String(apn) + "\"";
      ts(); SerialMon.println(cgdCmd);
      String cgdResp = sendAT(cgdCmd, "OK", 3000);
      ts(); SerialMon.println("Response: " + cgdResp);

      // Activate
      ts(); SerialMon.println("AT+CGACT=1,1 (up to 15s)...");
      String actResp = sendAT("AT+CGACT=1,1", "OK", 15000);
      actResp.trim();
      ts(); SerialMon.println("Response: " + actResp);

      if (actResp.indexOf("OK") < 0) {
        matrix[a][p].activated = false;
        matrix[a][p].family = "NONE";
        ts(); SerialMon.println("ACTIVATION_FAILED");
        continue;
      }

      matrix[a][p].activated = true;

      // Query address
      String addrResp = sendAT("AT+CGPADDR=1", "OK", 5000);
      addrResp.trim();
      ts(); SerialMon.println("CGPADDR: " + addrResp);

      AddrResult ar = parseCgpaddr(addrResp);
      matrix[a][p].family    = ar.family;
      matrix[a][p].canonical = ar.canonical;
      ts(); SerialMon.println("Family: " + ar.family + "  Addr: " + ar.canonical);

      // Check for IPv4 win
      if (ar.family == "IPv4" || ar.family == "DUAL") {
        gotIPv4 = true;
        winningApn    = String(apn);
        winningPdp    = String(pdpType);
        winningFamily = ar.family;
        // For DUAL, extract just the IPv4 part (before " | ")
        if (ar.family == "DUAL") {
          int sep = ar.canonical.indexOf(" | ");
          winningAddr = (sep > 0) ? ar.canonical.substring(0, sep) : ar.canonical;
        } else {
          winningAddr = ar.canonical;
        }
        SerialMon.println();
        SerialMon.println("**************************************************");
        ts(); SerialMon.println("*** IPv4 OBTAINED *** " + winningApn + "/" + winningPdp + " -> " + winningAddr);
        SerialMon.println("**************************************************");
      }

      // Track most recent IPv6 as fallback
      if (!gotIPv4 && (ar.family == "IPv6_HEX" || ar.family == "IPv6_DECIMAL")) {
        lastIPv6Apn  = String(apn);
        lastIPv6Pdp  = String(pdpType);
        lastIPv6Addr = ar.canonical;
        lastIPv6Fam  = ar.family;
      }
    }
  }

  // If no IPv4, fall back to most recent IPv6 for socket test
  if (!gotIPv4 && lastIPv6Apn.length() > 0) {
    winningApn    = lastIPv6Apn;
    winningPdp    = lastIPv6Pdp;
    winningAddr   = lastIPv6Addr;
    winningFamily = lastIPv6Fam;
    ts(); SerialMon.println("No IPv4. Using best IPv6: " + winningApn + "/" + winningPdp + " -> " + winningAddr);

    // Re-activate the winning IPv6 PDP for socket testing
    sendAT("AT+CGACT=0,1", "OK", 5000);
    sendAT("AT+CGDCONT=1,\"" + winningPdp + "\",\"" + winningApn + "\"", "OK", 3000);
    String reAct = sendAT("AT+CGACT=1,1", "OK", 15000);
    ts(); SerialMon.println("Re-activated: " + reAct);
  }

  bool haveAnyAddr = (winningAddr.length() > 0);

  // ═══════════════════════════════════════════════════════════
  //  STEP 4: TCP STACK ATTEMPT
  // ═══════════════════════════════════════════════════════════
  if (haveAnyAddr) {
    printSection("STEP 4: TCP STACK ATTEMPT");
    SerialMon.println("PDP: " + winningPdp + ", family: " + winningFamily);

    ts(); SerialMon.println("AT+NETSTATUS...");
    String ns = sendAT("AT+NETSTATUS", "OK", 3000);
    ns.trim();
    ts(); SerialMon.println("NETSTATUS: " + ns);

    ts(); SerialMon.println("AT+NETOPEN (up to 20s)...");
    String noResp = sendAT("AT+NETOPEN", "OK", 20000);
    noResp.trim();
    ts(); SerialMon.println("NETOPEN response: " + noResp);

    netopenOK = (noResp.indexOf("OK") >= 0)
             || (noResp.indexOf("already opened") >= 0)
             || (noResp.indexOf("+NETOPEN: 0") >= 0);

    // Wait for async URC if needed
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

    if (netopenOK) {
      String ipaddr = sendAT("AT+IPADDR", "OK", 3000);
      ipaddr.trim();
      ts(); SerialMon.println("IPADDR: " + ipaddr);
    } else {
      ts(); SerialMon.println("NETOPEN FAILED -- TCP/IP stack rejects this PDP.");
    }
  } else {
    printSection("STEP 4: TCP STACK ATTEMPT (SKIPPED)");
    SerialMon.println("No address obtained from matrix. Skipping.");
  }

  // ═══════════════════════════════════════════════════════════
  //  STEP 5: SOCKET + HTTP TEST
  // ═══════════════════════════════════════════════════════════
  if (haveAnyAddr && netopenOK) {
    printSection("STEP 5: SOCKET + HTTP TEST");

    // Choose target IP based on family
    String targetIP;
    if (gotIPv4 || winningFamily == "IPv4" || winningFamily == "DUAL") {
      targetIP = String(NTFY_LITERAL_IP4);
      ts(); SerialMon.println("Using IPv4 target: " + targetIP);
    } else {
      targetIP = String(NTFY_LITERAL_IP6);
      ts(); SerialMon.println("Using IPv6 target: " + targetIP);
    }

    String cipCmd = "AT+CIPOPEN=0,\"TCP\",\"" + targetIP + "\"," + String(HTTP_PORT);
    ts(); SerialMon.println(cipCmd);

    while (SerialAT.available()) SerialAT.read();
    SerialAT.println(cipCmd);

    unsigned long coStart = millis();
    String coResp;
    while (millis() - coStart < 30000) {
      while (SerialAT.available()) coResp += (char)SerialAT.read();
      if (coResp.indexOf("+CIPOPEN: 0,0") >= 0) { cipopenOK = true; break; }
      if (coResp.indexOf("+CIPOPEN: 0,") >= 0) break;
      if (coResp.indexOf("ERROR") >= 0) break;
      delay(20);
    }
    ts(); SerialMon.println("CIPOPEN response: " + coResp);
    ts(); SerialMon.println("CIPOPEN: " + String(cipopenOK ? "OK" : "FAILED"));

    if (cipopenOK) {
      // Build HTTP GET
      String req = "GET " + String(HTTP_PATH) + " HTTP/1.1\r\n"
                   "Host: ntfy.sh\r\n"
                   "User-Agent: SpeedTalkFinalTest\r\n"
                   "Connection: close\r\n\r\n";
      int reqLen = req.length();
      ts(); SerialMon.println("Sending " + String(reqLen) + " byte HTTP GET...");

      while (SerialAT.available()) SerialAT.read();
      SerialAT.println("AT+CIPSEND=0," + String(reqLen));

      // Wait for ">" prompt
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
      } else {
        ts(); SerialMon.println("Got '>' prompt, writing request...");
        SerialAT.print(req);

        // Wait for CIPSEND confirmation
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

        if (sendOK) {
          // Wait for HTTP response data
          ts(); SerialMon.println("Waiting for HTTP response (up to 20s)...");
          unsigned long rStart = millis();
          String rResp;
          while (millis() - rStart < 20000) {
            while (SerialAT.available()) rResp += (char)SerialAT.read();
            if (rResp.length() > 300) break;
            if (rResp.indexOf("+IPCLOSE:") >= 0) break;
            if (rResp.indexOf("CLOSED") >= 0) break;
            delay(20);
          }

          SerialMon.println();
          SerialMon.println("--- Received data (first 300 chars) ---");
          if ((int)rResp.length() > 300) {
            SerialMon.println(rResp.substring(0, 300));
            SerialMon.println("... (" + String(rResp.length()) + " total bytes)");
          } else {
            SerialMon.println(rResp);
          }
          SerialMon.println("--- End received data ---");

          httpResponseRecv = (rResp.indexOf("HTTP/1.") >= 0 || rResp.indexOf("HTTP/2") >= 0);
          ts(); SerialMon.println(httpResponseRecv ? "HTTP response detected!" : "No HTTP response detected.");
        }
      }

      // Close socket
      ts(); SerialMon.println("AT+CIPCLOSE=0...");
      sendAT("AT+CIPCLOSE=0", "OK", 5000);
    } else {
      ts(); SerialMon.println("CIPOPEN failed -- TCP layer rejects PDP regardless of address family");
    }
  } else if (haveAnyAddr && !netopenOK) {
    printSection("STEP 5: SOCKET + HTTP TEST (SKIPPED)");
    SerialMon.println("NETOPEN failed. Cannot test sockets.");
  } else {
    printSection("STEP 5: SOCKET + HTTP TEST (SKIPPED)");
    SerialMon.println("No address from matrix. Cannot test sockets.");
  }

  // ═══════════════════════════════════════════════════════════
  //  STEP 6: CLEANUP
  // ═══════════════════════════════════════════════════════════
  printSection("STEP 6: CLEANUP");
  ts(); SerialMon.println("AT+NETCLOSE...");
  sendAT("AT+NETCLOSE", "OK", 5000);
  ts(); SerialMon.println("AT+CGACT=0,1...");
  sendAT("AT+CGACT=0,1", "OK", 5000);

  // ═══════════════════════════════════════════════════════════
  //  FINAL VERDICT
  // ═══════════════════════════════════════════════════════════
  SerialMon.println();
  SerialMon.println();
  SerialMon.println("============== FINAL VERDICT ==============");
  SerialMon.println("  Test time:               hour ~26 post plan change");
  SerialMon.println("  Initial operator:        " + initialOperator);

  bool operatorChanged = (initialOperator.indexOf("STKMOBI") < 0
                       && initialOperator.indexOf("stkmobi") < 0);
  SerialMon.println("  Operator changed to");
  SerialMon.println("    something other than STKMOBI? " + String(operatorChanged ? "YES" : "NO"));
  SerialMon.println("  Non-IP PDP:              " + String(nonIpPdpOK ? "SUCCEEDED" : "FAILED"));

  // Matrix table
  SerialMon.println();
  SerialMon.println("  3x3 Matrix Results:");
  SerialMon.print("                ");
  for (int p = 0; p < PDP_COUNT; p++) {
    SerialMon.print(pad(String(PDP_TYPES[p]), 14));
  }
  SerialMon.println();
  for (int a = 0; a < APN_COUNT; a++) {
    SerialMon.print("  ");
    SerialMon.print(pad(String(APN_LIST[a]), 14));
    for (int p = 0; p < PDP_COUNT; p++) {
      SerialMon.print(pad(cellLabel(matrix[a][p]), 14));
    }
    SerialMon.println();
  }

  SerialMon.println();
  SerialMon.println("  Best PDP:                " +
    (winningApn.length() > 0 ? winningApn + "/" + winningPdp + " -> " + winningAddr : "NONE"));
  SerialMon.println("  NETOPEN:                 " +
    String(!haveAnyAddr ? "NOT_ATTEMPTED" : (netopenOK ? "OK" : "FAILED")));
  SerialMon.println("  CIPOPEN:                 " +
    String(!netopenOK ? "NOT_ATTEMPTED" : (cipopenOK ? "OK" : "FAILED")));
  SerialMon.println("  HTTP response received:  " +
    String(!cipopenOK ? "NOT_ATTEMPTED" : (httpResponseRecv ? "YES" : "NO")));
  SerialMon.println("===========================================");

  // ── VERDICT ────────────────────────────────────────────────
  SerialMon.println();
  SerialMon.println("VERDICT:");
  SerialMon.println();

  // E) Non-IP succeeded but no IP PDP worked
  if (nonIpPdpOK && !gotIPv4 && winningFamily != "IPv4" && winningFamily != "DUAL") {
    SerialMon.println("  SIM is provisioned for Non-IP IoT data only. TCP/IP is");
    SerialMon.println("  structurally impossible on this SIM regardless of plan tier.");
    SerialMon.println("  SpeedTalk closed.");
    SerialMon.println("  Next action: AT&T MVNO.");
  }
  // A) IPv4 obtained and HTTP works
  else if (gotIPv4 && httpResponseRecv) {
    SerialMon.println("  *** BREAKTHROUGH *** SpeedTalk works.");
    SerialMon.println("  Architecture decision: keep SpeedTalk, replace Hologram, gut Twilio.");
    SerialMon.println("  Winning config: APN=" + winningApn + ", PDP=" + winningPdp + ", family=IPv4");
    SerialMon.println("  Address: " + winningAddr);
    SerialMon.println("  Next action: update production firmware to use this config + ntfy.sh");
    SerialMon.println("  direct over Wholesale APN. Begin Twilio bridge removal.");
  }
  // B) IPv4 obtained but HTTP failed
  else if (gotIPv4 && !httpResponseRecv) {
    SerialMon.println("  PARTIAL: IPv4 obtained but HTTP transport failed. Possible firewall");
    SerialMon.println("  or routing issue. Worth one targeted retry against a different");
    SerialMon.println("  endpoint before deciding.");
  }
  // C) Operator changed from STKMOBI but no IPv4
  else if (operatorChanged && !gotIPv4) {
    SerialMon.println("  Operator unlocked from STKMOBI but data still IPv6-only / TCP fails.");
    SerialMon.println("  Carrier provisioning DID propagate but landed in a state our modem");
    SerialMon.println("  still can't use. SpeedTalk closed.");
    SerialMon.println("  Next action: order AT&T-backed MVNO SIM.");
  }
  // D) Still STKMOBI and no IPv4
  else if (!operatorChanged && !gotIPv4) {
    SerialMon.println("  *** SPEEDTALK DEFINITIVELY CLOSED ***");
    SerialMon.println("  After 26h propagation window, operator remains locked to STKMOBI");
    SerialMon.println("  and no APN/PDP combination yields a usable data PDP. This is the");
    SerialMon.println("  final SpeedTalk result. Six independent diagnostics across two days");
    SerialMon.println("  all converge on the same conclusion.");
    SerialMon.println("  Next action: order AT&T-backed MVNO SIM (Red Pocket, H2O, US Mobile");
    SerialMon.println("  Warp on AT&T, or PureTalk). Same diagnostic sketches can be reused");
    SerialMon.println("  on the new SIM with only APN/IMSI constants updated.");
  }
  // F) Catch-all
  else {
    SerialMon.println("  Inconclusive: modem returned unexpected results. Possible hardware");
    SerialMon.println("  regression or carrier-side outage. Recommend power cycle and one");
    SerialMon.println("  retry; if same result, treat as Verdict D.");
  }

  SerialMon.println();
  SerialMon.println("===========================================");
  ts(); SerialMon.println("Diagnostic complete. Output is stable.");
  SerialMon.println("===========================================");
}

// ─────────────────────────────────────────────────────────────
//  Loop — do nothing, output stands still for easy capture
// ─────────────────────────────────────────────────────────────
void loop() {
  delay(1000);
}
