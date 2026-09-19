// ============================================================================
// camera_common.h — Shared types for camera BLE backends
// ============================================================================
// Every camera backend (DJI Osmo, GoPro) implements the same function set so
// the camera manager can dispatch to the active one at runtime.
// ============================================================================

#ifndef CAMERA_COMMON_H
#define CAMERA_COMMON_H

#include <Arduino.h>
#include "config.h"

// ──────────────────────────────────────────────────────────────────────────────
// Camera State (parsed from telemetry notifications)
// ──────────────────────────────────────────────────────────────────────────────

enum CameraRecordingState : uint8_t {
    CAM_STATE_UNKNOWN   = 0,
    CAM_STATE_STANDBY   = 1,
    CAM_STATE_RECORDING = 2,
    CAM_STATE_ERROR     = 3,
};

struct CameraTelemetry {
    uint8_t              batteryPercent;
    uint16_t             recTimeSeconds;    // now: estimated REMAINING record time
    CameraRecordingState state;
    bool                 dataValid;
    char                 model[24];

    uint8_t   captureMode;   // 1 = video, 0 = photo (raw byte, mapped in UI layer)
    uint16_t  storageRaw;    // free-storage counter, unit still TBD
    bool      photoPending;  // true briefly after a photo capture (pData[24:27] != 0)

    CameraTelemetry()
        : batteryPercent(255), recTimeSeconds(0),
          state(CAM_STATE_UNKNOWN), dataValid(false),
          captureMode(1), storageRaw(0), photoPending(false) {
        model[0] = '\0';
    }
};

// ──────────────────────────────────────────────────────────────────────────────
// BLE Connection State
// ──────────────────────────────────────────────────────────────────────────────

enum BleConnectionState : uint8_t {
    BLE_DISCONNECTED,
    BLE_SCANNING,
    BLE_CONNECTING,
    BLE_AUTHENTICATING,   // DJI: pairing handshake / GoPro: waiting for approval
    BLE_CONNECTED,        // Ready to send commands
};

#endif // CAMERA_COMMON_H
