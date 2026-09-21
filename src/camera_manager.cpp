// ============================================================================
// camera_manager.cpp — Runtime camera backend dispatcher
// ============================================================================
// PROTOTYPE: dispatch is now a 3-way switch (DJI Osmo Nano / DJI Osmo
// Action / GoPro) instead of the previous 2-way ternary, now that the old
// single "dji_camera" backend has been split into dji_nano_camera.cpp
// (hardware-verified) and dji_action_camera.cpp (assumed compatible,
// untested). See PROTOTYPE_NOTES.md.
// ============================================================================

#include "camera_manager.h"
#include "dji_nano_camera.h"
#include "dji_action_camera.h"
#include "gopro_camera.h"
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstring>

// ──────────────────────────────────────────────────────────────────────────────
// Security & Concurrency Globals
// ──────────────────────────────────────────────────────────────────────────────

static SemaphoreHandle_t g_stateMutex = NULL;
static SemaphoreHandle_t g_scanMutex = NULL;

#define SAFE_STRNCPY(dest, src, size) do { \
    if (size > 0) { \
        strncpy((char*)(dest), (const char*)(src), (size) - 1); \
        ((char*)(dest))[(size) - 1] = '\0'; \
    } \
} while(0)

// Helper to safely sanitize device names (prevent XSS injection via BLE ads)
void sanitizeDeviceName(char* dest, const char* src, size_t maxSize) {
    if (!dest || !src || maxSize == 0) return;

    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j < maxSize - 1; i++) {
        char c = src[i];
        // Allow only alphanumeric, space, dash, underscore, dot
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == ' ' || c == '-' || c == '_' || c == '.') {
            dest[j++] = c;
        } else {
            // Replace unsafe chars with '?'
            dest[j++] = '?';
        }
    }
    dest[j] = '\0';
}

