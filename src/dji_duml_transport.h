// ============================================================================
// dji_duml_transport.h — Shared DUML-over-BLE transport for the DJI Osmo family
// ============================================================================
// PROTOTYPE — extracted from dji_camera.cpp as part of splitting the DJI
// backend into dji_action_camera.* (untested, assumed-compatible) and
// dji_nano_camera.* (hardware-verified). See PROTOTYPE_NOTES.md.
//
// Packet framing, GATT layout, the connect/auth/keepalive state machine, and
// the pairing handshake are — as far as has been verified — identical across
// the DJI Osmo Action + Nano DUML-over-BLE product line, so this is the one
// place both per-model backends build on. A transport-level fix (reconnect
// timing, pairing edge cases, the FC-UART-passthrough reconnect guard, etc.)
// only has to be made once and both backends get it.
//
// What is deliberately NOT here, because it's genuinely per-model and still
// needs real Action hardware to pin down: device-identification filters
// (name prefixes / MAC OUI), record command opcodes, and telemetry field
// offsets. Those stay in each model's own .cpp.
// ============================================================================

#ifndef DJI_DUML_TRANSPORT_H
#define DJI_DUML_TRANSPORT_H

#include "camera_common.h"
#include "settings.h"
#include <NimBLEDevice.h>

// ──────────────────────────────────────────────────────────────────────────────
// GATT layout — DJI "Camera Control" BLE service. Confirmed identical on the
// Osmo Nano; assumed identical on Action (same as the rest of this file).
// ──────────────────────────────────────────────────────────────────────────────
extern const NimBLEUUID DJI_DUML_SERVICE_UUID;      // 0xFFF0
extern const NimBLEUUID DJI_DUML_CHAR_FFF3;         // Telemetry notify
extern const NimBLEUUID DJI_DUML_CHAR_FFF4;         // Pairing arm + notifications
extern const NimBLEUUID DJI_DUML_CHAR_FFF5;         // DUML commands (WriteNoResponse)
extern const NimBLEUUID DJI_DUML_ADV_SERVICE_UUID;  // Same UUID, advertised form

// BLE Company Identifiers (Bluetooth SIG-assigned to DJI as a company, so
// brand-wide rather than per-model — safe to share):
//   0x08AA = DJI   (little-endian on the wire: 0xAA, 0x08 in adv payload)
//   0xAAF7 = Xtra  (silicon shared by some DJI cameras)
extern const uint8_t DJI_DUML_COMPANY_ID[2];
extern const uint8_t DJI_DUML_XTRA_COMPANY[2];

/// True if `haystack` contains the 2-byte needle anywhere. Used by each
/// model's isXxxDevice() to scan BLE manufacturer-data payloads.
bool dumlBytesContain(const std::string &haystack, const uint8_t *needle);

// ──────────────────────────────────────────────────────────────────────────────
// Session state — one instance per DJI backend (dji_action_camera.cpp and
// dji_nano_camera.cpp each keep their own static DjiDumlSession). Only one
// is ever driven per loop() tick (camera_manager dispatches to exactly one
// active backend), but keeping state in an explicit struct — instead of the
// file-scope statics the original dji_camera.cpp used — is what lets the
// transport functions below be reusable without copy-pasting them.
// ──────────────────────────────────────────────────────────────────────────────
struct DjiDumlSession {
    BleConnectionState bleState = BLE_DISCONNECTED;

    NimBLEClient               *pClient        = nullptr;
    NimBLERemoteCharacteristic *pControlChar   = nullptr;  // fff5
    NimBLERemoteCharacteristic *pTelemetryChar = nullptr;  // fff3
    NimBLERemoteCharacteristic *pAuthChar      = nullptr;  // fff4

    NimBLEAddress targetAddress;
    bool     hasTargetAddress   = false;
    bool     doConnect          = false;
    bool     sessionEstablished = false;
    uint16_t sequenceCounter    = 1;

    uint32_t lastReconnectAttempt = 0;
    uint32_t lastKeepAlive        = 0;
    uint32_t pairingArmedTime     = 0;
    uint32_t authStartMs          = 0;  // When pairing handshake began
    uint32_t lastRxMs             = 0;  // Last inbound DUML notification (ANY traffic)

    // User-facing error from the last connect attempt. Empty = no error.
    char lastError[48] = "";

    void resetError()            { lastError[0] = '\0'; }
    void setError(const char *m) { strlcpy(lastError, m, sizeof(lastError)); }
};

/// Plain function-pointer notify callback, matching what NimBLERemoteCharacteristic
/// ::subscribe() accepts. Each per-model file supplies its own static function
/// (NimBLE's callback has no user-data slot, so it can't close over a specific
/// DjiDumlSession — that's why this stays per-model even though most of its
/// BODY can call into the shared helpers below).
typedef void (*DumlNotifyCallback)(NimBLERemoteCharacteristic *pChar, uint8_t *pData,
                                    size_t length, bool isNotify);

