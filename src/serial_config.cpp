// ============================================================================
// serial_config.cpp — Web Serial bench config protocol
// ============================================================================
// A line-oriented JSON protocol, reachable over either of two physical
// wires, that mirrors the Wi-Fi REST API's configuration surface
// (api_core.cpp) so the bench tool (docs/index.html, served over GitHub
// Pages via Web Serial) and the SoftAP field Web UI can never drift out of
// sync with each other:
//
//   • Serial  (USB-CDC, 115200 baud) — the same port used for DBG() logging
//     and flashing. Plug a USB cable straight into the C3.
//   • Serial1 (the FC UART, 115200 baud) — reachable via a Betaflight
//     serial passthrough session targeting that UART (CLI: `serial` to
//     check the argument style this firmware wants, then `serialpassthrough
//     <target> <baud>` — see docs/app.js/README.md, the syntax changed in
//     Betaflight 25.12). No direct USB access to the C3 needed: the bench
//     console drives this itself, then points its Web Serial connection at
//     the FC's own COM port instead of the C3's. Betaflight stops running
//     while passthrough is active, so this is strictly a bench-only path —
//     never in the air. While a bench session is active on this channel,
//     main.cpp pauses its own periodic MSP polling (mspPollRC(),
//     fcStatusUpdate() — see serialConfigFcUartActive()) so those requests
//     don't leak out through the passthrough bridge and corrupt the JSON
//     line framing on the browser's end.
//
// Wire format — one JSON object per line, both directions, identical on
// both ports:
//   Host -> device:  {"path":"ping"}
//                     {"path":"status"}
//                     {"path":"settings", ...same fields /api/settings takes}
//                     {"path":"camera",   ...same fields /api/camera takes}
//                     {"path":"command","cmd":"start"|"stop"|"reboot"}
//                     {"path":"msp","cmd":<u8>}
//   Device -> host:   one JSON line per response, on the same port the
//                     request arrived on, e.g. {"ok":true} or the full
//                     status object. DBG() debug lines are ALSO still
//                     printed to Serial but never start with '{', so a
//                     host-side reader tells them apart trivially. Serial1
//                     normally only ever carries '$'-prefixed MSP frames
//                     to/from a live FC, which this protocol's line
//                     assembler silently ignores the same way.
//
// Only one request is expected in flight at a time per port — the browser
// sends a command and awaits the next '{'-prefixed line before sending
// another.
//
// The MSP passthrough here is intentionally UNrestricted, unlike /api/msp's
// read-only allowlist — normally this channel requires a physical USB
// cable, a much higher trust bar than the (possibly open) Wi-Fi AP. That
// trust assumption doesn't hold for a command that itself arrived over
// Serial1 (the Betaflight-passthrough path): during passthrough the wire
// IS the browser's connection, there's no independent live FC on the other
// end to bounce an MSP request off, so that one path is refused there (see
// handleMsp() below) rather than silently hanging until MSP_RESPONSE_TIMEOUT_MS.
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
// Line buffering — one assembler per port, since a byte from one port must
// never bleed into the other's in-progress line.
// ──────────────────────────────────────────────────────────────────────────────

struct LineAssembler {
    char   buf[512];
    size_t len = 0;
};

static LineAssembler _usbLine;   // Serial  — direct USB-CDC bench cable
static LineAssembler _fcLine;    // Serial1 — FC UART, reachable via Betaflight
                                  //           serial passthrough

// Last time a bench command actually arrived over Serial1 — see
// serialConfigFcUartActive() / FC_UART_BENCH_IDLE_MS.
static uint32_t _lastFcUartCmdMs = 0;

// ──────────────────────────────────────────────────────────────────────────────
// Response helpers — transport-aware: reply on whichever port the request
// came in on.
// ──────────────────────────────────────────────────────────────────────────────

static void sendOk(Print &out) {
    out.println("{\"ok\":true}");
}

static void sendOkExtra(Print &out, const char *extraJsonFields) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ok\":true,%s}", extraJsonFields);
    out.println(buf);
}

static void sendErr(Print &out, const char *msg) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    out.println(buf);
}

// ──────────────────────────────────────────────────────────────────────────────
// MSP passthrough
// ──────────────────────────────────────────────────────────────────────────────

