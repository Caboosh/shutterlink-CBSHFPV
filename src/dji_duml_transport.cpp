// ============================================================================
// dji_duml_transport.cpp — Shared DUML-over-BLE transport for the DJI Osmo family
// ============================================================================
// PROTOTYPE. See dji_duml_transport.h for what's shared vs. per-model and
// why, and PROTOTYPE_NOTES.md for the split rationale.
// ============================================================================

#include "dji_duml_transport.h"
#include "cam_registry.h"
#include "scan_results.h"

// ──────────────────────────────────────────────────────────────────────────────
// GATT layout + company IDs (brand-wide constants)
// ──────────────────────────────────────────────────────────────────────────────
const NimBLEUUID DJI_DUML_SERVICE_UUID("0000fff0-0000-1000-8000-00805f9b34fb");
const NimBLEUUID DJI_DUML_CHAR_FFF3("0000fff3-0000-1000-8000-00805f9b34fb");
const NimBLEUUID DJI_DUML_CHAR_FFF4("0000fff4-0000-1000-8000-00805f9b34fb");
const NimBLEUUID DJI_DUML_CHAR_FFF5("0000fff5-0000-1000-8000-00805f9b34fb");
const NimBLEUUID DJI_DUML_ADV_SERVICE_UUID("0000fff0-0000-1000-8000-00805f9b34fb");

const uint8_t DJI_DUML_COMPANY_ID[2]   = { 0xAA, 0x08 };
const uint8_t DJI_DUML_XTRA_COMPANY[2] = { 0xAA, 0xF7 };

bool dumlBytesContain(const std::string &haystack, const uint8_t *needle) {
    if (haystack.size() < 2) return false;
    for (size_t i = 0; i + 1 < haystack.size(); i++) {
        if ((uint8_t)haystack[i]     == needle[0] &&
            (uint8_t)haystack[i + 1] == needle[1]) return true;
    }
    return false;
}

// ──────────────────────────────────────────────────────────────────────────────
// Packet framing
// ──────────────────────────────────────────────────────────────────────────────
uint8_t dumlCrc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0x77;  // reflected init
    uint8_t poly = 0x8C; // reflected poly
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x01) crc = (crc >> 1) ^ poly;
            else crc >>= 1;
        }
    }
    return crc;
}

uint16_t dumlCrc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0x3692;  // reflected init
    uint16_t poly = 0x8408; // reflected poly
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) crc = (crc >> 1) ^ poly;
            else crc >>= 1;
        }
    }
    return crc;
}

size_t dumlBuildPacket(uint8_t *buffer, uint8_t sender, uint8_t receiver,
                        uint16_t msgId, uint8_t flags, uint8_t cmdSet, uint8_t cmdId,
                        const uint8_t *payload, size_t payloadLen) {
    size_t idx = 0;
    uint16_t totalLen = 13 + payloadLen;

    buffer[idx++] = 0x55;
    buffer[idx++] = totalLen & 0xFF;
    buffer[idx++] = 0x04 | ((totalLen >> 8) & 0x03); // ver=1
    buffer[idx++] = dumlCrc8(buffer, 3);

    buffer[idx++] = sender;
    buffer[idx++] = receiver;

    buffer[idx++] = (msgId >> 8) & 0xFF;
    buffer[idx++] = msgId & 0xFF;

    buffer[idx++] = flags;
    buffer[idx++] = cmdSet;
    buffer[idx++] = cmdId;

    for (size_t i = 0; i < payloadLen; i++) {
        buffer[idx++] = payload[i];
    }

    uint16_t crc = dumlCrc16(buffer, idx);
    buffer[idx++] = crc & 0xFF;
    buffer[idx++] = (crc >> 8) & 0xFF;

    return idx;
}

bool dumlParseHeader(const uint8_t *pData, size_t length, DumlFrameHeader &out) {
    if (length < 13 || pData[0] != 0x55) return false;
    out.flags  = pData[8];
    out.cmdSet = pData[9];
    out.cmdId  = pData[10];
    return true;
}

void dumlLogNonDuml(const char *charName, const uint8_t *pData, size_t length) {
    char hexBuf[128] = "";
    size_t hexLen = 0;
    for (size_t i = 0; i < length && i < 32; i++) {
        hexLen += snprintf(hexBuf + hexLen, sizeof(hexBuf) - hexLen, "%02X ", pData[i]);
    }
    DBG("DUML: <<< NON-DUML [%s] (%d bytes): %s", charName, length, hexBuf);
}

