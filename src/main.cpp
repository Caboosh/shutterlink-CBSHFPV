// ============================================================================
// main.cpp — ESP32-C3 ShutterLink: Main Entry Point & State Machine
// ============================================================================
// Open-source camera control bridge.
//
// This firmware bridges a Betaflight Flight Controller (MSP / UART) with a
// BLE action camera (DJI Osmo Action or GoPro HERO8+).  It reads RC switch
// and arming state from the FC, triggers Start/Stop recording on the camera
// (manually from the Web UI too), and injects live telemetry into the FC's
// four Custom Message OSD slots.
//
// ─── Main Loop Architecture (non-blocking) ──────────────────────────────────
//
//   ┌────────────────────────────────────────────────────────────┐
//   │                        loop()                              │
//   │                                                            │
//   │  1. camUpdate()          — BLE scan/connect/keep-alive     │
//   │  2. mspPollRC()          — MSP_RC request on timer         │
//   │  3. mspReadIncoming()    — Parse UART bytes (+ bench cfg)  │
//   │  4. fcStatusUpdate()     — Arm/battery/identity polling    │
//   │  5. recorderUpdate()     — Switch + arm → record commands  │
//   │  6. osdSlotsUpdate()     — Push 4 custom messages          │
//   │  7. webUpdate()          — Captive portal + REST API       │
//   │  8. updateStatusLED()    — Blink pattern for feedback      │
//   │                                                            │
//   └────────────────────────────────────────────────────────────┘
//
// ============================================================================

#include <Arduino.h>
#include "config.h"
#include "settings.h"
#include "msp_protocol.h"
#include "fc_status.h"
#include "camera_manager.h"
#include "recorder.h"
#include "osd_slots.h"
#include "web_server.h"
#include "wifiswitch.h"
#include "cam_registry.h"
#include "serial_config.h"

// ──────────────────────────────────────────────────────────────────────────────
// Internal State
// ──────────────────────────────────────────────────────────────────────────────

static uint32_t _lastMspPoll = 0;        // Last MSP_RC request time

// LED blink state
static uint32_t _lastLedToggle = 0;
static bool     _ledState      = false;

// ──────────────────────────────────────────────────────────────────────────────
// LED Helpers
// ──────────────────────────────────────────────────────────────────────────────

static void ledOn() {
    digitalWrite(STATUS_LED_PIN, LED_ACTIVE_LOW ? LOW : HIGH);
    _ledState = true;
}

static void ledOff() {
    digitalWrite(STATUS_LED_PIN, LED_ACTIVE_LOW ? HIGH : LOW);
    _ledState = false;
}

