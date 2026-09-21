// ============================================================================
// dji_nano_camera.cpp — DUML-over-BLE backend for the DJI Osmo Nano
// ============================================================================
// PROTOTYPE. Hardware-verified: pairing, start/stop record, and telemetry
// (battery %, recording state, elapsed/remaining record time) all confirmed
// end-to-end against a real Osmo Nano and the Betaflight OSD. Built on the
// shared dji_duml_transport module. See PROTOTYPE_NOTES.md.
// ============================================================================

#include "dji_nano_camera.h"
#include "dji_duml_transport.h"
#include "cam_registry.h"
#include "scan_results.h"
#include "settings.h"
#include "serial_config.h"
#include <NimBLEDevice.h>

// ──────────────────────────────────────────────────────────────────────────────
// Internal state
// ──────────────────────────────────────────────────────────────────────────────
static DjiDumlSession  _session;
static CameraTelemetry _telemetry;

static bool     _wasRecording  = false;
static uint32_t _recordStartMs = 0;

static const char *MODEL_NAME = "DJI Osmo Nano";

// ──────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ──────────────────────────────────────────────────────────────────────────────
void        djiNanoStartScan();
static void notifyCallback(NimBLERemoteCharacteristic *pChar, uint8_t *pData,
                            size_t length, bool isNotify);
static bool isDjiNanoDevice(NimBLEAdvertisedDevice *device);

// ──────────────────────────────────────────────────────────────────────────────
// BLE Callbacks
// ──────────────────────────────────────────────────────────────────────────────
class NanoClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient *pClient) override {
        DBG("NANO: Connected to camera");
    }
    void onDisconnect(NimBLEClient *pClient) {
        DBG("NANO: Disconnected from camera");
        _session.bleState = BLE_DISCONNECTED;
        _session.sessionEstablished = false;
        _session.pControlChar   = nullptr;
        _session.pTelemetryChar = nullptr;
        _session.pAuthChar      = nullptr;
        _telemetry.dataValid = false;
        _telemetry.state     = CAM_STATE_UNKNOWN;
    }
};
static NanoClientCallbacks _clientCallbacks;

class NanoAdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice *advertisedDevice) override {
        bool isMatched = isDjiNanoDevice(advertisedDevice);
        if (!isMatched && scanResultsIsShowAll()) {
            std::string addr = advertisedDevice->getAddress().toString();
            isMatched = !addr.empty();
        }
        if (!isMatched) return;

        std::string macStr  = advertisedDevice->getAddress().toString();
        std::string nameStr = advertisedDevice->haveName()
                              ? advertisedDevice->getName() : "";
        int8_t rssi = advertisedDevice->getRSSI();

        scanResultsAdd(CAMERA_DJI_NANO, macStr.c_str(), nameStr.c_str(), rssi, isMatched);
        camRegistryRemember(CAMERA_DJI_NANO, macStr.c_str(), nameStr.c_str());

        if (camRegistryMayAutoConnect(CAMERA_DJI_NANO, macStr.c_str())) {
            NimBLEDevice::getScan()->stop();
            _session.targetAddress    = advertisedDevice->getAddress();
            _session.hasTargetAddress = true;
            _session.doConnect        = true;
        }
    }
};
static NanoAdvertisedDeviceCallbacks _scanCallbacks;

// ──────────────────────────────────────────────────────────────────────────────
// Device identification — RENAME-PROOF
// ──────────────────────────────────────────────────────────────────────────────
// Name prefixes: "Osmo Nano" etc. added here — the previous combined
// dji_camera.cpp had NO Nano name entries at all, so a real Nano only ever
// matched via MAC OUI / service UUID / manufacturer ID. Adding the name
// signal is cheap and makes matching more robust/readable.
//
// The OUI list below is kept IDENTICAL to dji_action_camera.cpp's, on
// purpose: it's how this project's real Nano has been matching in practice,
// and it's unknown whether these OUIs are Nano-specific, Action-specific, or
// shared across DJI's camera line (likely, since OUIs are usually tied to a
// shared radio module/vendor, not a specific consumer product). Splitting
// this without real Action hardware to test against risks silently breaking
// the Nano's already-working auto-detect — safer to over-match on both
// files for now and narrow it once Action hardware is available.
static const char *DJI_NANO_NAME_PREFIXES[] = {
    "Osmo Nano", "DJI Nano", "OSMO NANO", "DJI OSMO NANO", "OsmoNano",
};
static const size_t DJI_NANO_NAME_PREFIX_COUNT =
    sizeof(DJI_NANO_NAME_PREFIXES) / sizeof(DJI_NANO_NAME_PREFIXES[0]);