// ──────────────────────────────────────────────────────────────────────────────
// Pairing handshake + keep-alive
// ──────────────────────────────────────────────────────────────────────────────
void dumlSendPairingArm(DjiDumlSession &s) {
    // Step 1: Write [0x01, 0x00] to fff4 to arm pairing mode.
    uint8_t armPairing[] = {0x01, 0x00};
    if (s.pAuthChar && s.pAuthChar->canWrite()) {
        s.pAuthChar->writeValue(armPairing, 2, true); // Write with response
        DBG("DUML: Wrote [0x01, 0x00] to FFF4 to arm pairing");
        s.pairingArmedTime = millis();
    }
}

void dumlSendPairingPin(DjiDumlSession &s) {
    // Semantics (hardware-verified by the osmosis project, Action 5 Pro /
    // Osmo Nano — do NOT "fix" these into dynamic values):
    //
    //  • identifier ("001749319286102") — the camera stores its approval
    //    UNDER this string. A fixed constant is what makes the "already
    //    paired" fast path (PairingStatus payload[1]==0x01) work, so the
    //    ESP32 reconnects silently instead of re-approval every boot.
    //    Retries must always carry the SAME identifier.
    //  • token ("osmo") — purely cosmetic on cameras: displayed verbatim on
    //    screen (as "OSMO"); the on-screen approve tap is the real gate.
    //    No numeric PIN needs to be read from the camera.
    uint8_t packet[64];
    uint8_t payload[32];
    size_t pIdx = 0;

    const char* id = "001749319286102";
    payload[pIdx++] = strlen(id);
    memcpy(&payload[pIdx], id, strlen(id));
    pIdx += strlen(id);

    const char* pin = "osmo";
    payload[pIdx++] = strlen(pin);
    memcpy(&payload[pIdx], pin, strlen(pin));
    pIdx += strlen(pin);

    // Target: App (0x02) -> WiFi (0x07). Type: flags=0x40, set=0x07, id=0x45
    size_t len = dumlBuildPacket(packet, 0x02, 0x07, s.sequenceCounter++,
                                  0x40, 0x07, 0x45, payload, pIdx);

    // MUST use Write-Without-Response (false) on FFF5.
    if (s.pControlChar && s.pControlChar->canWriteNoResponse()) {
        s.pControlChar->writeValue(packet, len, false);
        DBG("DUML: Sent SetPairingPIN DUML packet to FFF5 (%d bytes)", len);
    }
}

void dumlSendKeepAlive(DjiDumlSession &s) {
    uint8_t packet[32];
    uint8_t payload[] = {0x00};
    size_t len = dumlBuildPacket(packet, 0x02, 0x01, s.sequenceCounter++,
                                  0x40, 0x00, 0xF1, payload, 1);
    if (s.pControlChar && s.pControlChar->canWriteNoResponse()) {
        s.pControlChar->writeValue(packet, len, false);
    }
}

bool dumlHandlePairingFrame(DjiDumlSession &s, const DumlFrameHeader &hdr,
                             const uint8_t *pData, size_t length) {
    // PairingStatus response (flags=0xC0, set=0x07, id=0x45).
    // Payload: [00][status] — status 0x01 = already paired, 0x02 = approval
    // required. Status lives at pData[12] (payload[1]); guard needs >= 13.
    if (hdr.flags == 0xC0 && hdr.cmdSet == 0x07 && hdr.cmdId == 0x45) {
        if (length >= 13) {
            DBG("DUML: Pairing Status Payload: %02X %02X", pData[11], pData[12]);
            if (pData[12] == 0x01) {
                s.sessionEstablished = true;
                s.bleState = BLE_CONNECTED;
                DBG("DUML: *** ALREADY PAIRED - SESSION ESTABLISHED ***");
            } else if (pData[12] == 0x02) {
                DBG("DUML: Approval required - TAP APPROVE ON CAMERA SCREEN!");
            }
        }
        return true;
    }

    // PairingApproved (flags=0x40, set=0x07, id=0x46).
    if (hdr.flags == 0x40 && hdr.cmdSet == 0x07 && hdr.cmdId == 0x46) {
        s.sessionEstablished = true;
        s.bleState = BLE_CONNECTED;
        DBG("DUML: *** PAIRING APPROVED - SESSION ESTABLISHED ***");

        // ACK back: flags=0xC0, set=0x07, id=0x46, payload=0x00.
        uint8_t packet[16];
        uint8_t payload[] = {0x00};
        size_t len = dumlBuildPacket(packet, 0x02, 0x07, s.sequenceCounter++,
                                      0xC0, 0x07, 0x46, payload, 1);
        if (s.pControlChar && s.pControlChar->canWriteNoResponse()) {
            s.pControlChar->writeValue(packet, len, false);
        }
        return true;
    }

    return false;
}

