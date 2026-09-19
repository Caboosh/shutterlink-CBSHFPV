// ============================================================================
// json_scan.h — Tiny flat-JSON reader shared by the HTTP and Serial APIs
// ============================================================================
// Both /api/* (Wi-Fi) and the Web Serial bench protocol accept JSON produced
// by our own UI code, so a simple substring scanner is sufficient — no need
// for a real JSON library. Shared here so both transports parse identically.
// ============================================================================

#ifndef JSON_SCAN_H
#define JSON_SCAN_H

#include <Arduino.h>

bool jsonHas(const String &body, const char *key);
long jsonGetNum(const String &body, const char *key, long def = 0);
bool jsonGetBool(const String &body, const char *key, bool def = false);
String jsonGetStr(const String &body, const char *key);

#endif // JSON_SCAN_H