// Thread-safe initialization of mutexes
static void initMutexes() {
    if (g_stateMutex == NULL) {
        g_stateMutex = xSemaphoreCreateMutex();
        configASSERT(g_stateMutex);
    }
    if (g_scanMutex == NULL) {
        g_scanMutex = xSemaphoreCreateMutex();
        configASSERT(g_scanMutex);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Internal state
// ──────────────────────────────────────────────────────────────────────────────

static bool _stackReady = false;

static void shutdownActiveBackend() {
    // Stop any scan and drop the current BLE connection before switching.
    NimBLEScan *pScan = NimBLEDevice::getScan();
    if (pScan && pScan->isScanning()) {
        pScan->stop();
    }
    std::list<NimBLEClient *> *clients = NimBLEDevice::getClientList();
    if (clients) {
        for (NimBLEClient *c : *clients) {
            if (c->isConnected()) c->disconnect();
        }
    }
}

/// Initialise (or re-initialise, on a brand switch) whichever backend
/// matches `type`. CAMERA_DJI_NANO is the default case, matching the old
/// ternary's fallback behaviour for any unexpected stored value.
static void initBackend(CameraType type) {
    switch (type) {
        case CAMERA_GOPRO:      gpInit();        break;
        case CAMERA_DJI_ACTION: djiActionInit(); break;
        case CAMERA_DJI_NANO:
        default:                djiNanoInit();   break;
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────────────────────────────────────

void camInit() {
    if (_stackReady) return;

    // Initialize thread-safety primitives first
    initMutexes();

    DBG("CAM: Initialising NimBLE stack...");
    NimBLEDevice::init("ESP32-ShutterLink");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    // Bonding enabled (needed for GoPro LE pairing); Just Works IO caps.
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    _stackReady = true;

    initBackend(settingsGet().camera);
    DBG("CAM: Active backend — %s", cameraTypeName(settingsGet().camera));
}

void camUpdate() {
    if (!_stackReady) return;
    switch (settingsGet().camera) {
        case CAMERA_GOPRO:      gpUpdate();        break;
        case CAMERA_DJI_ACTION: djiActionUpdate(); break;
        case CAMERA_DJI_NANO:
        default:                djiNanoUpdate();   break;
    }
}

bool camSendStartRecord() {
    if (!_stackReady) return false;
    switch (settingsGet().camera) {
        case CAMERA_GOPRO:      return gpSendStartRecord();
        case CAMERA_DJI_ACTION: return djiActionSendStartRecord();
        case CAMERA_DJI_NANO:
        default:                return djiNanoSendStartRecord();
    }
}

bool camSendStopRecord() {
    if (!_stackReady) return false;
    switch (settingsGet().camera) {
        case CAMERA_GOPRO:      return gpSendStopRecord();
        case CAMERA_DJI_ACTION: return djiActionSendStopRecord();
        case CAMERA_DJI_NANO:
        default:                return djiNanoSendStopRecord();
    }
}

BleConnectionState camGetState() {
    if (!_stackReady) return BLE_DISCONNECTED;
    switch (settingsGet().camera) {
        case CAMERA_GOPRO:      return gpGetState();
        case CAMERA_DJI_ACTION: return djiActionGetState();
        case CAMERA_DJI_NANO:
        default:                return djiNanoGetState();
    }
}

const CameraTelemetry& camGetTelemetry() {
    static CameraTelemetry empty;
    if (!_stackReady) return empty;
    switch (settingsGet().camera) {
        case CAMERA_GOPRO:      return gpGetTelemetry();
        case CAMERA_DJI_ACTION: return djiActionGetTelemetry();
        case CAMERA_DJI_NANO:
        default:                return djiNanoGetTelemetry();
    }
}

bool camIsReady() {
    if (!_stackReady) return false;
    switch (settingsGet().camera) {
        case CAMERA_GOPRO:      return gpIsReady();
        case CAMERA_DJI_ACTION: return djiActionIsReady();
        case CAMERA_DJI_NANO:
        default:                return djiNanoIsReady();
    }
}

const char* camGetName() {
    return cameraTypeName(settingsGet().camera);
}

const char* camGetLastError() {
    if (!_stackReady) return "";
    switch (settingsGet().camera) {
        case CAMERA_GOPRO:      return gpGetLastError();
        case CAMERA_DJI_ACTION: return djiActionGetLastError();
        case CAMERA_DJI_NANO:
        default:                return djiNanoGetLastError();
    }
}

void camSetCamera(CameraType type) {
    if (_stackReady && type != settingsGet().camera) {
        DBG("CAM: Switching backend to %s", cameraTypeName(type));
        shutdownActiveBackend();

        settingsGet().camera = type;
        settingsSave();

        initBackend(type);
    }
}

void camKick() {
    if (!_stackReady) return;

    // Find the active saved camera (if any).
    ShutterSettings &s = settingsGet();
    int8_t active = -1;
    for (uint8_t i = 0; i < s.camCount; i++) {
        if (s.cams[i].active) { active = (int8_t)i; break; }
    }
    if (active < 0) {
        DBG("CAM: kick requested but no active saved camera");
        return;
    }

    SavedCamera &c = s.cams[active];
    if ((CameraType)c.type != s.camera) {
        camSetCamera((CameraType)c.type);   // Also persists + re-inits backend
    }

    DBG("CAM: kicking connection to %s (%s)", c.mac,
        cameraTypeName((CameraType)c.type));
    switch ((CameraType)c.type) {
        case CAMERA_GOPRO:      gpTargetMac(c.mac);        break;
        case CAMERA_DJI_ACTION: djiActionTargetMac(c.mac); break;
        case CAMERA_DJI_NANO:
        default:                djiNanoTargetMac(c.mac);   break;
    }
}

// Disconnect current camera and stop BLE operations (for UI disconnect)
void camDisconnect() {
    // Thread-safe shutdown
    if (g_stateMutex != NULL) {
        xSemaphoreTake(g_stateMutex, portMAX_DELAY);
    }

    shutdownActiveBackend();

    // Clear active camera flag in settings
    ShutterSettings &s = settingsGet();
    for (uint8_t i = 0; i < s.camCount; i++) {
        if (s.cams[i].active) {
            s.cams[i].active = false;
            break;
        }
    }
    settingsSave();

    DBG("CAM: disconnected active camera");

    if (g_stateMutex != NULL) {
        xSemaphoreGive(g_stateMutex);
    }
}

// User-initiated one-shot discovery scan.  Called from the /api/camera
// {scan:true} endpoint — the ONLY code path that may start a discovery
// scan.  Per-model Update() functions will NOT auto-restart the scan after
// the 5 s window closes (acceptance criterion D).
void camStartUserScan() {
    if (!_stackReady) return;

    // Thread-safe scan start
    if (g_scanMutex != NULL) {
        xSemaphoreTake(g_scanMutex, portMAX_DELAY);
    }

    CameraType t = settingsGet().camera;
    DBG("CAM: user-initiated scan (backend=%s)", cameraTypeName(t));

    switch (t) {
        case CAMERA_GOPRO:      gpStartScan();        break;
        case CAMERA_DJI_ACTION: djiActionStartScan(); break;
        case CAMERA_DJI_NANO:
        default:                djiNanoStartScan();   break;
    }

    if (g_scanMutex != NULL) {
        xSemaphoreGive(g_scanMutex);
    }
}