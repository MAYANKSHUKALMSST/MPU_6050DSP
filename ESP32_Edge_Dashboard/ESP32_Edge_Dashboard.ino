/* ============================================================
 * ESP32_Edge_Dashboard.ino — Full Dashboard + OTA + Cloud Forward
 * ============================================================ */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include "mbedtls/md.h"

#include "dashboard_html.h"

WebServer server(80);
DNSServer dnsServer;
Preferences prefs;
bool apMode = false;

String telemetry = "{\"ax\":0,\"ay\":0,\"az\":1,\"temp\":25,\"peak\":0,\"freq\":0,\"fault\":0}";

/* ── Cloud Dashboard (Oracle Server) ──────────────────────── */
#define CLOUD_SERVER_IP   "161.118.167.196"
#define CLOUD_SERVER_PORT 3001
#define CLOUD_HTTP_PORT   3000
#define DEVICE_SECRET     "stm32device2024"   // must match server.js

WiFiClient cloudClient;
static uint32_t lastCloudReconnect = 0;

/* Connect (or reconnect) to Oracle server.
   Called lazily — only when there is data to send. */
static bool cloudEnsureConnected() {
  if (cloudClient.connected()) return true;
  if (millis() - lastCloudReconnect < 5000) return false; // back-off 5 s
  lastCloudReconnect = millis();
  Serial.println("[CLOUD] Connecting to " CLOUD_SERVER_IP ":" + String(CLOUD_SERVER_PORT) + "...");
  if (cloudClient.connect(CLOUD_SERVER_IP, CLOUD_SERVER_PORT)) {
    Serial.println("[CLOUD] Connected OK");
    return true;
  }
  Serial.println("[CLOUD] Connection failed — will retry in 5 s");
  return false;
}

/* Forward a JSON telemetry line to the Oracle server. */
static void cloudForward(const String &json) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!cloudEnsureConnected()) return;
  cloudClient.println(json); // \n is the record delimiter server.js expects
}

/* Send a log event to the Oracle server dashboard.
   Format: LOG:LEVEL:message — parsed by server.js TCP handler. */
static void cloudLog(const char *level, const String &msg) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!cloudEnsureConnected()) return;
  cloudClient.println(String("LOG:") + level + ":" + msg);
  Serial.println(String("[DEVLOG] ") + level + ": " + msg);
}

/* ── OTA State — declared before cloudFetchAndFlash so it can use them ── */
String otaState = "idle";
int    otaProg  = 0;
String otaMsg   = "Ready";

/* Download firmware from server into SPIFFS, then call doFlash() */
static void cloudFetchAndFlash() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[OTA] No WiFi — cannot download firmware");
    return;
  }

  String url = "http://" CLOUD_SERVER_IP ":" + String(CLOUD_HTTP_PORT) +
               "/firmware/update.bin?secret=" DEVICE_SECRET;
  Serial.println("[OTA] Downloading firmware from: " + url);

  HTTPClient http;
  http.begin(url);
  http.setTimeout(30000);
  int code = http.GET();

  if (code != 200) {
    Serial.printf("[OTA] HTTP error %d — aborting\n", code);
    cloudLog("ERROR", "OTA download HTTP error: " + String(code));
    http.end();
    return;
  }

  int totalSize = http.getSize();
  Serial.printf("[OTA] Firmware size: %d bytes\n", totalSize);

  if (SPIFFS.exists("/update.bin")) SPIFFS.remove("/update.bin");
  File f = SPIFFS.open("/update.bin", FILE_WRITE);
  if (!f) { Serial.println("[OTA] SPIFFS open failed"); http.end(); return; }

  WiFiClient *stream = http.getStreamPtr();
  uint8_t  buf[512];
  int received = 0;

  while (http.connected() && (totalSize < 0 || received < totalSize)) {
    size_t avail = stream->available();
    if (avail) {
      size_t n = stream->readBytes(buf, min(avail, sizeof(buf)));
      f.write(buf, n);
      received += n;
      if (received % (16 * 1024) == 0)
        Serial.printf("[OTA] Downloaded %d / %d bytes\n", received, totalSize);
    }
    delay(1);
  }
  f.close();
  http.end();

  if (received > 0 && (totalSize < 0 || received >= totalSize)) {
    Serial.printf("[OTA] Download complete: %d bytes — starting flash\n", received);
    cloudLog("OTA", "Firmware downloaded (" + String(received) + " bytes) — flashing STM32");
    doFlash();   // existing SPIFFS → STM32 flash routine
    // Report result back to server after doFlash() returns
    if (otaState == "done") {
      if (cloudEnsureConnected()) cloudClient.println("FLASH_OK");
      cloudLog("OTA", "Flash complete — STM32 rebooted with new firmware");
    } else {
      if (cloudEnsureConnected()) cloudClient.println("FLASH_FAIL:" + otaMsg);
      cloudLog("ERROR", "Flash failed: " + otaMsg);
    }
  } else {
    Serial.printf("[OTA] Incomplete download (%d / %d) — aborting\n", received, totalSize);
    cloudLog("ERROR", "OTA download incomplete: " + String(received) + "/" + String(totalSize) + " bytes");
    SPIFFS.remove("/update.bin");
  }
}

