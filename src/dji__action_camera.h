// ============================================================================
// dji_action_camera.h — DUML-over-BLE backend for DJI Osmo Action cameras
// ============================================================================
// PROTOTYPE. Forked from dji_camera.cpp: this file is a near-verbatim copy
// of the previous "DJI Osmo" backend's behaviour — device-ID filters,
// pairing, record opcodes, telemetry offsets — kept as-is.
//
// STATUS: assumed compatible, NEVER TESTED on real Action hardware. This
// project only owns an Osmo Nano; everything model-specific in this file is
// inherited from what was already here, some of which (see
// dji_action_camera.cpp) may itself have been tuned against Nano captures
// during earlier telemetry RE work despite the "Action" label. Treat this
// as a starting point to verify/fix once real Action hardware is available,
// not as confirmed behaviour. See PROTOTYPE_NOTES.md.
// ============================================================================

#ifndef DJI_ACTION_CAMERA_H
#define DJI_ACTION_CAMERA_H

#include "camera_common.h"

void                djiActionInit();
void                djiActionUpdate();
bool                djiActionSendStartRecord();
bool                djiActionSendStopRecord();
BleConnectionState  djiActionGetState();
const CameraTelemetry& djiActionGetTelemetry();
bool                djiActionIsReady();
/// Stop scanning and connect directly to this MAC (user-approved pairing).
void                djiActionTargetMac(const char *mac);

/// User-initiated one-shot discovery scan (5 s window). Called by
/// camera_manager::camStartUserScan() from the /api/camera {scan:true}
/// endpoint — NEVER from the background reconnect loop.
void                djiActionStartScan();

/// Read-only access to the last connect-attempt error (empty = no error).
/// Surfaced via /api/status and shown as a toast in the Web UI.
const char*         djiActionGetLastError();

#endif // DJI_ACTION_CAMERA_H