static const char *DJI_NANO_OUI_PREFIXES[] = {
    "34:d2:62", "60:60:1f", "ec:9e:ea",
};
static const size_t DJI_NANO_OUI_COUNT =
    sizeof(DJI_NANO_OUI_PREFIXES) / sizeof(DJI_NANO_OUI_PREFIXES[0]);

static bool isDjiNanoDevice(NimBLEAdvertisedDevice *device) {
    if (settingsGet().scanAll) {
        std::string addr = device->getAddress().toString();
        return !addr.empty();
    }

    // Signal 1: name prefix match.
    if (device->haveName()) {
        std::string name = device->getName();
        for (size_t i = 0; i < DJI_NANO_NAME_PREFIX_COUNT; i++) {
            if (name.find(DJI_NANO_NAME_PREFIXES[i]) != std::string::npos) return true;
        }
    }

    // Signal 2: MAC OUI match.
    std::string addr = device->getAddress().toString();
    for (size_t i = 0; i < DJI_NANO_OUI_COUNT; i++) {
        if (addr.rfind(DJI_NANO_OUI_PREFIXES[i], 0) == 0) return true;
    }

    // Signal 3: advertised service UUID 0xFFF0 (shared — see transport module).
    if (device->isAdvertisingService(DJI_DUML_ADV_SERVICE_UUID)) return true;

    // Signal 4: manufacturer data contains DJI / Xtra company id (shared).
    std::string mfr = device->getManufacturerData();
    if (!mfr.empty()) {
        if (dumlBytesContain(mfr, DJI_DUML_COMPANY_ID))   return true;
        if (dumlBytesContain(mfr, DJI_DUML_XTRA_COMPANY)) return true;
    }

    return false;
}

