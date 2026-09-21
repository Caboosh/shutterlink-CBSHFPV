// ============================================================================
// config.h — Project-wide configuration, pin definitions, and tunables
// ============================================================================
// ESP32-C3 ShutterLink: Betaflight ↔ DJI Osmo / GoPro camera bridge.
//
// Runtime-changeable options (camera brand, switch channel, record-on-arm,
// OSD slot contents, Wi-Fi credentials) live in settings.h / NVS and are
// edited from the built-in Web UI.  Only low-level defaults belong here.
// ============================================================================

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// Firmware version string (shown in UI and OTA status endpoint)
#define FIRMWARE_VERSION "v2.1.1-CBSHFPV"

// ──────────────────────────────────────────────────────────────────────────────
// UART / MSP Configuration
// ──────────────────────────────────────────────────────────────────────────────

// Hardware UART pins for Serial1 (connected to Betaflight FC).
// On the ESP32-C3 DevKitM-1, any GPIO can be mapped to UART1.
// Connect FC TX → ESP32 RX_PIN, FC RX → ESP32 TX_PIN.
#define FC_UART_RX_PIN        5   // GPIO5 — receives data FROM the FC
#define FC_UART_TX_PIN        4   // GPIO4 — sends data TO the FC

// UART baud rate — must match Betaflight serial port config.
// Betaflight default MSP baud is 115200.
#define FC_UART_BAUD          115200

// How recently a Web Serial bench command must have arrived over Serial1
// (the FC UART, i.e. a Betaflight serial passthrough session) for
// serialConfigFcUartActive() to report the channel as an active bench
// session rather than a live FC link.
//
// Must comfortably exceed the Configurator's worst-case request cycle,
// not just its steady-state poll interval: docs/app.js polls status every
// 1000ms but gives each request up to a 4000ms timeout before giving up
// (sendCommand()'s default timeoutMs), and requests are serialized, so a
// single dropped/garbled reply can leave a ~4-5s gap between successfully
// parsed lines. If this idle window were shorter than that gap (it used to
// be 3000ms), the very first hiccup would let mspPollRC()/fcStatusUpdate()
// resume, spray raw MSP bytes onto Serial1, corrupt the *next* reply too,
// and repeat forever — a self-sustaining failure loop that looks like the
// Configurator never working, rather than one bad poll. 8000ms gives that
// retry room to succeed cleanly while still resuming normal FC polling
// reasonably promptly once a bench session actually ends.
#define FC_UART_BENCH_IDLE_MS 8000

// Raw bytes per OTA firmware chunk on the Web Serial bench protocol
// (serial_config.cpp's "ota"/"chunk" command). That protocol is line-
// oriented JSON with a 512-byte LineAssembler buffer per port (see
// serial_config.cpp), so firmware bytes travel base64-encoded -- 256 raw
// bytes -> 344 base64 chars, plus ~41 bytes of JSON wrapper, comfortably
// under the 511-byte usable line length with room to spare. Mirrored in
// docs/app.js as OTA_CHUNK_BYTES; keep the two in sync if this changes.
#define OTA_CHUNK_MAX_BYTES 256

// ──────────────────────────────────────────────────────────────────────────────
// Default User Settings (editable at runtime via Web UI / stored in NVS)
// ──────────────────────────────────────────────────────────────────────────────

// Active camera backend: 0 = DJI Osmo Action, 1 = GoPro HERO8 and newer.
#define DEFAULT_CAMERA_TYPE       CAMERA_DJI

// Zero-based index of the RC channel used as the "Record" switch.
// In Betaflight, AUX1 = channel 5 (index 4), AUX4 = channel 8 (index 7), etc.
#define DEFAULT_AUX_CHANNEL_INDEX 11    // AUX5 — change to match your setup

// Threshold (µs) above which the switch is considered ON (record).
#define DEFAULT_RC_THRESHOLD_US   1800

// Debounce duration in milliseconds for the RC switch.
#define DEFAULT_RC_DEBOUNCE_MS    300

// Automatically start recording when the flight controller arms.
#define DEFAULT_RECORD_ON_ARM     true

// When record-on-arm is active, stop recording when the FC disarms.
#define DEFAULT_STOP_ON_DISARM    true

// Configurable Delay (in ms) when disarmed, allowing for a grace period
// when turtling out of a crash. 
#define DEFAULT_STOP_ON_DISARM_DELAY_MS 0 // Default is 0 (old behaviour), change this to increase the delay.

