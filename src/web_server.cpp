// ============================================================================
// web_server.cpp — SoftAP + captive portal + REST API
// ============================================================================
// Endpoints:
//   GET  /                Glassmorphism Web UI (PROGMEM)
//   GET  /api/status      Live telemetry snapshot (JSON)
//   POST /api/settings    Update + persist settings (JSON)
//   POST /api/command     {"cmd":"start"|"stop"|"reboot"}
//   *    everything else  Redirect to / (captive portal behaviour)
// ============================================================================

#include "web_server.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Update.h>
#include <ctype.h>
#include "config.h"
#include "settings.h"
#include "camera_manager.h"
#include "fc_status.h"
#include "recorder.h"
#include "osd_slots.h"
#include "cam_registry.h"
#include "msp_protocol.h"
#include "scan_results.h"
#include "web_assets.h"
#include "json_scan.h"
#include "api_core.h"

// ──────────────────────────────────────────────────────────────────────────────
// Internal state
// ──────────────────────────────────────────────────────────────────────────────

static WebServer _server(80);
static DNSServer _dns;
static bool      _up = false;
static char      _ipStr[16] = "192.168.4.1";

// ──────────────────────────────────────────────────────────────────────────────
// Handlers
// ──────────────────────────────────────────────────────────────────────────────

static void handleRoot() {
    // no-store: the UI is iterated quickly — never serve a stale cached copy.
    _server.sendHeader("Cache-Control", "no-store, must-revalidate");
    _server.send_P(200, "text/html", INDEX_HTML);
}

static void handleStatus() {
    static char buf[2200];
    apiBuildStatusJson(buf, sizeof(buf));
    _server.sendHeader("Cache-Control", "no-store");
    _server.send(200, "application/json", buf);
}

static void sendJsonError(const char *msg) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    _server.send(400, "application/json", buf);
}

