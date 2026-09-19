// ============================================================================
// serial_config.cpp — Web Serial bench config protocol
// ============================================================================
// A line-oriented JSON protocol over the same USB serial port used for
// debug logging (Serial, 115200 baud) and flashing. Mirrors the Wi-Fi REST
// API's configuration surface (api_core.cpp) so the bench tool
// (docs/index.html, served over GitHub Pages via Web Serial) and the
// SoftAP field Web UI can never drift out of sync with each other.
//
// Wire format — one JSON object per line, both directions:
//   Host -> device:  {"path":"ping"}
//                     {"path":"status"}
//                     {"path":"settings", ...same fields /api/settings takes}
//                     {"path":"camera",   ...same fields /api/camera takes}
//                     {"path":"command","cmd":"start"|"stop"|"reboot"}
//                     {"path":"msp","cmd":<u8>}
//   Device -> host:   one JSON line per response, e.g. {"ok":true} or the
//                     full status object. DBG() debug lines are ALSO still
//                     printed to this same port but never start with '{',
//                     so a host-side reader tells them apart trivially.
//
// Only one request is expected in flight at a time — the browser sends a
// command and awaits the next '{'-prefixed line before sending another.
//
// The MSP passthrough here is intentionally UNrestricted, unlike /api/msp's
// read-only allowlist — this channel requires a physical USB cable, a much
// higher trust bar than the (possibly open) Wi-Fi AP.
// ============================================================================

#include "serial_config.h"
#include "config.h"
#include "settings.h"
#include "api_core.h"
#include "json_scan.h"
#include "msp_protocol.h"
#include "fc_status.h"
#include "web_server.h"   // webInit()

// ──────────────────────────────────────────────────────────────────────────────
// Line buffering
// ──────────────────────────────────────────────────────────────────────────────

static char   _lineBuf[512];
static size_t _lineLen = 0;

// ──────────────────────────────────────────────────────────────────────────────
// Response helpers
// ──────────────────────────────────────────────────────────────────────────────

static void sendOk() {
    Serial.println("{\"ok\":true}");
}

static void sendOkExtra(const char *extraJsonFields) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ok\":true,%s}", extraJsonFields);
    Serial.println(buf);
}

static void sendErr(const char *msg) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    Serial.println(buf);
}

// ──────────────────────────────────────────────────────────────────────────────
// MSP passthrough
// ──────────────────────────────────────────────────────────────────────────────

static void handleMsp(const String &line) {
    long cmd = jsonGetNum(line, "cmd", -1);
    if (cmd < 0 || cmd > 255) { sendErr("missing/invalid cmd"); return; }

    while (Serial1.available()) Serial1.read();
    mspSendRequest((uint8_t)cmd);

    MspMessage msg;
    uint32_t started = millis();
    bool got = false;
    while (millis() - started < MSP_RESPONSE_TIMEOUT_MS) {
        while (Serial1.available()) {
            uint8_t b = Serial1.read();
            if (mspParseByte(b, msg)) {
                fcStatusFeed(msg);
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

    if (!got) { sendErr("FC timeout"); return; }

    static char hex[MSP_MAX_PAYLOAD_SIZE * 2 + 1];
    size_t o = 0;
    for (uint8_t i = 0; i < msg.payloadSize; i++) {
        o += snprintf(hex + o, sizeof(hex) - o, "%02X", msg.payload[i]);
    }
    hex[o] = '\0';

    char out[192];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"cmd\":%u,\"len\":%u,\"payload\":\"%s\"}",
             (unsigned)msg.cmd, (unsigned)msg.payloadSize, hex);
    Serial.println(out);
}

// ──────────────────────────────────────────────────────────────────────────────
// Dispatch one complete command line
// ──────────────────────────────────────────────────────────────────────────────

static void dispatchLine(const String &line) {
    String path = jsonGetStr(line, "path");

    if (path == "ping") {
        char out[96];
        snprintf(out, sizeof(out), "{\"ok\":true,\"device\":\"shutterlink\",\"fw\":\"%s\"}", FIRMWARE_VERSION);
        Serial.println(out);

    } else if (path == "status") {
        static char buf[2200];
        apiBuildStatusJson(buf, sizeof(buf));
        Serial.println(buf);

    } else if (path == "settings") {
        bool apNeedsRestart = false;
        char err[80];
        if (!apiApplySettings(line, apNeedsRestart, err, sizeof(err))) {
            sendErr(err);
        } else if (apNeedsRestart) {
            sendOkExtra("\"apRestart\":true");
            webInit();
        } else {
            sendOk();
        }

    } else if (path == "camera") {
        bool alreadyScanning = false;
        char err[80];
        if (!apiApplyCamera(line, alreadyScanning, err, sizeof(err))) {
            sendErr(err);
        } else if (alreadyScanning) {
            sendOkExtra("\"already\":true");
        } else {
            sendOk();
        }

    } else if (path == "command") {
        String cmd = jsonGetStr(line, "cmd");
        bool shouldReboot = false;
        char err[40];
        if (!apiApplyCommand(cmd, shouldReboot, err, sizeof(err))) {
            sendErr(err);
        } else {
            sendOk();
            if (shouldReboot) {
                delay(400);
                ESP.restart();
            }
        }

    } else if (path == "msp") {
        handleMsp(line);

    } else {
        sendErr("unknown path");
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────────────────────────────────────

void serialConfigInit() {
    _lineLen = 0;
}

void serialConfigUpdate() {
    while (Serial.available()) {
        char c = (char)Serial.read();f

        if (c == '\n' || c == '\r') {
            if (_lineLen > 0) {
                _lineBuf[_lineLen] = '\0';
                String line(_lineBuf);
                line.trim();
                if (line.length() > 0 && line.charAt(0) == '{') {
                    dispatchLine(line);
                }
            }
            _lineLen = 0;
            continue;
        }

        if (_lineLen < sizeof(_lineBuf) - 1) {
            _lineBuf[_lineLen++] = c;
        } else {
            DBG("SERIAL-CFG: line too long, dropping");
            _lineLen = 0;
        }
    }
}