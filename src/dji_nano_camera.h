// ============================================================================
// dji_nano_camera.h — DUML-over-BLE backend for the DJI Osmo Nano
// ============================================================================
// PROTOTYPE. This is the hardware-verified DJI backend: pairing, record
// start/stop, and telemetry parsing (battery / recording state / rec time)
// have all been confirmed end-to-end against a real Osmo Nano. See
// PROTOTYPE_NOTES.md.
// ============================================================================

#ifndef DJI_NANO_CAMERA_H
#define DJI_NANO_CAMERA_H

#include "camera_common.h"

void                djiNanoInit();
void                djiNanoUpdate();
bool                djiNanoSendStartRecord();
bool                djiNanoSendStopRecord();
BleConnectionState  djiNanoGetState();
const CameraTelemetry& djiNanoGetTelemetry();
bool                djiNanoIsReady();
/// Stop scanning and connect directly to this MAC (user-approved pairing).
void                djiNanoTargetMac(const char *mac);

/// User-initiated one-shot discovery scan (5 s window). Called by
/// camera_manager::camStartUserScan() from the /api/camera {scan:true}
/// endpoint — NEVER from the background reconnect loop.
void                djiNanoStartScan();

/// Read-only access to the last connect-attempt error (empty = no error).
/// Surfaced via /api/status and shown as a toast in the Web UI.
const char*         djiNanoGetLastError();

#endif // DJI_NANO_CAMERA_H