// "Show all nearby devices" in the Camera tab.  When false, the BLE
// discovery filter accepts only known DJI Osmo / GoPro signatures
// (rename-proof: MAC OUI, advertised service UUID, mfr data).  When true,
// every advertiser with a valid address is shown so the user can identify
// their camera manually.  Either way the saved-camera registry is only
// written when the user explicitly taps "Pair & Save".
#define DEFAULT_SCAN_ALL          false

// RC channel used as a Wi-Fi on/off switch. 255 = disabled (AP always on).
#define DEFAULT_WIFI_SWITCH_CH    255

// SoftAP credentials for the Web UI (password empty = open network).
#define WIFI_AP_DEFAULT_SSID      "ShutterLink"
#define WIFI_AP_DEFAULT_PASS      ""

// Default content of Betaflight Custom Message slots 1..4 (OsdSlotContent).
#define DEFAULT_OSD_SLOT_1        OSD_SLOT_CAM_STATUS
#define DEFAULT_OSD_SLOT_2        OSD_SLOT_REC_TIME
#define DEFAULT_OSD_SLOT_3        OSD_SLOT_BATTERY
#define DEFAULT_OSD_SLOT_4        OSD_SLOT_LINK

// Maximum characters per custom message (Betaflight MAX_NAME_LENGTH).
#define OSD_MAX_TEXT_LEN          16

// ──────────────────────────────────────────────────────────────────────────────
// MSP Timing
// ──────────────────────────────────────────────────────────────────────────────

// How often to poll the FC for RC channel data (milliseconds).
#define MSP_RC_POLL_INTERVAL_MS     200

// How often to poll MSP_STATUS for arming state (milliseconds).
#define MSP_STATUS_POLL_INTERVAL_MS 250

// How often to poll MSP_ANALOG for battery voltage (milliseconds).
#define MSP_ANALOG_POLL_INTERVAL_MS 1000

// How often to poll MSP_BOXIDS (ARM bit index can change with config).
#define MSP_BOXIDS_POLL_INTERVAL_MS 5000

// Timeout waiting for an MSP response (milliseconds).
#define MSP_RESPONSE_TIMEOUT_MS     500

// ──────────────────────────────────────────────────────────────────────────────
// OSD Configuration
// ──────────────────────────────────────────────────────────────────────────────

// How often to push custom-message text to the FC (milliseconds).
#define OSD_UPDATE_INTERVAL_MS      500

// Text types for MSP2_SET_TEXT (0x3007) — verified against Betaflight master:
//   1 = PILOT_NAME   2 = CRAFT_NAME   3 = PID_PROFILE   4 = RATE_PROFILE
//   7..10 = CUSTOM_MSG_0..3  (i.e. "Custom Message 1..4" in the OSD tab)
#define MSP2TEXT_CUSTOM_MSG_0   7

// ──────────────────────────────────────────────────────────────────────────────
// BLE Configuration (DJI)
// ──────────────────────────────────────────────────────────────────────────────

// Interval between reconnection attempts (milliseconds).
#define BLE_RECONNECT_INTERVAL_MS 5000

// Keep-alive ping interval to the DJI camera (milliseconds).
#define BLE_KEEPALIVE_INTERVAL_MS 15000

// DJI link-liveness watchdog: the camera pushes DUML telemetry constantly;
// if no notification arrives within this window the link is considered
// wedged and a reconnect is forced (milliseconds).
#define DJI_LINK_STALE_MS         15000

// BLE connection timeout (milliseconds).
#define BLE_CONNECT_TIMEOUT_MS    10000

// GoPro keep-alive interval — Open GoPro spec recommends every ~3 s.
#define GOPRO_KEEPALIVE_INTERVAL_MS 3000

// GoPro status re-query fallback (notifications usually keep values fresh).
#define GOPRO_STATUS_POLL_MS      10000

// ──────────────────────────────────────────────────────────────────────────────
// Status LED (optional, for visual feedback)
// ──────────────────────────────────────────────────────────────────────────────

// Built-in LED on most ESP32-C3 dev boards (GPIO8, active LOW on DevKitM-1).
#define STATUS_LED_PIN        8
#define LED_ACTIVE_LOW        true   // Set to false if your LED is active HIGH

// ──────────────────────────────────────────────────────────────────────────────
// Debug
// ──────────────────────────────────────────────────────────────────────────────

// Uncomment to enable verbose serial debug output on Serial (USB CDC).
#define DEBUG_ENABLED

#ifdef DEBUG_ENABLED
    #define DBG(fmt, ...)   log_printf("[%lu] " fmt "\n", millis(), ##__VA_ARGS__)
#else
    #define DBG(fmt, ...)   ((void)0)
#endif

#endif // CONFIG_H
