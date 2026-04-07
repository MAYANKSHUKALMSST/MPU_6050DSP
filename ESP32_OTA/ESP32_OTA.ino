/* ============================================================
 * ESP32_OTA.ino — Optimized for iPhone Hotspot & Linux Server
 * ============================================================ */

#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPIFFS.h>

/* ── Server Configuration (Matched to your Screenshot) ────── */
#define WIFI_SSID        "Mayank iPhone"       
#define WIFI_PASSWORD    "123456789"           

// If your server.py uses a port (like 8000 or 5000), add it here: e.g. ":8000"
#define OTA_SERVER       "http://161.118.167.196:5000"
#define VERSION_URL      OTA_SERVER "/version.json"
#define FIRMWARE_URL     OTA_SERVER "/firmware_signed.bin" 
#define FIRMWARE_PATH    "/firmware.bin"        
#define MAX_FW_SIZE      (128U * 1024U)         

/* ── UART pins for STM32 link ────────────────────────────── */
#define STM32_RX_PIN     16    
#define STM32_TX_PIN     17    
#define STM32_BAUD       115200

/* ── Protocol constants ──────────────────────────────────── */
#define UART_OTA_TRIGGER    0x55
#define UART_OTA_BOOT_CMD   0xAA
#define UART_OTA_ACK        0x06
#define UART_OTA_NAK        0x15
#define UART_OTA_CHUNK_HDR  0xC0
#define UART_OTA_DONE       0x4F
#define UART_OTA_FAIL       0x45
#define UART_OTA_REBOOT_CMD 0xBB
#define CHUNK_SIZE          256

HardwareSerial stm32(2); 
Preferences    prefs;

/* ── Helper: wait for one byte from STM32 ───────────────── */
static bool wait_byte(uint8_t expected, uint32_t timeout_ms) {
    uint32_t t0 = millis();
    while (millis() - t0 < timeout_ms) {
        if (stm32.available()) {
            uint8_t b = (uint8_t)stm32.read();
            if (b == expected) return true;
        }
        delay(1);
    }
    return false;
}

/* ── Core OTA transfer function ─────────────────────────── */
static bool do_ota_from_spiffs() {
    File f = SPIFFS.open(FIRMWARE_PATH, "r");
    if (!f) return false;

    uint32_t fw_size = (uint32_t)f.size();
    uint8_t *buf = (uint8_t *)malloc(fw_size);
    if (!buf) { f.close(); return false; }
    f.read(buf, fw_size);
    f.close();

    uint32_t fw_crc = 0;
    for (uint32_t i = 0; i < fw_size; i++) fw_crc += buf[i];

    Serial.println("[OTA] Triggering STM32...");
    stm32.flush();
    while (stm32.available()) stm32.read(); 

    uint8_t trig = UART_OTA_TRIGGER;
    stm32.write(&trig, 1);

    uint8_t hdr[12];
    hdr[0] = 0xAA; hdr[1] = 0x55; hdr[2] = 0xAA; hdr[3] = 0x55;
    memcpy(hdr + 4, &fw_size, 4);
    memcpy(hdr + 8, &fw_crc,  4);
    stm32.write(hdr, 12);
    stm32.flush();

    if (!wait_byte(UART_OTA_ACK, 5000)) { free(buf); return false; }

    uint32_t sent = 0;
    uint16_t seq = 0;
    bool ok = true;

    while (sent < fw_size && ok) {
        uint16_t clen = (uint16_t)min((uint32_t)CHUNK_SIZE, fw_size - sent);
        uint8_t xchk = 0;
        for (uint16_t i = 0; i < clen; i++) xchk ^= buf[sent + i];

        uint8_t ch[5] = { UART_OTA_CHUNK_HDR };
        memcpy(ch + 1, &seq, 2);
        memcpy(ch + 3, &clen, 2);
        
        stm32.write(ch, 5);
        stm32.write(buf + sent, clen);
        stm32.write(&xchk, 1);
        stm32.flush();

        if (wait_byte(UART_OTA_ACK, 2000)) {
            sent += clen;
            seq++;
        } else { ok = false; }
    }

    free(buf);
    if (ok && wait_byte(UART_OTA_DONE, 5000)) {
        uint8_t rb = UART_OTA_REBOOT_CMD;
        stm32.write(&rb, 1);
        return true;
    }
    return false;
}