// ──────────────────────────────────────────────────────────────────────────────
// Notification Callback
// ──────────────────────────────────────────────────────────────────────────────
static void notifyCallback(NimBLERemoteCharacteristic *pChar, uint8_t *pData,
                            size_t length, bool isNotify) {
    const char *charName = (pChar == _session.pAuthChar) ? "FFF4" : "FFF5";
    _session.lastRxMs = millis();  // Any inbound traffic proves the link is alive

    DumlFrameHeader hdr;
    if (!dumlParseHeader(pData, length, hdr)) {
        dumlLogNonDuml(charName, pData, length);
        return;
    }

    if (dumlHandlePairingFrame(_session, hdr, pData, length)) return;

    // Record-control response (flags=0xC0, set=0x02, id=0x02): first
    // payload byte is the camera's reply code (MEDIA_PROTOCOL.md):
    //   00 = OK, d8 = resource busy, d9 = wrong state, df = wrong param,
    //   e3 = bad param, e0 = not supported, silence = receiver missing.
    if (hdr.flags == 0xC0 && hdr.cmdSet == 0x02 && hdr.cmdId == 0x02 && length >= 12) {
        uint8_t reply = pData[11];
        switch (reply) {
            case 0x00: DBG("NANO: record command OK"); break;
            case 0xd8: DBG("NANO: record cmd: resource not ready"); break;
            case 0xd9: DBG("NANO: record cmd: wrong state (already rec?)"); break;
            case 0xdf: DBG("NANO: record cmd: wrong parameter"); break;
            case 0xe3: DBG("NANO: record cmd: bad/missing parameter"); break;
            case 0xe0: DBG("NANO: record cmd: NOT SUPPORTED by camera"); break;
            default:   DBG("NANO: record cmd reply 0x%02X", reply); break;
        }
    }

    // General Status Push (flags=0x00, set=0x02, id=0x80). HARDWARE-VERIFIED.
    // pData[11]: 0x01 idle, 0x41 arming/starting, 0x81 recording (bit 0x80 =
    // active). pData[15]: 0x01 video mode, 0x00 photo mode (unused on this
    // build — FPV-only, photo mode deliberately left unparsed).
    // pData[20:21] LE: storage counter (decreases while writing) — unit TBD.
    // pData[28:29] LE: estimated remaining record time (standby only).
    if (hdr.flags == 0x00 && hdr.cmdSet == 0x02 && hdr.cmdId == 0x80 && length >= 30) {
        _telemetry.dataValid = true;

        uint8_t recByte     = pData[11];
        bool    isRecording = (recByte & 0x80) != 0;

        if (isRecording)           _telemetry.state = CAM_STATE_RECORDING;
        else if (recByte == 0x01)  _telemetry.state = CAM_STATE_STANDBY;
        // 0x41 (arming) intentionally left as previous state — too brief to act on yet.

        // Edge-detect standby -> recording so the elapsed-time clock starts
        // from the moment recording actually began, not from whenever the
        // Web UI next polls /api/status.
        if (isRecording && !_wasRecording) {
            _recordStartMs = millis();
        }
        _wasRecording = isRecording;

        _telemetry.captureMode = pData[15];
        _telemetry.storageRaw  = pData[20] | (pData[21] << 8);

        if (isRecording) {
            // While recording: locally-tracked ELAPSED time (camera doesn't
            // push this directly — pData[28:29] goes stale/irrelevant here).
            _telemetry.recTimeSeconds = (uint16_t)((millis() - _recordStartMs) / 1000);
        } else {
            // Standby: camera's own estimated REMAINING record time.
            _telemetry.recTimeSeconds = pData[28] | (pData[29] << 8);
        }
    }

    // Battery Status Push (flags=0x00, set=0x0D, id=0x02). HARDWARE-VERIFIED.
    // pData[31]: battery % — confirmed against real drain across sessions.
    if (hdr.flags == 0x00 && hdr.cmdSet == 0x0D && hdr.cmdId == 0x02 && length >= 32) {
        _telemetry.batteryPercent = pData[31];
        _telemetry.dataValid = true;
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────────────────────────────────────
void djiNanoInit() {
    DBG("NANO: Backend ready (NimBLE stack shared)");
    _session   = DjiDumlSession();
    _telemetry = CameraTelemetry();
}

void djiNanoUpdate() {
    dumlUpdate(_session, _telemetry, CAMERA_DJI_NANO, MODEL_NAME,
               serialConfigFcUartActive(), &_clientCallbacks, notifyCallback);
}

void djiNanoStartScan() {
    dumlStartScan(_session, &_scanCallbacks);
}

bool djiNanoSendStartRecord() {
    if (_session.bleState != BLE_CONNECTED || !_session.sessionEstablished) return false;

    uint8_t packet[32];
    uint8_t payload[] = {0x01}; // 1 = Start

    // Hardware-verified against the Osmo Nano: CmdSet 0x02, CmdId 0x02,
    // payload 0x01 = start.
    size_t len = dumlBuildPacket(packet, 0x02, 0x01, _session.sequenceCounter++,
                                  0x40, 0x02, 0x02, payload, sizeof(payload));

    if (_session.pControlChar && _session.pControlChar->canWriteNoResponse()) {
        _session.pControlChar->writeValue(packet, len, false);
        DBG("NANO: Sent Start Record to FFF5");
        return true;
    }
    return false;
}

bool djiNanoSendStopRecord() {
    if (_session.bleState != BLE_CONNECTED || !_session.sessionEstablished) return false;

    uint8_t packet[32];
    uint8_t payload[] = {0x00}; // 0 = Stop

    size_t len = dumlBuildPacket(packet, 0x02, 0x01, _session.sequenceCounter++,
                                  0x40, 0x02, 0x02, payload, sizeof(payload));

    if (_session.pControlChar && _session.pControlChar->canWriteNoResponse()) {
        _session.pControlChar->writeValue(packet, len, false);
        DBG("NANO: Sent Stop Record to FFF5");
        return true;
    }
    return false;
}

BleConnectionState djiNanoGetState() { return _session.bleState; }
const CameraTelemetry& djiNanoGetTelemetry() { return _telemetry; }
bool djiNanoIsReady() { return (_session.bleState == BLE_CONNECTED) && _session.sessionEstablished; }
const char* djiNanoGetLastError() { return _session.lastError; }

void djiNanoTargetMac(const char *mac) {
    dumlTargetMac(_session, mac, "NANO");
}