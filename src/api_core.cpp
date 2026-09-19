// ============================================================================
// api_core.cpp — Shared configuration-surface logic (HTTP + Serial transports)
// ============================================================================
// Both the Wi-Fi REST API (web_server.cpp) and the Web Serial bench protocol
// (serial_config.cpp) expose the exact same configuration surface. This file
// holds that logic ONCE so the two transports can never drift out of sync.
// Every function here is transport-agnostic — plain strings/buffers in,
// no WebServer or Serial calls inside it.
// ============================================================================

#include "api_core.h"
#include "config.h"
#include "settings.h"
#include "camera_manager.h"
#include "dji_camera.h"
#include "gopro_camera.h"
#include "fc_status.h"
#include "recorder.h"
#include "osd_slots.h"
#include "cam_registry.h"
#include "scan_results.h"
#include "json_scan.h"
#include "web_server.h"   // sanitizeDeviceName(), webApIp(), webIsUp(), webInit()
#include <WiFi.h>

// ──────────────────────────────────────────────────────────────────────────────
// Status
// ──────────────────────────────────────────────────────────────────────────────

size_t apiBuildStatusJson(char *buf, size_t bufLen) {
    const CameraTelemetry &tel = camGetTelemetry();
    const FcTelemetry &fc = fcGetTelemetry();
    const ShutterSettings &cfg = settingsGet();

    BleConnectionState st = camGetState();
    static const char *kStateNames[] =
        {"OFF", "SCANNING", "CONNECTING", "PAIRING", "CONNECTED"};

    int batt = tel.batteryPercent <= 100 ? tel.batteryPercent : -1;

    char cams[512];
    {
        size_t o = 0;
        o += snprintf(cams + o, sizeof(cams) - o, "[");
        bool online = camIsReady();
        for (uint8_t i = 0; i < cfg.camCount; i++) {
            const SavedCamera &c = cfg.cams[i];
            const char *nm = c.name[0] ? c.name : c.mac;
            char safeName[sizeof(c.name)];
            strlcpy(safeName, nm, sizeof(safeName));
            for (char *q = safeName; *q; q++) {
                if (*q == '"' || *q == '\\') { memmove(q + 1, q, strlen(q)); *q = ' '; q++; }
            }
            o += snprintf(cams + o, sizeof(cams) - o,
                          "%s{\"t\":%u,\"n\":\"%s\",\"m\":\"%s\",\"a\":%s,\"on\":%s}",
                          i ? "," : "", c.type, safeName, c.mac,
                          c.active ? "true" : "false",
                          (c.active && online) ? "true" : "false");
            if (o >= sizeof(cams) - 8) break;
        }
        o += snprintf(cams + o, sizeof(cams) - o, "]");
    }

    char pending[640] = "[]";
    {
        ScanResult const *sorted[MAX_SCAN_RESULTS];
        uint8_t n = scanResultsGetSortedByRssi(sorted, MAX_SCAN_RESULTS);
        size_t o = 0;
        o += snprintf(pending + o, sizeof(pending) - o, "[");
        for (uint8_t i = 0; i < n && o < sizeof(pending) - 80; i++) {
            const char *typeStr = (sorted[i]->type == CAMERA_GOPRO) ? "GoPro" : "DJI";
            char safeName[sizeof(sorted[i]->name)];
            strlcpy(safeName, sorted[i]->name, sizeof(safeName));
            for (char *q = safeName; *q; q++) {
                if (*q == '"' || *q == '\\') { memmove(q + 1, q, strlen(q)); *q = ' '; q++; }
            }
            o += snprintf(pending + o, sizeof(pending) - o,
                "%s{\"mac\":\"%s\",\"n\":\"%s\",\"t\":\"%s\",\"r\":%d}",
                i ? "," : "", sorted[i]->mac, safeName, typeStr, sorted[i]->rssi);
        }
        if (o < sizeof(pending) - 1) snprintf(pending + o, sizeof(pending) - o, "]");
    }

    const char *lastErr = "";
    if (cfg.camera == CAMERA_DJI)  lastErr = djiGetLastError();
    else if (cfg.camera == CAMERA_GOPRO) lastErr = gpGetLastError();
    char safeErr[64] = "";
    {
        size_t i = 0;
        for (; lastErr[i] && i < sizeof(safeErr) - 1; i++) {
            char ch = lastErr[i];
            safeErr[i] = (ch == '"' || ch == '\\') ? ' ' : ch;
        }
        safeErr[i] = '\0';
    }

    uint32_t heap = ESP.getFreeHeap();

    size_t w = snprintf(buf, bufLen,
        "{\"heap\":%u,"
        "\"cam\":{\"type\":%d,\"name\":\"%s\",\"state\":%d,"
        "\"stateName\":\"%s\",\"batt\":%d,\"recTime\":%u,\"valid\":%s,"
        "\"model\":\"%s\"},"
        "\"rec\":{\"desired\":%s,\"switchOn\":%s,\"roa\":%s,\"sod\":%s,\"sodDelay\":%u,\"rcValue\":%u,"
        "\"auxCh\":%u,\"thr\":%u,\"deb\":%u},"
        "\"slots\":[%d,%d,%d,%d],"
        "\"osd\":[\"%s\",\"%s\",\"%s\",\"%s\"],"
        "\"wifiSwitch\":%d,\"wifiOn\":%s,\"scanAll\":%s,"
        "\"lastError\":\"%s\","
        "\"cams\":%s,\"pending_cams\":%s,\"scanning\":%s,"
        "\"fc\":{\"alive\":%s,\"armed\":%s,\"vbat10\":%u,\"rssi\":%u,"
        "\"cycle\":%u,\"api\":\"%s\",\"fw\":\"%s\",\"board\":\"%s\"},"
        "\"sys\":{\"heap\":%u,\"uptime\":%lu,\"ip\":\"%s\",\"sta\":%d,\"version\":\"%s\"}}",
        heap,
        (int)cfg.camera, camGetName(), (int)st, kStateNames[st], batt,
        tel.recTimeSeconds, tel.dataValid ? "true" : "false", tel.model,
        recorderDesiredRecording() ? "true" : "false",
        recorderSwitchOn() ? "true" : "false",
        cfg.recordOnArm ? "true" : "false",
        cfg.stopOnDisarm ? "true" : "false",
        cfg.stopOnDisarmDelayMs,
        recorderLastRcValue(),
        cfg.auxChannelIndex, cfg.rcThresholdUs, cfg.debounceMs,
        cfg.osdSlot[0], cfg.osdSlot[1], cfg.osdSlot[2], cfg.osdSlot[3],
        osdSlotText(0), osdSlotText(1), osdSlotText(2), osdSlotText(3),
        (cfg.wifiSwitchCh <= 15) ? (int)cfg.wifiSwitchCh : -1,
        webIsUp() ? "true" : "false",
        cfg.scanAll ? "true" : "false",
        safeErr,
        cams, pending,
        scanResultsIsScanning() ? "true" : "false",
        fc.fcAlive ? "true" : "false", fc.armed ? "true" : "false",
        fc.vbat10, fc.rssi / 10, fc.cycleTimeUs,
        fcApiVersion(), fcFirmwareVersion(), fcBoardName(),
        heap, millis() / 1000UL, webApIp(),
        WiFi.softAPgetStationNum(),
        FIRMWARE_VERSION);
    return w;
}