/* ── WiFi + download (iPhone Optimized) ─────────────────── */
static void check_and_download() {
    Serial.println("\n[WiFi] Connecting to iPhone...");
    
    WiFi.disconnect(true); 
    delay(500);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 25) {
        delay(1000);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\n[WiFi] Failed. Enable 'Maximize Compatibility' on iPhone.");
        return;
    }

    Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());

    prefs.begin("ota", false);
    int stored_ver = prefs.getInt("version", 0);

    HTTPClient http;
    http.begin(VERSION_URL);
    int code = http.GET();
    if (code != 200) { http.end(); prefs.end(); return; }
    
    String payload = http.getString();
    http.end();

    // Simple JSON Parser (looking for "version": X)
    int server_ver = 0;
    int index = payload.indexOf("\"version\":");
    if (index != -1) {
        String ver_str = "";
        for (int i = index + 10; i < payload.length(); i++) {
            if (isdigit(payload[i])) ver_str += payload[i];
            else if (ver_str.length() > 0) break;
        }
        server_ver = ver_str.toInt();
    }

    if (server_ver <= stored_ver) {
        Serial.printf("[OTA] Version %d is current.\n", stored_ver);
        prefs.end();
        return;
    }

    Serial.printf("[OTA] New Update: v%d. Downloading...\n", server_ver);
    http.begin(FIRMWARE_URL);
    code = http.GET();
    if (code != 200) { http.end(); prefs.end(); return; }

    int fw_size = http.getSize();
    if (SPIFFS.exists(FIRMWARE_PATH)) SPIFFS.remove(FIRMWARE_PATH);
    File f = SPIFFS.open(FIRMWARE_PATH, "w");
    
    WiFiClient *stream = http.getStreamPtr();
    uint8_t rbuf[1024];
    int written = 0;
    
    while (http.connected() && written < fw_size) {
        int avail = stream->available();
        if (avail > 0) {
            int n = stream->readBytes(rbuf, min(avail, (int)sizeof(rbuf)));
            f.write(rbuf, n);
            written += n;
        }
        delay(1);
    }
    f.close();
    http.end();

    if (written == fw_size) {
        prefs.putInt("pending_version", server_ver);
        Serial.println("[OTA] Binary saved. Update will apply on next boot.");
    }
    prefs.end();
}

/* ── Main Setup ─────────────────────────────────────────── */
void setup() {
    Serial.begin(115200);
    Serial.println("\n=== ESP32 OTA Agent ===");

    if (!SPIFFS.begin(true)) return;
    stm32.begin(STM32_BAUD, SERIAL_8N1, STM32_RX_PIN, STM32_TX_PIN);
    delay(200);

    if (SPIFFS.exists(FIRMWARE_PATH)) {
        Serial.println("[OTA] Found cached firmware.");
        if (do_ota_from_spiffs()) {
            prefs.begin("ota", false);
            int pending = prefs.getInt("pending_version", 0);
            if (pending > 0) {
                prefs.putInt("version", pending);
                prefs.putInt("pending_version", 0);
            }
            prefs.end();
            SPIFFS.remove(FIRMWARE_PATH);
            Serial.println("[OTA] Success! Resetting STM32...");
        } else {
            Serial.println("[OTA] Flash failed. Booting STM32 normally.");
            uint8_t b = UART_OTA_BOOT_CMD;
            stm32.write(&b, 1);
        }
    } else {
        uint8_t b = UART_OTA_BOOT_CMD;
        stm32.write(&b, 1);
    }

    check_and_download();
}

void loop() { delay(1000); }