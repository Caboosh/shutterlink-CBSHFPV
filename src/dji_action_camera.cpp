// ============================================================================
// dji_action_camera.cpp — DUML-over-BLE backend for DJI Osmo Action cameras
// ============================================================================
// PROTOTYPE. See dji_action_camera.h — this preserves the previous
// "dji_camera.cpp" backend's device-ID filters, pairing, record opcode and
// telemetry parsing as-is (assumed compatible, UNTESTED on real Action
// hardware), rebuilt on top of the shared dji_duml_transport module so
// transport-level fixes (reconnect timing, staleness watchdog, the
// FC-UART-passthrough reconnect guard) apply to both DJI backends
// automatically. See PROTOTYPE_NOTES.md for the full rationale.
// ============================================================================

#include "dji_action_camera.h"
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

static const char *MODEL_NAME = "DJI Osmo Action";

// ──────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ──────────────────────────────────────────────────────────────────────────────
void        djiActionStartScan();
static void notifyCallback(NimBLERemoteCharacteristic *pChar, uint8_t *pData,
                            size_t length, bool isNotify);
static bool isDjiActionDevice(NimBLEAdvertisedDevice *device);

// ──────────────────────────────────────────────────────────────────────────────
// BLE Callbacks
// ──────────────────────────────────────────────────────────────────────────────
class ActionClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient *pClient) override {
        DBG("ACTION: Connected to camera");
    }
    void onDisconnect(NimBLEClient *pClient) {
        DBG("ACTION: Disconnected from camera");
        _session.bleState = BLE_DISCONNECTED;
        _session.sessionEstablished = false;
        _session.pControlChar   = nullptr;
        _session.pTelemetryChar = nullptr;
        _session.pAuthChar      = nullptr;
        _telemetry.dataValid = false;
        _telemetry.state     = CAM_STATE_UNKNOWN;
    }
};
static ActionClientCallbacks _clientCallbacks;

// RTOS-SAFE scan callback — see dji_duml_transport.h's notes on the NimBLE
// host task; same rules as the original file (no NVS/Serial/heavy work in
// here, copy NimBLE's std::string temporaries before use).
class ActionAdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice *advertisedDevice) override {
        bool isMatched = isDjiActionDevice(advertisedDevice);
        if (!isMatched && scanResultsIsShowAll()) {
            std::string addr = advertisedDevice->getAddress().toString();
            isMatched = !addr.empty();
        }
        if (!isMatched) return;

        std::string macStr  = advertisedDevice->getAddress().toString();
        std::string nameStr = advertisedDevice->haveName()
                              ? advertisedDevice->getName() : "";
        int8_t rssi = advertisedDevice->getRSSI();

        scanResultsAdd(CAMERA_DJI_ACTION, macStr.c_str(), nameStr.c_str(), rssi, isMatched);
        camRegistryRemember(CAMERA_DJI_ACTION, macStr.c_str(), nameStr.c_str());

        if (camRegistryMayAutoConnect(CAMERA_DJI_ACTION, macStr.c_str())) {
            NimBLEDevice::getScan()->stop();
            _session.targetAddress    = advertisedDevice->getAddress();
            _session.hasTargetAddress = true;
            _session.doConnect        = true;
        }
    }
};
static ActionAdvertisedDeviceCallbacks _scanCallbacks;

// ──────────────────────────────────────────────────────────────────────────────
// Device identification — RENAME-PROOF
// ──────────────────────────────────────────────────────────────────────────────
// Preserved verbatim from the previous combined dji_camera.cpp. The
// DJI_ACTION_OUI_PREFIXES list below is NOT confirmed to be Action-specific
// — it may be brand-wide (shared silicon/radio module across DJI's camera
// line) or it may in fact be the Nano's OUI carried over by mistake when
// this file was originally labeled "Action" but tuned against Nano hardware
// (see PROTOTYPE_NOTES.md). Kept unchanged here pending real Action hardware
// to test against; dji_nano_camera.cpp keeps the identical list for the same
// reason — safer to over-match on both than to guess a split and break the
// Nano's already-working auto-detect.
static const char *DJI_ACTION_NAME_PREFIXES[] = {
    "Osmo Action", "DJI Action", "OSMO ACTION", "DJI ACTION",
    "Action 2", "action2", "Action 4", "Action 5", "OsmoAction",
    "rishavhsAction2", "RishavhsAction2", "RISHAVHSACTION2",
};
static const size_t DJI_ACTION_NAME_PREFIX_COUNT =
    sizeof(DJI_ACTION_NAME_PREFIXES) / sizeof(DJI_ACTION_NAME_PREFIXES[0]);

// MAC OUI (Organisationally Unique Identifier) — first 3 bytes of the MAC.
// See the note above: status unconfirmed as Action-specific vs. brand-wide.
static const char *DJI_ACTION_OUI_PREFIXES[] = {
    "34:d2:62", "60:60:1f", "ec:9e:ea",
};
static const size_t DJI_ACTION_OUI_COUNT =
    sizeof(DJI_ACTION_OUI_PREFIXES) / sizeof(DJI_ACTION_OUI_PREFIXES[0]);