// ──────────────────────────────────────────────────────────────────────────────
// Settings
// ──────────────────────────────────────────────────────────────────────────────

bool apiApplySettings(const String &body, bool &apNeedsRestart,
                       char *errBuf, size_t errBufLen) {
    apNeedsRestart = false;

    if (!jsonHas(body, "camera") && !jsonHas(body, "auxChannel") &&
        !jsonHas(body, "recordOnArm") && !jsonHas(body, "ssid") &&
        !jsonHas(body, "slot0") && !jsonHas(body, "slot1") &&
        !jsonHas(body, "slot2") && !jsonHas(body, "slot3") &&
        !jsonHas(body, "wifiSwitch") && !jsonHas(body, "scanAll") &&
        !jsonHas(body, "threshold") && !jsonHas(body, "debounce") &&
        !jsonHas(body, "stopOnDisarm") && !jsonHas(body, "stopOnDisarmDelay")) {
        if (errBuf) strlcpy(errBuf, "no recognized keys", errBufLen);
        return false;
    }

    ShutterSettings &cfg = settingsGet();

    if (jsonHas(body, "camera")) {
        long cam = jsonGetNum(body, "camera");
        if (cam == CAMERA_GOPRO && cfg.camera != CAMERA_GOPRO) {
            camSetCamera(CAMERA_GOPRO);
        } else if (cam == CAMERA_DJI && cfg.camera != CAMERA_DJI) {
            camSetCamera(CAMERA_DJI);
        } else if (cam != CAMERA_DJI && cam != CAMERA_GOPRO) {
            if (errBuf) strlcpy(errBuf, "invalid camera type", errBufLen);
            return false;
        }
    }

    if (jsonHas(body, "auxChannel")) {
        long ch = jsonGetNum(body, "auxChannel");
        if (ch < 0 || ch > 15) { if (errBuf) strlcpy(errBuf, "channel out of range", errBufLen); return false; }
        cfg.auxChannelIndex = (uint8_t)ch;
    }
    if (jsonHas(body, "threshold")) {
        long t = jsonGetNum(body, "threshold");
        if (t < 1200 || t > 1800) { if (errBuf) strlcpy(errBuf, "threshold out of range", errBufLen); return false; }
        cfg.rcThresholdUs = (uint16_t)t;
    }
    if (jsonHas(body, "debounce")) {
        long d = jsonGetNum(body, "debounce");
        if (d < 50 || d > 1000) { if (errBuf) strlcpy(errBuf, "debounce out of range", errBufLen); return false; }
        cfg.debounceMs = (uint16_t)d;
    }

    if (jsonHas(body, "recordOnArm"))
        cfg.recordOnArm = jsonGetBool(body, "recordOnArm");
    if (jsonHas(body, "stopOnDisarm"))
        cfg.stopOnDisarm = jsonGetBool(body, "stopOnDisarm");
    if (jsonHas(body, "stopOnDisarmDelay")) {
        long d = jsonGetNum(body, "stopOnDisarmDelay");
        if (d < 0 || d > 15000) { if (errBuf) strlcpy(errBuf, "stop-on-disarm delay out of range", errBufLen); return false; }
        cfg.stopOnDisarmDelayMs = (uint16_t)d;
    }

    if (jsonHas(body, "scanAll"))
        cfg.scanAll = jsonGetBool(body, "scanAll");

    if (jsonHas(body, "wifiSwitch")) {
        long ch = jsonGetNum(body, "wifiSwitch");
        if ((ch >= 0 && ch <= 15) || ch == -1) {
            cfg.wifiSwitchCh = (ch == -1) ? 255 : (uint8_t)ch;
        } else {
            if (errBuf) strlcpy(errBuf, "wifi switch channel out of range", errBufLen);
            return false;
        }
    }

    for (uint8_t s = 0; s < 4; s++) {
        char key[8];
        snprintf(key, sizeof(key), "slot%d", s);
        if (jsonHas(body, key)) {
            long v = jsonGetNum(body, key);
            if (v < 0 || v >= OSD_SLOT_COUNT) {
                if (errBuf) strlcpy(errBuf, "invalid slot content", errBufLen);
                return false;
            }
            cfg.osdSlot[s] = (uint8_t)v;
        }
    }

    if (jsonHas(body, "ssid")) {
        String ssid = jsonGetStr(body, "ssid");
        ssid.trim();
        if (ssid.length() < 1 || ssid.length() > 32) {
            if (errBuf) strlcpy(errBuf, "SSID must be 1-32 chars", errBufLen);
            return false;
        }
        strlcpy(cfg.apSsid, ssid.c_str(), sizeof(cfg.apSsid));
        apNeedsRestart = true;
    }
    if (jsonHas(body, "pass")) {
        String pass = jsonGetStr(body, "pass");
        pass.trim();
        if (pass.length() > 0 && pass.length() < 8) {
            if (errBuf) strlcpy(errBuf, "password must be empty or 8-64 chars", errBufLen);
            return false;
        }
        strlcpy(cfg.apPass, pass.c_str(), sizeof(cfg.apPass));
        apNeedsRestart = true;
    }

    settingsSave();
    DBG("API: settings saved");
    return true;
}