/* ── OTA Constants (match bootloader) ─────────────────────── */
#define OTA_TRIGGER    0x55
#define OTA_BOOT_CMD   0xAA
#define OTA_ACK        0x06
#define OTA_CHUNK_HDR  0xC0
#define OTA_DONE_BYTE  0x4F
#define OTA_REBOOT_CMD 0xBB
#define CHUNK_SZ       256

/* ── OTA State (declared here — used by cloudFetchAndFlash above) ── */
// (moved to top — see declaration before cloudFetchAndFlash)
File upFile;
bool otaGo = false;

/* ── wait for a byte ──────────────────────────────────────── */
bool waitB(uint8_t want, uint32_t ms) {
  uint32_t t = millis();
  while (millis() - t < ms) {
    if (Serial2.available()) {
      if ((uint8_t)Serial2.read() == want) return true;
    }
    delay(1);
  }
  return false;
}

/* ── Flash firmware via bootloader protocol ───────────────── */
void doFlash() {
  File f = SPIFFS.open("/update.bin", "r");
  if (!f) { otaMsg = "No firmware file"; otaState = "error"; return; }
  uint32_t fwsz = f.size();
  Serial.printf("[OTA] Size: %lu bytes, Heap: %lu\n", (unsigned long)fwsz, (unsigned long)ESP.getFreeHeap());

  // ── Pass 1: Compute SHA-256 by streaming from file ────────
  otaState = "flashing"; otaProg = 2; otaMsg = "Computing hash...";
  uint8_t fw_hash[32];
  uint8_t chunk_buf[CHUNK_SZ];
  mbedtls_md_context_t md;
  mbedtls_md_init(&md);
  mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
  mbedtls_md_starts(&md);
  while (f.available()) {
    size_t n = f.read(chunk_buf, CHUNK_SZ);
    mbedtls_md_update(&md, chunk_buf, n);
  }
  mbedtls_md_finish(&md, fw_hash);
  mbedtls_md_free(&md);
  f.close();

  Serial.print("[OTA] SHA-256: ");
  for (int i = 0; i < 32; i++) Serial.printf("%02x", fw_hash[i]);
  Serial.println();

  // ── Step 1: Reboot STM32 into bootloader ──────────────────
  otaProg = 5; otaMsg = "Resetting STM32...";

  Serial.println("[OTA] Checking UART link to STM32...");
  int rxCount = 0;
  uint32_t linkCheck = millis();
  while (millis() - linkCheck < 1000) {
    if (Serial2.available()) {
      char c = Serial2.read();
      rxCount++;
      if (rxCount <= 20) Serial.printf("[OTA] RX byte: 0x%02X '%c'\n", (uint8_t)c, (c >= 32 && c < 127) ? c : '.');
    }
    delay(1);
  }
  Serial.printf("[OTA] UART link check: received %d bytes in 1s\n", rxCount);

  if (rxCount > 0) {
    Serial.println("[OTA] STM32 app detected — sending REBOOT commands...");
    for (int r = 0; r < 8; r++) {
      while (Serial2.available()) Serial2.read();
      Serial2.print("REBOOT\n"); Serial2.flush();
      Serial.printf("[OTA] REBOOT attempt %d/8\n", r + 1);
      delay(500);
    }
    Serial.println("[OTA] Waiting for bootloader startup...");
    delay(1500);
  } else {
    Serial.println("[OTA] WARNING: No data from STM32 — may already be in bootloader");
    delay(500);
  }

  int drained = 0;
  while (Serial2.available()) { Serial2.read(); drained++; }
  Serial.printf("[OTA] Drained %d stale bytes\n", drained);

  // ── Step 2: Try trigger up to 5 times ─────────────────────
  otaProg = 10; otaMsg = "Triggering bootloader...";
  cloudLog("OTA", "Progress: 10% — triggering bootloader");
  bool triggered = false;

  for (int attempt = 0; attempt < 5 && !triggered; attempt++) {
    Serial.printf("[OTA] Trigger attempt %d/5...\n", attempt + 1);
    while (Serial2.available()) Serial2.read();

    uint8_t trig = OTA_TRIGGER;
    Serial2.write(&trig, 1); Serial2.flush();
    delay(10);

    uint8_t hdr[40];
    hdr[0]=0xAA; hdr[1]=0x55; hdr[2]=0xAA; hdr[3]=0x55;
    memcpy(hdr+4, &fwsz, 4);
    memcpy(hdr+8, fw_hash, 32);
    Serial2.write(hdr, 40); Serial2.flush();

    otaProg = 15; otaMsg = "Erasing Slot B...";
    if (waitB(OTA_ACK, 12000)) {
      triggered = true;
      cloudLog("OTA", "Progress: 18% — bootloader ACK, Slot B erased");
      Serial.println("[OTA] Bootloader ACK received!");
    } else {
      Serial.println("[OTA] No ACK, retrying...");
      delay(500);
    }
  }

  if (!triggered) {
    otaMsg = "Bootloader not responding (check STM32 wiring)";
    otaState = "error";
    return;
  }

  // ── Step 5: Stream chunks from SPIFFS (pass 2) ────────────
  f = SPIFFS.open("/update.bin", "r");
  if (!f) { otaMsg = "File read error"; otaState = "error"; return; }

  uint32_t sent = 0; uint16_t seq = 0;
  uint32_t lastProgressLog = 0;  // track bytes at last cloudLog to avoid flooding
  while (sent < fwsz) {
    uint16_t clen = min((uint32_t)CHUNK_SZ, fwsz - sent);
    f.read(chunk_buf, clen);

    uint8_t xor_chk = 0;
    for (uint16_t i = 0; i < clen; i++) xor_chk ^= chunk_buf[i];

    uint8_t ch[5] = {OTA_CHUNK_HDR};
    memcpy(ch+1, &seq, 2); memcpy(ch+3, &clen, 2);
    Serial2.write(ch, 5);
    Serial2.write(chunk_buf, clen);
    Serial2.write(&xor_chk, 1);
    Serial2.flush();

    if (!waitB(OTA_ACK, 3000)) {
      f.close();
      otaMsg = "Chunk " + String(seq) + " failed"; otaState = "error"; return;
    }
    sent += clen; seq++;
    otaProg = 20 + (int)(sent * 70UL / fwsz);
    otaMsg = "Flashing " + String(otaProg) + "%";
    server.handleClient();

    // Report progress to cloud every 16 KB to keep dashboard live
    if (sent - lastProgressLog >= 16384 || sent == fwsz) {
      cloudLog("OTA", "Progress: " + String(otaProg) + "% — " +
               String(sent / 1024) + "/" + String(fwsz / 1024) + " KB");
      lastProgressLog = sent;
    }
  }
  f.close();

  // ── Step 6: Verify ────────────────────────────────────────
  otaProg = 95; otaMsg = "Verifying SHA-256...";
  cloudLog("OTA", "Progress: 95% — verifying SHA-256");
  if (!waitB(OTA_DONE_BYTE, 10000)) {
    otaMsg = "Verification failed"; otaState = "error"; return;
  }

  uint8_t rb = OTA_REBOOT_CMD;
  Serial2.write(&rb, 1);
  SPIFFS.remove("/update.bin");
  otaState = "done"; otaProg = 100; otaMsg = "Success! STM32 rebooting.";
  cloudLog("OTA", "Progress: 100% — flash complete, STM32 rebooting");
  Serial.println("[OTA] SUCCESS");
}