// ──────────────────────────────────────────────────────────────────────────────
// Packet framing (byte-identical across the family — pure DUML, no model
// specifics).
// ──────────────────────────────────────────────────────────────────────────────
uint8_t  dumlCrc8(const uint8_t *data, size_t len);
uint16_t dumlCrc16(const uint8_t *data, size_t len);
size_t   dumlBuildPacket(uint8_t *buffer, uint8_t sender, uint8_t receiver,
                          uint16_t msgId, uint8_t flags, uint8_t cmdSet, uint8_t cmdId,
                          const uint8_t *payload, size_t payloadLen);

/// Parsed common DUML frame header (flags/cmdSet/cmdId). Every model's
/// notifyCallback should update session.lastRxMs itself FIRST (any inbound
/// traffic — even a non-DUML frame — proves the link is alive), then call
/// dumlParseHeader(); on false, log the raw bytes (dumlLogNonDuml()) and stop.
struct DumlFrameHeader {
    uint8_t flags;
    uint8_t cmdSet;
    uint8_t cmdId;
};
bool dumlParseHeader(const uint8_t *pData, size_t length, DumlFrameHeader &out);

/// Hex-dumps a non-DUML notification for debugging, matching the original
/// file's behaviour (first 32 bytes only).
void dumlLogNonDuml(const char *charName, const uint8_t *pData, size_t length);

// ──────────────────────────────────────────────────────────────────────────────
// Pairing handshake. NOTE: the identifier+token scheme here is commented in
// the original dji_camera.cpp as "hardware-verified by the osmosis project,
// Action 5 Pro / Osmo Nano" — i.e. unlike the record opcode, this piece
// already has an external claim of cross-model verification, not just this
// project's own Nano testing. Treated as shared on that basis.
// ──────────────────────────────────────────────────────────────────────────────
void dumlSendPairingArm(DjiDumlSession &s);
void dumlSendPairingPin(DjiDumlSession &s);
void dumlSendKeepAlive(DjiDumlSession &s);

/// Handles the two pairing-related notification frames (PairingStatus
/// 0x07/0x45 and PairingApproved 0x07/0x46) that are identical regardless of
/// model. Returns true if it consumed the frame — the caller's notifyCallback
/// should return immediately without any further per-model handling of it.
bool dumlHandlePairingFrame(DjiDumlSession &s, const DumlFrameHeader &hdr,
                             const uint8_t *pData, size_t length);

// ──────────────────────────────────────────────────────────────────────────────
// Connection lifecycle
// ──────────────────────────────────────────────────────────────────────────────

/// Opens the NimBLE client, connects to s.targetAddress, discovers the
/// 0xFFF0 service + subscribes fff3/fff4, sets `modelName` into the shared
/// CameraTelemetry.model field, and kicks off the pairing-arm handshake.
/// Returns false (with s.lastError set) on any failure.
bool dumlConnectToCamera(DjiDumlSession &s, CameraTelemetry &telemetry,
                          const char *modelName,
                          NimBLEClientCallbacks *clientCallbacks,
                          DumlNotifyCallback notifyCallback);

/// Starts a one-shot 5s discovery scan window with the given advertised-
/// device callback object. Mirrors the duty-cycle/window tuning that keeps
/// the Wi-Fi SoftAP beaconing during scans (40ms window / 100ms interval).
void dumlStartScan(DjiDumlSession &s, NimBLEAdvertisedDeviceCallbacks *scanCallbacks);

/// Drives the shared state machine for one loop() tick: honours a pending
/// doConnect, direct-reconnects to the registry's active saved MAC of
/// `camType` (skipping discovery entirely — same as the original), handles
/// the scanning→disconnected transition, the pairing-arm→PIN timing + 30s
/// auth timeout, and the connected-state keepalive/staleness watchdog.
///
/// `skipAutoReconnect` mirrors the FC-UART-passthrough guard the project's
/// history already fixed for this exact stall class: pass
/// serialConfigFcUartActive(). A background reconnect attempt blocks loop()
/// for up to BLE_CONNECT_TIMEOUT_MS if the saved camera is unreachable,
/// which — during an active bench session over the FC-UART passthrough —
/// outlasts FC_UART_BENCH_IDLE_MS and corrupts Serial1's JSON framing. A
/// user-initiated connect (s.doConnect) is never skipped, only the
/// *automatic* periodic one.
void dumlUpdate(DjiDumlSession &s, CameraTelemetry &telemetry, CameraType camType,
                 const char *modelName, bool skipAutoReconnect,
                 NimBLEClientCallbacks *clientCallbacks,
                 DumlNotifyCallback notifyCallback);

/// Stop scanning and connect directly to this MAC (user-approved pairing /
/// saved-camera kick). `logPrefix` is just the DBG() tag ("ACTION"/"NANO").
void dumlTargetMac(DjiDumlSession &s, const char *mac, const char *logPrefix);

#endif // DJI_DUML_TRANSPORT_H