// ──────────────────────────────────────────────────────────────────────────────
// Scan
// ──────────────────────────────────────────────────────────────────────────────
static void dumlScanCompleteCb(NimBLEScanResults results) {
    DBG("DUML: Scan window complete (%d devices)", results.getCount());
    scanResultsMarkComplete();
    scanResultsUpdateSavedStatus();
}

void dumlStartScan(DjiDumlSession &s, NimBLEAdvertisedDeviceCallbacks *scanCallbacks) {
    if (s.bleState == BLE_SCANNING) return;
    DBG("DUML: Starting 5s scan (40%% duty cycle)...");
    s.bleState  = BLE_SCANNING;
    s.doConnect = false;

    // Open the collection gate so scanResultsAdd() accepts entries from the
    // NimBLE callback. Without this the gate stays closed and the
    // callback's results are dropped.
    scanResultsStart();
    camRegistryClearDiscovered();  // New scan: wipe in-RAM discovered list

    NimBLEScan *pScan = NimBLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(scanCallbacks, false);
    pScan->setActiveScan(true);
    // 40ms window in 100ms interval = 40% radio duty cycle, leaving 60%
    // airtime for the Wi-Fi SoftAP to keep beaconing.
    pScan->setInterval(100);
    pScan->setWindow(40);
    pScan->setMaxResults(0);
    // ONE-SHOT 5s window. Must pass dumlScanCompleteCb explicitly — see the
    // original dji_camera.cpp's comment on the blocking-overload footgun
    // this avoids (start(duration, is_continue) freezes loop() and never
    // fires the completion callback, wedging scanResults' _scanning flag).
    pScan->start(5, dumlScanCompleteCb, false);
}

// ──────────────────────────────────────────────────────────────────────────────
// Connection lifecycle
// ──────────────────────────────────────────────────────────────────────────────
bool dumlConnectToCamera(DjiDumlSession &s, CameraTelemetry &telemetry,
                          const char *modelName,
                          NimBLEClientCallbacks *clientCallbacks,
                          DumlNotifyCallback notifyCallback) {
    if (!s.hasTargetAddress) return false;
    DBG("DUML: Connecting to target %s...", s.targetAddress.toString().c_str());
    s.bleState = BLE_CONNECTING;
    s.resetError();

    if (!s.pClient) {
        s.pClient = NimBLEDevice::createClient();
        s.pClient->setClientCallbacks(clientCallbacks, false);
        s.pClient->setConnectTimeout(BLE_CONNECT_TIMEOUT_MS / 1000);
    }

    if (!s.pClient->connect(s.targetAddress)) {
        DBG("DUML: Connection failed!");
        s.bleState = BLE_DISCONNECTED;
        s.setError("connect failed");
        return false;
    }

    DBG("DUML: Connected! Discovering GATT services...");
    NimBLERemoteService *pService = s.pClient->getService(DJI_DUML_SERVICE_UUID);
    if (!pService) {
        DBG("DUML: Service 0xFFF0 not found! Disconnecting.");
        s.pClient->disconnect();
        s.bleState = BLE_DISCONNECTED;
        s.setError("not a DJI Osmo camera");
        return false;
    }

    s.pTelemetryChar = pService->getCharacteristic(DJI_DUML_CHAR_FFF3);
    s.pAuthChar      = pService->getCharacteristic(DJI_DUML_CHAR_FFF4);
    s.pControlChar   = pService->getCharacteristic(DJI_DUML_CHAR_FFF5);

    if (!s.pControlChar || !s.pAuthChar) {
        DBG("DUML: Missing FFF4 or FFF5 characteristics!");
        s.pClient->disconnect();
        s.bleState = BLE_DISCONNECTED;
        s.setError("DJI service missing chars");
        return false;
    }

    if (s.pAuthChar && s.pAuthChar->canNotify()) {
        s.pAuthChar->subscribe(true, notifyCallback);
        DBG("DUML: Subscribed to FFF4 notifications");
    }
    if (s.pControlChar && s.pControlChar->canNotify()) {
        s.pControlChar->subscribe(true, notifyCallback);
        DBG("DUML: Subscribed to FFF5 notifications");
    }

    strlcpy(telemetry.model, modelName, sizeof(telemetry.model));
    s.lastRxMs = millis();  // Don't trip the staleness watchdog right after connect

    s.bleState = BLE_AUTHENTICATING;
    s.sessionEstablished = false;
    s.authStartMs = millis();  // Fresh timeout — lastReconnectAttempt is stale
                                // when we got here via direct connect (switching)
    dumlSendPairingArm(s);
    return true;
}