static void handleMsp(Print &out, const String &line, bool viaFcUart) {
    if (viaFcUart) {
        // We'd be sending the MSP request out the very wire the request
        // itself arrived on, with no real FC left to answer it (that's
        // what "passthrough" means) — every call would just silently eat
        // MSP_RESPONSE_TIMEOUT_MS and time out. Fail fast with the real
        // reason instead. Use the direct-USB bench cable for MSP passthrough.
        sendErr(out, "msp passthrough needs the direct USB cable, not the FC-UART/passthrough channel");
        return;
    }

    long cmd = jsonGetNum(line, "cmd", -1);
    if (cmd < 0 || cmd > 255) { sendErr(out, "missing/invalid cmd"); return; }

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

    if (!got) { sendErr(out, "FC timeout"); return; }

    static char hex[MSP_MAX_PAYLOAD_SIZE * 2 + 1];
    size_t o = 0;
    for (uint8_t i = 0; i < msg.payloadSize; i++) {
        o += snprintf(hex + o, sizeof(hex) - o, "%02X", msg.payload[i]);
    }
    hex[o] = '\0';

    char resp[192];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"cmd\":%u,\"len\":%u,\"payload\":\"%s\"}",
             (unsigned)msg.cmd, (unsigned)msg.payloadSize, hex);
    out.println(resp);
}

// ──────────────────────────────────────────────────────────────────────────────
// Dispatch one complete command line
// ──────────────────────────────────────────────────────────────────────────────

static void dispatchLine(Print &out, const String &line, bool viaFcUart) {
    String path = jsonGetStr(line, "path");

    if (path == "ping") {
        char resp[112];
        snprintf(resp, sizeof(resp),
                 "{\"ok\":true,\"device\":\"shutterlink\",\"fw\":\"%s\",\"via\":\"%s\"}",
                 FIRMWARE_VERSION, viaFcUart ? "fc-uart" : "usb");
        out.println(resp);

    } else if (path == "status") {
        static char buf[2200];
        apiBuildStatusJson(buf, sizeof(buf));
        out.println(buf);

    } else if (path == "settings") {
        bool apNeedsRestart = false;
        char err[80];
        if (!apiApplySettings(line, apNeedsRestart, err, sizeof(err))) {
            sendErr(out, err);
        } else if (apNeedsRestart) {
            sendOkExtra(out, "\"apRestart\":true");
            webInit();
        } else {
            sendOk(out);
        }

    } else if (path == "camera") {
        bool alreadyScanning = false;
        char err[80];
        if (!apiApplyCamera(line, alreadyScanning, err, sizeof(err))) {
            sendErr(out, err);
        } else if (alreadyScanning) {
            sendOkExtra(out, "\"already\":true");
        } else {
            sendOk(out);
        }

    } else if (path == "command") {
        String cmd = jsonGetStr(line, "cmd");
        bool shouldReboot = false;
        char err[40];
        if (!apiApplyCommand(cmd, shouldReboot, err, sizeof(err))) {
            sendErr(out, err);
        } else {
            sendOk(out);
            if (shouldReboot) {
                delay(400);
                ESP.restart();
            }
        }

    } else if (path == "msp") {
        handleMsp(out, line, viaFcUart);

    } else {
        sendErr(out, "unknown path");
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Shared line-assembly (one instance per port, via LineAssembler above)
// ──────────────────────────────────────────────────────────────────────────────

static void feedByte(LineAssembler &la, Print &out, bool viaFcUart, char c) {
    if (c == '\n' || c == '\r') {
        if (la.len > 0) {
            la.buf[la.len] = '\0';
            String line(la.buf);
            line.trim();
            if (line.length() > 0 && line.charAt(0) == '{') {
                if (viaFcUart) _lastFcUartCmdMs = millis();
                dispatchLine(out, line, viaFcUart);
            }
        }
        la.len = 0;
        return;
    }

    if (la.len < sizeof(la.buf) - 1) {
        la.buf[la.len++] = c;
    } else {
        DBG("SERIAL-CFG: line too long, dropping (%s)", viaFcUart ? "fc-uart" : "usb");
        la.len = 0;
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────────────────────────────────────

void serialConfigInit() {
    _usbLine.len = 0;
    _fcLine.len = 0;
    _lastFcUartCmdMs = 0;
}

void serialConfigUpdate() {
    while (Serial.available()) {
        feedByte(_usbLine, Serial, false, (char)Serial.read());
    }
}

void serialConfigFeedFcUartByte(uint8_t b) {
    feedByte(_fcLine, Serial1, true, (char)b);
}

bool serialConfigFcUartActive() {
    return _lastFcUartCmdMs != 0 && (millis() - _lastFcUartCmdMs) < FC_UART_BENCH_IDLE_MS;
}