/// Blink the LED at a given interval (non-blocking).
static void ledBlink(uint32_t intervalMs) {
    uint32_t now = millis();
    if (now - _lastLedToggle >= intervalMs) {
        _lastLedToggle = now;
        if (_ledState) ledOff(); else ledOn();
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Status LED Patterns
// ──────────────────────────────────────────────────────────────────────────────
//   • BLE disconnected / scanning : slow blink (1000 ms)
//   • BLE connecting / pairing    : fast blink (200 ms)
//   • BLE connected, standby      : solid ON
//   • Recording desired           : medium blink (500 ms)

static void updateStatusLED() {
    BleConnectionState bleState = camGetState();

    switch (bleState) {
        case BLE_DISCONNECTED:
        case BLE_SCANNING:
            ledBlink(1000);
            break;

        case BLE_CONNECTING:
        case BLE_AUTHENTICATING:
            ledBlink(200);
            break;

        case BLE_CONNECTED:
            if (recorderDesiredRecording()) {
                ledBlink(500);  // Recording: medium blink
            } else {
                ledOn();        // Standby: solid
            }
            break;
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// MSP — Periodic RC channel polling + incoming byte pump
// ──────────────────────────────────────────────────────────────────────────────

static void mspPollRC() {
    uint32_t now = millis();
    if (now - _lastMspPoll >= MSP_RC_POLL_INTERVAL_MS) {
        _lastMspPoll = now;
        mspSendRequest(MSP_RC);
    }
}

static void mspReadIncoming() {
    MspMessage msg;

    // Drain the UART buffer byte-by-byte, feeding every byte to both the
    // MSP parser AND the Web Serial bench-config JSON line assembler for
    // this port (serialConfigFeedFcUartByte). Serial1 can only be consumed
    // once, so this is the single owner of Serial1.read() — both consumers
    // just ignore whatever isn't theirs (MSP frames always start with '$',
    // JSON commands always start with '{'), the same trick already used to
    // separate DBG() lines from JSON replies sharing the USB port.
    //
    // In normal flight this only ever sees MSP frames from a live FC, and
    // the JSON assembler harmlessly discards every line (none start with
    // '{'). During a Betaflight serial passthrough session targeting this
    // UART, the FC stops running its own firmware and just bridges the
    // wire to a browser instead — so JSON bench-config commands appear
    // here, and the MSP parser's state machine harmlessly ignores them.
    while (Serial1.available()) {
        uint8_t byte = Serial1.read();

        serialConfigFeedFcUartByte(byte);

        if (mspParseByte(byte, msg)) {
            // ── Complete MSP message received ───────────────────────────
            DBG("MSP: received CMD=%u size=%u valid=%d error=%d", 
                msg.cmd, msg.payloadSize, msg.valid ? 1 : 0, msg.isError ? 1 : 0);
            
            fcStatusFeed(msg);   // Arm state / voltage / identity

            if (msg.cmd == MSP_RC && msg.valid && !msg.isError) {
                uint16_t value = mspGetRcChannel(msg, settingsGet().auxChannelIndex);
                DBG("MSP: RC ch[%d] = %d µs", settingsGet().auxChannelIndex, value);
                recorderFeedRcValue(value);

                // Separate channel toggles the Wi-Fi AP (255 = disabled).
                if (settingsGet().wifiSwitchCh <= 15) {
                    wifiSwitchFeedRc(
                        mspGetRcChannel(msg, settingsGet().wifiSwitchCh));
                }
            }
        }
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Arduino setup()
// ──────────────────────────────────────────────────────────────────────────────

void setup() {
    // ── USB Serial for debug output ─────────────────────────────────────
    Serial.begin(115200);
    delay(1000);  // Allow USB CDC to enumerate

    DBG("============================================");
    DBG("  ESP32-C3 ShutterLink v2.0");
    DBG("  Betaflight <-> DJI Osmo / GoPro bridge");
    DBG("============================================");

    // ── Persistent settings ─────────────────────────────────────────────
    settingsLoad();
    DBG("CONFIG: cam=%s auxCh=%d thr=%dµs deb=%dms roa=%d sod=%d",
        cameraTypeName(settingsGet().camera),
        settingsGet().auxChannelIndex, settingsGet().rcThresholdUs,
        settingsGet().debounceMs,
        settingsGet().recordOnArm, settingsGet().stopOnDisarm);

    // ── Status LED ──────────────────────────────────────────────────────
    pinMode(STATUS_LED_PIN, OUTPUT);
    ledOff();

    // ── MSP / UART ──────────────────────────────────────────────────────
    mspInit(Serial1);
    fcStatusInit();
    DBG("MSP: Initialised");

    // ── Recorder decision engine ────────────────────────────────────────
    recorderInit();

    // ── Camera BLE backend (DJI or GoPro per settings) ──────────────────
    camInit();

    // ── Web UI (SoftAP + captive portal + REST API) ─────────────────────
    wifiSwitchInit();
    webInit();

    // ── Web Serial bench config protocol (USB port used for DBG output,   ─
    // ── plus the FC UART when reached via Betaflight serial passthrough) ─
    serialConfigInit();

    DBG("SETUP: Complete — entering main loop");
}

// ──────────────────────────────────────────────────────────────────────────────
// Arduino loop() — Non-blocking state machine
// ──────────────────────────────────────────────────────────────────────────────

void loop() {
    // 1. Camera management: scan, connect, keep-alive, auto-reconnect.
    camUpdate();

    // While a Web Serial bench session is actively using Serial1 (i.e. a
    // Betaflight serial passthrough session bridges that UART to a
    // browser instead of a live FC), skip our own outbound MSP polling.
    // Those requests would otherwise go out over Serial1 exactly as
    // always, get faithfully relayed by the passthrough bridge straight
    // back to the browser, and corrupt its JSON line framing (raw MSP
    // frames carry no '\n', so they glue onto the front of the next real
    // JSON reply). There's no live FC to poll during passthrough anyway,
    // so skipping costs nothing beyond the bench status view's "FC" field
    // freezing at whatever it last read rather than promptly ticking over
    // to "no link" — cosmetic, and expected during a bench-only session.
    bool fcUartBenchActive = serialConfigFcUartActive();

    // 2. Send periodic MSP_RC requests to the Flight Controller.
    if (!fcUartBenchActive) mspPollRC();

    // 3. Parse any incoming UART bytes from the FC (RC + status/analog).
    mspReadIncoming();

    // 4. FC polling state machine (arm detection, battery, identity).
    if (!fcUartBenchActive) fcStatusUpdate();

    // 5. Recording decision engine: switch + record-on-arm + manual.
    recorderUpdate();

    // 6. Push the four Custom Message OSD slots to the FC.
    osdSlotsUpdate();

    // 7. Serve the captive portal + REST API.
    camRegistryProcess();   // Flush cameras discovered by BLE scan callbacks
    wifiSwitchUpdate();
    scanResultsEvictStale();   // Evict stale scan entries (TTL 15s)
    webUpdate();
    serialConfigUpdate();
    
    // 8. Update status LED blink pattern.
    updateStatusLED();

    // 9. Debug heartbeat — if these timestamps stop advancing, loop() is
    //    wedged somewhere (BLE call, driver, etc.).
    static uint32_t lastHeartbeat = 0;
    if (millis() - lastHeartbeat >= 10000) {
        lastHeartbeat = millis();
        DBG("STATE: BLE=%d rc=%u switch=%s desired=%d armed=%d heap=%u",
            camGetState(), recorderLastRcValue(),
            recorderSwitchOn() ? "ON" : "OFF",
            recorderDesiredRecording() ? 1 : 0,
            fcGetTelemetry().armed ? 1 : 0,
            ESP.getFreeHeap());
    }

    yield();
}