static bool isDjiActionDevice(NimBLEAdvertisedDevice *device) {
    if (settingsGet().scanAll) {
        std::string addr = device->getAddress().toString();
        return !addr.empty();
    }

    // Signal 1: name prefix match.
    if (device->haveName()) {
        std::string name = device->getName();
        for (size_t i = 0; i < DJI_ACTION_NAME_PREFIX_COUNT; i++) {
            if (name.find(DJI_ACTION_NAME_PREFIXES[i]) != std::string::npos) return true;
        }
    }

    // Signal 2: MAC OUI match.
    std::string addr = device->getAddress().toString();
    for (size_t i = 0; i < DJI_ACTION_OUI_COUNT; i++) {
        if (addr.rfind(DJI_ACTION_OUI_PREFIXES[i], 0) == 0) return true;
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
    //
    // NOTE: README.md documents this command as cmdSet 0x0A/cmdId 0x0D,
    // but this file (as inherited) sends and listens on 0x02/0x02. Left
    // unchanged pending real Action hardware to resolve which is correct —
    // see PROTOTYPE_NOTES.md.
    if (hdr.flags == 0xC0 && hdr.cmdSet == 0x02 && hdr.cmdId == 0x02 && length >= 12) {
        uint8_t reply = pData[11];
        switch (reply) {
            case 0x00: DBG("ACTION: record command OK"); break;
            case 0xd8: DBG("ACTION: record cmd: resource not ready"); break;
            case 0xd9: DBG("ACTION: record cmd: wrong state (already rec?)"); break;
            case 0xdf: DBG("ACTION: record cmd: wrong parameter"); break;
            case 0xe3: DBG("ACTION: record cmd: bad/missing parameter"); break;
            case 0xe0: DBG("ACTION: record cmd: NOT SUPPORTED by camera"); break;
            default:   DBG("ACTION: record cmd reply 0x%02X", reply); break;
        }
    }

    // General Status Push (flags=0x00, set=0x02, id=0x80).
    // UNVERIFIED ON ACTION HARDWARE — these offsets were reverse-engineered
    // against a real Osmo Nano (see dji_nano_camera.cpp) and copied here on
    // the assumption the two models share the same telemetry layout. May be
    // wrong. pData[11]: 0x01 idle, 0x41 arming/starting, 0x81 recording
    // (bit 0x80 = active). pData[15]: 0x01 video / 0x00 photo (unused —
    // FPV-only build). pData[20:21] LE: storage counter. pData[28:29] LE:
    // estimated remaining record time (standby only).
    if (hdr.flags == 0x00 && hdr.cmdSet == 0x02 && hdr.cmdId == 0x80 && length >= 30) {
        _telemetry.dataValid = true;

        uint8_t recByte     = pData[11];
        bool    isRecording = (recByte & 0x80) != 0;

        if (isRecording)           _telemetry.state = CAM_STATE_RECORDING;
        else if (recByte == 0x01)  _telemetry.state = CAM_STATE_STANDBY;

        if (isRecording && !_wasRecording) {
            _recordStartMs = millis();
        }
        _wasRecording = isRecording;

        _telemetry.captureMode = pData[15];
        _telemetry.storageRaw  = pData[20] | (pData[21] << 8);

        if (isRecording) {
            _telemetry.recTimeSeconds = (uint16_t)((millis() - _recordStartMs) / 1000);
        } else {
            _telemetry.recTimeSeconds = pData[28] | (pData[29] << 8);
        }
    }

    // Battery Status Push (flags=0x00, set=0x0D, id=0x02).
    // UNVERIFIED ON ACTION HARDWARE — same caveat as above.
    if (hdr.flags == 0x00 && hdr.cmdSet == 0x0D && hdr.cmdId == 0x02 && length >= 32) {
        _telemetry.batteryPercent = pData[31];
        _telemetry.dataValid = true;
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────────────────────────────────────
void djiActionInit() {
    DBG("ACTION: Backend ready (NimBLE stack shared)");
    _session   = DjiDumlSession();
    _telemetry = CameraTelemetry();
}

void djiActionUpdate() {
    dumlUpdate(_session, _telemetry, CAMERA_DJI_ACTION, MODEL_NAME,
               serialConfigFcUartActive(), &_clientCallbacks, notifyCallback);
}

void djiActionStartScan() {
    dumlStartScan(_session, &_scanCallbacks);
}

bool djiActionSendStartRecord() {
    if (_session.bleState != BLE_CONNECTED || !_session.sessionEstablished) return false;

    uint8_t packet[32];
    uint8_t payload[] = {0x01}; // 1 = Start

    // Osmo Action family record control: CmdSet 0x02, CmdId 0x02, payload
    // 0x01 = start. Preserved as inherited — see the README-mismatch note
    // in notifyCallback() above.
    size_t len = dumlBuildPacket(packet, 0x02, 0x01, _session.sequenceCounter++,
                                  0x40, 0x02, 0x02, payload, sizeof(payload));

    if (_session.pControlChar && _session.pControlChar->canWriteNoResponse()) {
        _session.pControlChar->writeValue(packet, len, false);
        DBG("ACTION: Sent Start Record to FFF5");
        return true;
    }
    return false;
}

bool djiActionSendStopRecord() {
    if (_session.bleState != BLE_CONNECTED || !_session.sessionEstablished) return false;

    uint8_t packet[32];
    uint8_t payload[] = {0x00}; // 0 = Stop

    size_t len = dumlBuildPacket(packet, 0x02, 0x01, _session.sequenceCounter++,
                                  0x40, 0x02, 0x02, payload, sizeof(payload));

    if (_session.pControlChar && _session.pControlChar->canWriteNoResponse()) {
        _session.pControlChar->writeValue(packet, len, false);
        DBG("ACTION: Sent Stop Record to FFF5");
        return true;
    }
    return false;
}

BleConnectionState djiActionGetState() { return _session.bleState; }
const CameraTelemetry& djiActionGetTelemetry() { return _telemetry; }
bool djiActionIsReady() { return (_session.bleState == BLE_CONNECTED) && _session.sessionEstablished; }
const char* djiActionGetLastError() { return _session.lastError; }

void djiActionTargetMac(const char *mac) {
    dumlTargetMac(_session, mac, "ACTION");
}