static void handleSettingsPost() {
    String body = _server.arg("plain");
    bool apNeedsRestart = false;
    char err[80];

    if (!apiApplySettings(body, apNeedsRestart, err, sizeof(err))) {
        sendJsonError(err);
        return;
    }

    if (apNeedsRestart) {
        _server.send(200, "application/json", "{\"ok\":true,\"apRestart\":true}");
        delay(500);
        webInit();
        return;
    }

    _server.send(200, "application/json", "{\"ok\":true}");
}
static void handleCommand() {
    String body = _server.arg("plain");
    String cmd = jsonGetStr(body, "cmd");

    bool shouldReboot = false;
    char err[40];
    if (!apiApplyCommand(cmd, shouldReboot, err, sizeof(err))) {
        sendJsonError(err);
        return;
    }

    _server.send(200, "application/json", "{\"ok\":true}");
    if (shouldReboot) {
        delay(400);
        ESP.restart();
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Saved-camera registry endpoints:  POST /api/camera {"select":i} | {"remove":i}
// ──────────────────────────────────────────────────────────────────────────────

static void handleCameraPost() {
    String body = _server.arg("plain");
    char err[80];
    bool alreadyScanning = false;

    if (!apiApplyCamera(body, alreadyScanning, err, sizeof(err))) {
        sendJsonError(err);
        return;
    }
    if (alreadyScanning) {
        _server.send(200, "application/json", "{\"ok\":true,\"already\":true}");
        return;
    }
    _server.send(200, "application/json", "{\"ok\":true}");
}

// ──────────────────────────────────────────────────────────────────────────────
// MSP passthrough — lets the Web UI talk MSP directly to the FC.
// Read-only allowlist: safe introspection commands only.  This is the
// foundation for a future browser-side "configurator-lite" panel.
// ──────────────────────────────────────────────────────────────────────────────

static bool mspCmdAllowed(uint16_t cmd) {
    static const uint16_t kAllowed[] = {
        MSP_API_VERSION,   // 1
        MSP_FC_VARIANT,    // 2
        MSP_FC_VERSION,    // 3
        MSP_BOARD_INFO,    // 4
        5,                 // MSP_BUILD_INFO
        MSP_STATUS,        // 101
        104,               // MSP_MOTOR
        MSP_RC,            // 105
        108,               // MSP_RAW_GPS? (harmless read)
        109,               // MSP_ALTITUDE
        MSP_ANALOG,        // 110
        116,               // MSP_BOXNAMES
        MSP_BOXIDS,        // 119
        130,               // MSP_BATTERY_STATE
    };
    for (size_t i = 0; i < sizeof(kAllowed) / sizeof(kAllowed[0]); i++) {
        if (kAllowed[i] == cmd) return true;
    }
    return false;
}

static void handleMspPost() {
    String body = _server.arg("plain");
    long cmd = jsonGetNum(body, "cmd", -1);

    if (cmd < 0 || cmd > 255) {
        sendJsonError("missing/invalid cmd");
        return;
    }
    if (!mspCmdAllowed((uint16_t)cmd)) {
        sendJsonError("command not in read-only allowlist");
        return;
    }

    // Flush any stale bytes, then send the request and wait for the reply.
    // Safe here: we run inside loop()'s call stack (single-threaded), and the
    // main-loop parser simply resyncs on '$' afterwards.
    while (Serial1.available()) Serial1.read();

    mspSendRequest((uint8_t)cmd);

    MspMessage msg;
    uint32_t started = millis();
    bool got = false;
    while (millis() - started < MSP_RESPONSE_TIMEOUT_MS) {
        while (Serial1.available()) {
            uint8_t b = Serial1.read();
            if (mspParseByte(b, msg)) {
                fcStatusFeed(msg);          // Keep live stats fresh
                if ((uint16_t)msg.cmd == (uint16_t)cmd && msg.valid && !msg.isError) {
                    got = true;
                    break;
                }
            }
        }
        if (got) break;
        delay(2);
        yield();
    }

    if (!got) {
        _server.send(504, "application/json",
                     "{\"ok\":false,\"error\":\"FC timeout\"}");
        return;
    }

    // Hex-encode the payload.
    static char hex[MSP_MAX_PAYLOAD_SIZE * 3 + 4];
    size_t o = 0;
    for (uint8_t i = 0; i < msg.payloadSize; i++) {
        o += snprintf(hex + o, sizeof(hex) - o, "%02X", msg.payload[i]);
    }
    hex[o] = '\0';

    char out[192];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"cmd\":%u,\"len\":%u,\"payload\":\"%s\"}",
             (unsigned)msg.cmd, (unsigned)msg.payloadSize, hex);
    _server.send(200, "application/json", out);
}

// ──────────────────────────────────────────────────────────────────────────────
// Scan Results Endpoint
// ──────────────────────────────────────────────────────────────────────────────

static void handleScanResults() {
    // Serve the REAL scan-results table (populated directly by the BLE
    // scan callbacks, sorted strongest-RSSI first) — not the cam_registry
    // discovered list, which is fed through a single-slot "newest wins"
    // funnel and drops all but the last advertiser.
    ScanResult const *sorted[MAX_SCAN_RESULTS];
    uint8_t n = scanResultsGetSortedByRssi(sorted, MAX_SCAN_RESULTS);

    static char buf[4096];
    char *p = buf;
    size_t left = sizeof(buf);

    size_t w = snprintf(p, left, "{\"scanning\":%s,\"results\":[",
                        scanResultsIsScanning() ? "true" : "false");
    p += w; left -= w;

    // Field names match what the Web UI's renderDiscovered() reads:
    // r.mac, r.n (name), r.t (type STRING), r.rssi.
    //
    // PROTOTYPE NOTE: kept as the old GoPro/"DJI" catch-all on purpose —
    // NOT changed to distinguish "DJI Osmo Nano" vs "DJI Osmo Action" here.
    // web_assets.h's renderDiscovered() hardcodes `r.t==='GoPro'?1:0` when
    // building the Pair & Save button's data-pair-type, so ANY non-GoPro
    // label — however this string reads — is saved as type 0 (Nano). If
    // this were changed to emit "DJI Osmo Action" without also updating
    // that JS, the button would show a correct-looking label while
    // silently mis-pairing an Action camera as a Nano in the registry.
    // Left generic and safe until web_assets.h gets a real 3-way picker.
    for (uint8_t i = 0; i < n && left > 96; i++) {
        if (i > 0) { *p++ = ','; left--; }

        // Sanitize device name before sending to client (prevent XSS)
        char sanitizedName[24] = "";
        sanitizeDeviceName(sanitizedName, sorted[i]->name, sizeof(sanitizedName));

        const char *typeStr = (sorted[i]->type == CAMERA_GOPRO) ? "GoPro" : "DJI";
        w = snprintf(p, left,
                     "{\"mac\":\"%s\",\"n\":\"%s\",\"t\":\"%s\",\"rssi\":%d}",
                     sorted[i]->mac, sanitizedName, typeStr, sorted[i]->rssi);
        p += w; left -= w;
    }

    // Close the array and the object — without this the JSON is invalid,
    // r.json() throws in the browser, and the Discover card stays empty.
    if (left > 2) { snprintf(p, left, "]}"); }

    _server.send(200, "application/json", buf);
}

// ──────────────────────────────────────────────────────────────────────────────
// OTA Firmware Update Endpoint
// ──────────────────────────────────────────────────────────────────────────────

static void handleOtaPost() {
    HTTPUpload& upload = _server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("OTA Start: %s\n", upload.filename.c_str());

        // Check firmware header (ESP32 magic byte)
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Serial.printf("OTA Begin Error: %s\n", Update.errorString());
            _server.sendHeader("Connection", "close");
            _server.send(500, "application/json",
                "{\"ok\":false,\"error\":\"Begin failed: " + String(Update.errorString()) + "\"}");
            return;
        }
    }
    else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Serial.printf("OTA Write Error: %s\n", Update.errorString());
            _server.sendHeader("Connection", "close");
            _server.send(500, "application/json",
                "{\"ok\":false,\"error\":\"Write failed: " + String(Update.errorString()) + "\"}");
        }
    }
    else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("OTA Success: %u bytes written\n", upload.totalSize);
            _server.send(200, "application/json",
                "{\"ok\":true,\"message\":\"Firmware updated! Rebooting...\"}");
            delay(1000);
            ESP.restart();
        } else {
            Serial.printf("OTA End Error: %s\n", Update.errorString());
            _server.sendHeader("Connection", "close");
            _server.send(500, "application/json",
                "{\"ok\":false,\"error\":\"End failed: " + String(Update.errorString()) + "\"}");
        }
    }
}

