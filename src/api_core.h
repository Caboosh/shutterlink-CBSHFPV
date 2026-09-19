#ifndef API_CORE_H
#define API_CORE_H

#include <Arduino.h>

/// Build the full status JSON (same shape /api/status returns) into buf.
/// Returns the length written (as snprintf would).
size_t apiBuildStatusJson(char *buf, size_t bufLen);

/// Apply a settings-update JSON body (same shape /api/settings accepts).
/// Returns true on success; on failure a short message is written into
/// errBuf. apNeedsRestart is set true when Wi-Fi credentials changed and
/// the caller should call webInit() after responding.
bool apiApplySettings(const String &body, bool &apNeedsRestart,
                       char *errBuf, size_t errBufLen);

/// Apply a camera-registry action (same shape /api/camera accepts:
/// {"scan":true} | {"select":i} | {"remove":i} | {"pair":true,"mac":...,"type":...}).
/// alreadyScanning is set true when a scan was requested but one was
/// already in progress (nothing new was started).
bool apiApplyCamera(const String &body, bool &alreadyScanning,
                     char *errBuf, size_t errBufLen);

/// Apply a {"cmd":"start"|"stop"|"reboot"} command.
/// shouldReboot is set true for "reboot" — the caller must send its
/// response BEFORE acting on it, then delay briefly and call ESP.restart().
bool apiApplyCommand(const String &cmd, bool &shouldReboot,
                      char *errBuf, size_t errBufLen);

#endif // API_CORE_H