// ──────────────────────────────────────────────────────────────────────────────
// Camera registry
// ──────────────────────────────────────────────────────────────────────────────

bool apiApplyCamera(const String &body, bool &alreadyScanning,
                     char *errBuf, size_t errBufLen) {
    alreadyScanning = false;

    if (jsonHas(body, "scan")) {
        if (scanResultsIsScanning()) { alreadyScanning = true; return true; }
        camStartUserScan();
        return true;

    } else if (jsonHas(body, "select")) {
        long idx = jsonGetNum(body, "select", -1);
        if (idx < 0 || !camRegistrySelect((uint8_t)idx)) {
            if (errBuf) strlcpy(errBuf, "invalid camera index", errBufLen);
            return false;
        }
        camKick();
        return true;

    } else if (jsonHas(body, "remove")) {
        long idx = jsonGetNum(body, "remove", -1);
        if (idx < 0 || !camRegistryRemove((uint8_t)idx)) {
            if (errBuf) strlcpy(errBuf, "invalid camera index", errBufLen);
            return false;
        }
        camDisconnect();
        return true;

    } else if (jsonHas(body, "pair")) {
        String mac = jsonGetStr(body, "mac");
        long type = jsonGetNum(body, "type", -1);

        if (mac.length() != 17) {
            if (errBuf) strlcpy(errBuf, "pair: invalid MAC length", errBufLen);
            return false;
        }
        for (int i = 0; i < 17; i++) {
            char c = mac.charAt(i);
            if (i % 3 == 2) {
                if (c != ':') { if (errBuf) strlcpy(errBuf, "pair: invalid MAC format", errBufLen); return false; }
            } else {
                if (!isHexadecimalDigit(c)) { if (errBuf) strlcpy(errBuf, "pair: invalid MAC character", errBufLen); return false; }
            }
        }

        if (type != CAMERA_DJI && type != CAMERA_GOPRO) {
            if (errBuf) strlcpy(errBuf, "pair: mac and type required", errBufLen);
            return false;
        }

        ScanResult const *sorted[MAX_SCAN_RESULTS];
        uint8_t scount = scanResultsGetSortedByRssi(sorted, MAX_SCAN_RESULTS);
        bool isOnline = false;
        char nameBuf[24] = "";
        for (uint8_t i = 0; i < scount; i++) {
            if (strcasecmp(sorted[i]->mac, mac.c_str()) == 0 &&
                sorted[i]->type == (uint8_t)type) {
                isOnline = true;
                sanitizeDeviceName(nameBuf, sorted[i]->name, sizeof(nameBuf));
                break;
            }
        }
        if (!isOnline) {
            if (errBuf) strlcpy(errBuf, "Cannot pair: device is offline or not in range", errBufLen);
            return false;
        }
        if (!camRegistrySave((uint8_t)type, mac.c_str(),
                              nameBuf[0] ? nameBuf : mac.c_str())) {
            if (errBuf) strlcpy(errBuf, "pair: registry full or invalid", errBufLen);
            return false;
        }
        for (uint8_t i = 0; i < settingsGet().camCount; i++) {
            if (strcasecmp(settingsGet().cams[i].mac, mac.c_str()) == 0 &&
                settingsGet().cams[i].type == (uint8_t)type) {
                camRegistrySelect(i);
                camKick();
                break;
            }
        }
        return true;
    }

    if (errBuf) strlcpy(errBuf, "expected scan, select, remove, or pair", errBufLen);
    return false;
}

// ──────────────────────────────────────────────────────────────────────────────
// Start / stop / reboot
// ──────────────────────────────────────────────────────────────────────────────

bool apiApplyCommand(const String &cmd, bool &shouldReboot,
                      char *errBuf, size_t errBufLen) {
    shouldReboot = false;
    if (cmd == "start") {
        recorderManualStart();
        return true;
    } else if (cmd == "stop") {
        recorderManualStop();
        return true;
    } else if (cmd == "reboot") {
        shouldReboot = true;
        return true;
    }
    if (errBuf) strlcpy(errBuf, "unknown command", errBufLen);
    return false;
}