static void handleOtaStatus() {
    char out[128];
    snprintf(out, sizeof(out),
        "{\"ok\":true,\"version\":\"%s\",\"free_heap\":%lu}",
        FIRMWARE_VERSION, (unsigned long)ESP.getFreeHeap());
    _server.send(200, "application/json", out);
}

static void redirectToRoot() {
    _server.sendHeader(String("Location"), String("http://") + _ipStr + "/", true);
    _server.send(302, "text/plain", "");
}

// ── OS captive-portal probe handlers ─────────────────────────────────────────
// Each OS probes a known URL to detect "internet available".  Answering these
// with their expected SUCCESS bodies makes Windows/iOS/Android mark the
// network as usable immediately — no scary errors, no blocked popups.  Any
// other hostname still lands on our UI via the onNotFound redirect.

static const char NCSI_SUCCESS[] =
    "Microsoft Connect Test";

static void handleProbeConnectTest() {          // Windows: /connecttest.txt
    _server.send(200, "text/plain", NCSI_SUCCESS);
}

static void handleProbeNcsi() {                 // Windows legacy: /ncsi.txt
    _server.send(200, "text/plain", "Microsoft NCSI");
}

static void handleProbe204() {                  // Android: /generate_204
    _server.send(204, "text/plain", "");
}