void dumlUpdate(DjiDumlSession &s, CameraTelemetry &telemetry, CameraType camType,
                 const char *modelName, bool skipAutoReconnect,
                 NimBLEClientCallbacks *clientCallbacks,
                 DumlNotifyCallback notifyCallback) {
    uint32_t now = millis();

    switch (s.bleState) {
        case BLE_DISCONNECTED:
            // A pending direct-connect (Web UI "Use" / camKick) must be
            // honoured immediately — don't wait out the reconnect interval,
            // and never let this be skipped by the bench-session guard.
            if (s.doConnect) {
                dumlConnectToCamera(s, telemetry, modelName, clientCallbacks, notifyCallback);
                s.doConnect = false;
                break;
            }
            // NO discovery scanning here. If we have a saved+active camera
            // of this type, target-connect to it directly every
            // BLE_RECONNECT_INTERVAL_MS — saves the single 2.4GHz radio
            // from time-sharing BLE scanning with the SoftAP's beaconing.
            //
            // Skipped entirely while a Web-Serial bench session is active
            // over the FC-UART passthrough: this connect attempt blocks for
            // up to BLE_CONNECT_TIMEOUT_MS if the camera is unreachable,
            // which would outlast FC_UART_BENCH_IDLE_MS and corrupt the
            // bench console's Serial1 JSON framing — the same failure class
            // already fixed for this exact scenario.
            if (!skipAutoReconnect) {
                char mac[18];
                if (camRegistryActiveMac(camType, mac, sizeof(mac)) &&
                    (now - s.lastReconnectAttempt) >= BLE_RECONNECT_INTERVAL_MS) {
                    s.lastReconnectAttempt = now;
                    s.targetAddress = NimBLEAddress(mac);
                    s.hasTargetAddress = true;
                    DBG("DUML: direct reconnect to %s (no scan)", mac);
                    dumlConnectToCamera(s, telemetry, modelName, clientCallbacks, notifyCallback);
                }
            }
            break;

        case BLE_SCANNING:
            if (s.doConnect) {
                dumlConnectToCamera(s, telemetry, modelName, clientCallbacks, notifyCallback);
                s.doConnect = false;
                break;
            }
            // ONE-SHOT scan window. When NimBLE finishes, isScanning() flips
            // false — transition back to DISCONNECTED so the next tick can
            // attempt a direct reconnect. NEVER restart the scan here;
            // discovery is strictly user-initiated.
            if (!NimBLEDevice::getScan()->isScanning()) {
                s.bleState = BLE_DISCONNECTED;
                s.lastReconnectAttempt = now;
            }
            break;

        case BLE_AUTHENTICATING:
            // Wait 200ms after arming, then send the PIN packet.
            if (now - s.pairingArmedTime >= 200 && s.pairingArmedTime > 0) {
                s.pairingArmedTime = 0;  // Only send once
                dumlSendPairingPin(s);
            }
            if (now - s.authStartMs >= 30000) {
                DBG("DUML: Auth timeout (30s) — resetting");
                if (s.pClient) s.pClient->disconnect();
                s.bleState = BLE_DISCONNECTED;
                s.lastReconnectAttempt = now;
            }
            break;

        case BLE_CONNECTED:
            if (!s.pClient || !s.pClient->isConnected()) {
                s.bleState = BLE_DISCONNECTED;
                break;
            }
            // Liveness watchdog: the camera pushes DUML notifications
            // constantly (~1Hz GeneralStatus and up). If nothing arrives for
            // a long while the link is wedged even if the BLE stack hasn't
            // noticed yet — force a reconnect.
            if (now - s.lastRxMs >= DJI_LINK_STALE_MS) {
                DBG("DUML: No camera traffic for %d s — forcing reconnect",
                    DJI_LINK_STALE_MS / 1000);
                s.pClient->disconnect();
                s.bleState = BLE_DISCONNECTED;
                break;
            }
            if (now - s.lastKeepAlive >= BLE_KEEPALIVE_INTERVAL_MS) {
                s.lastKeepAlive = now;
                dumlSendKeepAlive(s);
            }
            break;
    }
}

void dumlTargetMac(DjiDumlSession &s, const char *mac, const char *logPrefix) {
    if (!mac || !*mac) return;
    NimBLEScan *pScan = NimBLEDevice::getScan();
    if (pScan && pScan->isScanning()) pScan->stop();
    if (s.pClient && s.pClient->isConnected()) s.pClient->disconnect();
    s.targetAddress    = NimBLEAddress(mac);
    s.hasTargetAddress = true;
    s.doConnect        = true;
    DBG("%s: targeting %s (direct connect)", logPrefix, mac);
}