/* ============================================================ */
void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, 16, 17);
  Serial2.setTimeout(50);
  delay(1500); // Let power supply stabilize on cold boot

  Serial.println("\n=== ESP32 Edge Dashboard ===");

  if (!SPIFFS.begin(true)) Serial.println("[SPIFFS] FAIL");
  else Serial.println("[SPIFFS] OK");

  uint8_t bc = OTA_BOOT_CMD;
  Serial2.write(&bc, 1);

  prefs.begin("wifi_creds", false);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");

  if (ssid.length() > 0) {
    Serial.println("[WiFi] Connecting to: " + ssid);
    // Reset Wi-Fi baseband to clear dirty state from cold boot
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) {
      delay(500); Serial.print(".");
    }
    Serial.println();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] IP: " + WiFi.localIP().toString());
    if (MDNS.begin("stm32")) {
      MDNS.addService("http", "tcp", 80);
      Serial.println("[mDNS] Dashboard: http://stm32.local");
    }
    Serial.println("[CLOUD] Will forward telemetry to " CLOUD_SERVER_IP ":" + String(CLOUD_SERVER_PORT));
    // Log boot event to cloud after WiFi is up
    cloudLog("INFO", "ESP32 boot — WiFi IP: " + WiFi.localIP().toString() +
             "  FW: " __DATE__ " " __TIME__);
  } else {
    Serial.println("[WiFi] AP Mode");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("STM32_Edge_AP");
    delay(200);
    dnsServer.start(53, "*", WiFi.softAPIP());
    apMode = true;
    Serial.println("[AP] SSID: STM32_Edge_AP");
    Serial.println("[CLOUD] NOTE: Cloud forwarding disabled in AP mode (no internet)");
  }

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", index_html);
  });

  server.on("/api/tele", HTTP_GET, []() {
    server.send(200, "application/json", telemetry);
  });

  server.on("/api/ota_status", HTTP_GET, []() {
    server.send(200, "application/json",
      "{\"state\":\"" + otaState + "\",\"progress\":" + String(otaProg) +
      ",\"message\":\"" + otaMsg + "\"}");
  });

  server.on("/api/wifi", HTTP_POST, []() {
    String s = server.arg("s"), p = server.arg("p");
    if (s.length() > 0) {
      prefs.putString("ssid", s);
      prefs.putString("pass", p);
      server.send(200, "text/plain", "OK");
      delay(1000);
      ESP.restart();
    }
  });

  server.on("/api/upload", HTTP_POST,
    []() { server.send(200, "text/plain", "OK"); otaGo = true; },
    []() {
      HTTPUpload &u = server.upload();
      if (u.status == UPLOAD_FILE_START) {
        if (SPIFFS.exists("/update.bin")) SPIFFS.remove("/update.bin");
        upFile = SPIFFS.open("/update.bin", FILE_WRITE);
      } else if (u.status == UPLOAD_FILE_WRITE && upFile) {
        upFile.write(u.buf, u.currentSize);
      } else if (u.status == UPLOAD_FILE_END && upFile) {
        upFile.close();
        Serial.printf("[OTA] Saved %u bytes\n", u.totalSize);
      }
    }
  );

  server.on("/generate_204", HTTP_GET, []() { server.send(200, "text/html", index_html); });
  server.on("/fwlink", HTTP_GET, []() { server.send(200, "text/html", index_html); });
  server.onNotFound([]() { server.send(200, "text/html", index_html); });

  server.begin();
  Serial.println("[HTTP] Server ready");
}