static void handleProbeApple() {                // iOS/macOS: /hotspot-detect.html
    _server.send(200, "text/html",
                 "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
}

// ──────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ──────────────────────────────────────────────────────────────────────────────

void webStart() {
    const ShutterSettings &cfg = settingsGet();
    if (_up) return;   // Already running

    WiFi.persistent(false);              // Keep Wi-Fi creds out of NVS
    WiFi.mode(WIFI_AP);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    delay(100);

    // Deterministic AP address: 192.168.4.1/24 (matches UI + captive portal).
    IPAddress localIp(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(localIp, gateway, subnet);

    bool havePass = (strlen(cfg.apPass) >= 8);
    bool ok;
    if (havePass) {
        ok = WiFi.softAP(cfg.apSsid, cfg.apPass);
    } else {
        ok = WiFi.softAP(cfg.apSsid);   // Open network
    }
    if (!ok) {
        DBG("WEB: SoftAP failed — retrying once");
        delay(500);
        ok = havePass ? WiFi.softAP(cfg.apSsid, cfg.apPass)
                      : WiFi.softAP(cfg.apSsid);
    }
    if (!ok) {
        DBG("WEB: SoftAP failed twice!");
        _up = false;
        return;
    }

    strlcpy(_ipStr, localIp.toString().c_str(), sizeof(_ipStr));

    // Captive DNS: every hostname resolves to us.
    _dns.stop();
    _dns.setErrorReplyCode(DNSReplyCode::NoError);
    if (!_dns.start(53, "*", localIp)) {
        DBG("WEB: captive DNS failed to start");
    }

    _server.stop();
    _server.on("/",               HTTP_GET,  handleRoot);
    _server.on("/api/status",     HTTP_GET,  handleStatus);
    _server.on("/api/settings",   HTTP_POST, handleSettingsPost);
    _server.on("/api/camera",     HTTP_POST, handleCameraPost);
    _server.on("/api/command",    HTTP_POST, handleCommand);
    _server.on("/api/msp",        HTTP_POST, handleMspPost);
    _server.on("/api/scan",       HTTP_GET,  handleScanResults);
    _server.on("/api/ota",        HTTP_POST, handleOtaPost);
    _server.on("/api/ota/status", HTTP_GET,  handleOtaStatus);

    // OS connectivity probes → answer with SUCCESS bodies (see handlers above).
    _server.on("/connecttest.txt",              HTTP_GET, handleProbeConnectTest);
    _server.on("/ncsi.txt",                     HTTP_GET, handleProbeNcsi);
    _server.on("/generate_204",                 HTTP_GET, handleProbe204);
    _server.on("/gen_204",                      HTTP_GET, handleProbe204);
    _server.on("/hotspot-detect.html",          HTTP_GET, handleProbeApple);
    _server.on("/library/test/success.html",    HTTP_GET, handleProbeApple);
    // Anything else (any hostname, any path) → our UI.
    _server.onNotFound(redirectToRoot);

    _server.begin();
    _up = true;

    DBG("WEB: AP '%s' up — http://%s/ (open=%s)",
        cfg.apSsid, _ipStr, havePass ? "false" : "true");
}

void webStop() {
    if (!_up) return;
    _dns.stop();
    _server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);          // Powers down the Wi-Fi radio (BLE unaffected)
    _up = false;
    DBG("WEB: Wi-Fi powered off (radio switch)");
}

void webInit() {
    // Boot-time entry point.  Always starts the AP so the user can never be
    // locked out; a configured radio switch turns it off once RC data flows.
    webStart();
}

void webUpdate() {
    if (!_up) return;
    _dns.processNextRequest();
    _server.handleClient();
}

const char* webApIp() { return _ipStr; }
bool webIsUp()        { return _up; }