/* ============================================================ */
static uint32_t lastTeleLog = 0;
static uint32_t teleCount = 0;

void loop() {
  if (apMode) dnsServer.processNextRequest();
  server.handleClient();

  if (otaGo) { otaGo = false; doFlash(); }

  // ── Read commands sent back from Oracle server ─────────────
  if (cloudClient.connected() && cloudClient.available() && otaState != "flashing") {
    String cmd = cloudClient.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() > 0) {
      Serial.println("[CLOUD] Command received: " + cmd);
      if (cmd == "UPDATE") {
        cloudFetchAndFlash();               // download from server, flash STM32
      } else if (cmd == "REBOOT") {
        Serial2.print("REBOOT\n");          // forward to STM32
        Serial2.flush();
        Serial.println("[CLOUD] REBOOT forwarded to STM32");
      }
    }
  }

  // Telemetry from STM32 — update local cache AND forward to cloud
  if (Serial2.available() && otaState != "flashing") {
    String line = Serial2.readStringUntil('\n');
    line.trim();
    if (line.length() > 2 && line[0] == '{' && line[line.length()-1] == '}') {
      telemetry = line;
      teleCount++;
      cloudForward(line);  // ← forward to Oracle server
    }
  }

  // Log telemetry + cloud status every 10 seconds
  if (millis() - lastTeleLog > 10000) {
    Serial.printf("[TEL] Packets: %lu | Last: %s | Cloud: %s\n",
      teleCount,
      teleCount > 0 ? "OK" : "NONE — check STM32 UART wiring",
      cloudClient.connected() ? "CONNECTED" : "disconnected");
    lastTeleLog = millis();
  }
}
