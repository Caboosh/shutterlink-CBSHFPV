# shutterlink

Open-source CamLink alternative: an ESP32-C3 BLE bridge that turns a radio
switch (or your arming switch!) into record control for **DJI Osmo Action**
and **GoPro HERO8+** cameras, and pushes live camera telemetry into the
Betaflight OSD - with a built-in **Glassmorphism Web UI**.

> **Requires a Betaflight build with Custom Message OSD elements**
> (`MSP2_SET_TEXT` 0x3007, custom message types 7-10). Betaflight 4.x and
> 4.5 are **not** supported.
>
> **Not affiliated with or endorsed by DJI, GoPro or itsFPV.**
> All protocols based on public documentation / community reverse-engineering.

---

# CAMERA SUPPORT
Upstream Shutterlink supports DJI and GoPro, so does my fork, the difference is
i have used my Osmo Nano to get the DUML over BLE telemetry functional, this will
need testing on the "mainline" Osmo Action's as i cannot confirm if the telemetry
is the same on them as it is on the Osmo Nano.

## Features

- **RC-switch record control** - start/stop recording from *any* AUX channel,
  read straight from Betaflight via MSP. Channel, threshold and debounce are
  configurable from the Web UI - no recompiling.
- **Record-on-arm** - optional auto-start when the FC arms, with optional
  stop-on-disarm. Toggle it in the Web UI.
- **Parallel OSD telemetry on all 4 Custom Messages** - assign any of
  Cam status / Rec time / Battery / Link state / FC battery / Arm state / Off
  to each slot via the Web UI. Pushed with `MSP2_SET_TEXT` (MSP v2, `0x3007`).
- **Two camera backends** - DJI Osmo Action (DUML over BLE) and GoPro HERO8
  through HERO13 (official Open GoPro BLE API), switchable at runtime.
- **Saved-camera registry + discovery scanning** - scan for nearby cameras
  from the Web UI, **Pair & Save** the one that's yours (up to 4 saved), and
  the ESP32 auto-reconnects to it forever after - no ghost devices, nothing
  is written to flash without your explicit consent.
- **Wi-Fi power switch** - assign a spare AUX channel to toggle the Web-UI
  hotspot on/off in flight (saves ~60-100 mA; the BLE camera link keeps
  running).
- **Robust BLE link** - auto-reconnect, keep-alive, non-blocking state
  machine. Camera commands are absolute start/stop (never toggles), so
  retries after reconnects are always safe.
- **Built-in Web UI** - connect to the ESP32's Wi-Fi network and a modern
  Glassmorphism dashboard opens automatically (captive portal): live status,
  manual REC/STOP buttons, all configuration, OTA firmware updates, dark &
  light mode, frosted-glass SVG icon set.
- **Web Serial bench console** - a GitHub Pages-hosted page (`docs/`) that
  talks straight to the board over USB (no Wi-Fi needed) for bench setup and
  debugging: live status, all the same configuration forms as the field Web
  UI, unrestricted MSP passthrough, OSD live preview, OTA firmware flashing
  (works through a live Betaflight passthrough session too — no unplugging
  required), and a raw serial log panel.
- **Persistent settings** - everything you configure lives in NVS flash.
- **Status LED patterns** - know your link state at a glance on the bench.
- **Modular firmware** - `msp_protocol` (FC side), `fc_status` (arming),
  `dji_camera` / `gopro_camera` (camera side), `camera_manager` (dispatch),
  `cam_registry` + `scan_results` (pairing), `recorder` (decision engine),
  `osd_slots` (OSD), `wifiswitch` (AP power), `api_core` + `json_scan`
  (shared config-surface logic), `web_server` + `web_assets` (Wi-Fi
  transport + UI), `serial_config` (USB Web Serial transport).

---

## How It Works

```
 +-----------+  RC frames   +------------------+ MSP (UART) +---------------+
 | Radio TX  | -----------> | Flight Controller| <--------> |   ESP32-C3    |
 +-----------+              |   (Betaflight)   |  Serial1   | (ShutterLink) |
                            +------------------+            +-------+-------+
                                     ^                              | BLE
                                     |                              +-> DJI DUML
                          HD OSD custom                             +-> GoPro Open
                          messages 1-4 <-- telemetry --+                BLE API
                                                                       |
                 phone/PC  <-- Wi-Fi AP + captive portal --------------+
                                  (Web UI)
                 bench PC  <-- USB Web Serial (docs/ bench console) ---+
```

1. **Switch / arm to ESP32:** polls `MSP_RC` every 200 ms; watches your
   configured AUX channel (debounced, configurable threshold). Polls
   `MSP_STATUS` + `MSP_BOXIDS` for arming state.
2. **ESP32 to Camera:** desired-recording = switch ON OR (record-on-arm AND
   armed). On transitions it sends the camera's absolute start/stop command
   over BLE. Manual buttons in the Web UI (or bench console) do the same.
3. **Camera to OSD:** up to four independent strings pushed on change (checked
   every 500 ms) into Betaflight Custom Messages 1-4 via `MSP2_SET_TEXT`.
4. **Web UI:** the ESP32 runs a SoftAP (default SSID `ShutterLink`, password
   `shutterlink`). Browse to `http://192.168.4.1`.
5. **Bench console:** the same configuration surface is also reachable over
   USB from `docs/index.html` (see "Web Serial Bench Console" below) — useful
   on the bench without joining the ESP32's Wi-Fi network, and for reading
   MSP without the Wi-Fi API's read-only restriction.

---

## Hardware Requirements

| Item | Notes |
|---|---|
| **ESP32-C3** board | DevKitM-1 / "Super Mini" class. BLE 5.0, NimBLE stack. |
| **Flight controller** | Betaflight with Custom Message OSD elements + one free UART. |
| **Camera** | DJI Osmo Action family **or** GoPro HERO8/9/10/11/12/13. |
| Wiring | 4 wires: 5 V, GND, FC TX, FC RX. |

> **ESP32-C3 "Super Mini" / native-USB boards:** these have no separate
> USB-UART bridge chip — the chip's native USB peripheral is used directly.
> `platformio.ini` must set `-D ARDUINO_USB_MODE=1` and
> `-D ARDUINO_USB_CDC_ON_BOOT=1` in `build_flags` (already set in this
> repo's `platformio.ini`), or Arduino's `Serial` object binds to the
> disconnected UART0 peripheral instead of the native USB-CDC your monitor
> is actually attached to. Symptom if this is ever missing: `DBG()` output
> still appears in the serial monitor fine (it's routed via the IDF console
> over USB-Serial-JTAG regardless), but anything you *type* into the
> monitor is silently lost — `Serial.read()` never sees it, even at the
> byte level. This affects both the plain debug console and the Web Serial
> bench protocol.

---

## Step-by-Step Setup Guide

### Step 1 - Wire the ESP32 to the flight controller

Four wires between the ESP32-C3 and a free UART on your FC:

| ESP32-C3 | Flight Controller | Notes |
|---|---|---|
| **GPIO5** (RX) | UART **TX** | FC transmits → ESP32 receives |
| **GPIO4** (TX) | UART **RX** | ESP32 transmits → FC receives |
| **5 V** | 5 V | Or power the ESP32 from its own USB while bench-testing |
| **GND** | GND | Common ground is mandatory |

> TX goes to RX on both ends - it's a crossover, not pin-to-pin. Double-check
> 5 V tolerance: most ESP32-C3 dev boards accept 5 V on the VIN/5V pin only.

The on-board status LED (GPIO8 on most C3 boards) blinks the link state:

| Pattern | Meaning |
|---|---|
| Slow blink (1 s) | Disconnected / scanning |
| Fast blink (200 ms) | Connecting / authenticating |
| Solid | Connected, camera standby |
| Medium blink (500 ms) | Recording desired |

### Step 2 - Configure Betaflight

1. **Ports tab:** enable **MSP** on the UART wired to the ESP32
   (115200 baud, leave everything else off for that port).
2. **OSD tab:** place **Custom Message 1-4** elements wherever you want them
   on your screen - position comes from Betaflight, *content* is pushed live
   by ShutterLink.
3. Assign an AUX channel (on your radio's mixes tab) as your record switch if
   you want switch control. You don't need to create a Betaflight *mode* for
   it - ShutterLink reads the raw RC channels.

> Remember: your Betaflight build must support **Custom Message OSD
> elements** (`MSP2_SET_TEXT`). Stock 4.x / 4.5 does not.

### Step 3 - Build & flash the firmware

```bash
# 1. Clone
git clone https://github.com/caboosh/shutterlink-CBSHFPV.git
cd shutterlink

# 2. Build
pio run

# 3. Flash (hold BOOT on some C3 boards if upload doesn't start)
pio run -t upload

# 4. Watch it work
pio device monitor -b 115200
```

Dependencies (handled by PlatformIO): `h2zero/NimBLE-Arduino @ ^1.4.1`.
Partition table switched to `min_spiffs.csv` (BLE + Wi-Fi + web UI need the
bigger app slot). `ARDUINO_USB_MODE` / `ARDUINO_USB_CDC_ON_BOOT` build flags
are required for native-USB C3 boards — see the Hardware Requirements note
above.

### Step 4 - Connect to the Web UI

1. Power the ESP32 (FC 5 V or USB).
2. On your phone or PC, join the Wi-Fi network **`ShutterLink`**
   (password: **`shutterlink`**).
3. The captive portal opens automatically on most devices; otherwise browse
   to **`http://192.168.4.1`**.
4. You land on the **Dashboard** with live status cards.

### Step 5 - Pair your camera

1. Power the camera on and put it within a few metres of the ESP32.
2. In the Web UI open the **Camera** tab.
3. Tap **Scan for Cameras** - a one-shot 5-second BLE scan runs and lists
   nearby cameras (brand is auto-detected; strongest signal first).
   - Camera not in the list? Tap **"Camera not listed? Show all nearby
     devices"** and pick yours by signal strength (hold it within 1 m -
     the strongest RSSI is usually yours).
4. Tap **Pair & Save** on your camera. This is the only moment anything is
   written to flash - devices the radio merely *sees* are never persisted.
5. Watch the camera screen:
   - **GoPro:** the first ever connection asks for a **one-time approval
     tap** on the camera's own screen. After that, reconnection is silent.
   - **DJI:** an approve prompt may appear once - tap approve.
6. The LED goes solid and the Dashboard shows the camera model + battery.
   From now on the ESP32 reconnects to this camera automatically, no scan
   needed.

Up to **4 cameras** can be saved; switch the active one (or remove entries)
from the **Saved cameras** card in the same tab.

### Step 6 - Configure the record switch

1. Open the **Controls** tab.
2. Pick your **switch channel** (CH5-16 / AUX1-12), the **ON threshold**
   (default 1500 us) and **debounce** (default 300 ms) - hit **Save switch
   settings**.
3. Optional: enable **Record on arm** (+ **Stop on disarm**) so recording
   follows the arming state instead of a switch.
4. Flip the switch and watch the Dashboard: the record-switch card should
   flip from IDLE to ON, and the camera starts/stops recording.

### Step 7 - (Optional) Fine-tune OSD & Wi-Fi

- **OSD tab:** assign content (Cam status / Rec time / Battery / Link / FC
  battery / Arm state / Off) to Custom Messages 1-4 with live previews.
- **Controls tab:** change the Wi-Fi SSID/password, or assign a spare AUX
  channel as a **Wi-Fi radio switch** - flip it low in flight and the hotspot
  powers down to save ~60-100 mA (BLE camera control keeps running; the AP
  always boots ON so you can't lock yourself out).

Done - go fly.

---

## Web UI Guide

Connect a phone/PC to the ShutterLink Wi-Fi network - the captive portal
opens automatically on most devices, otherwise browse to
`http://192.168.4.1`. Five tabs along the top:

| Tab | What you can do |
|---|---|
| **Dashboard** | Live link/camera/FC status, big START / STOP buttons, live preview of the four OSD strings, camera battery, record-switch value, FC battery & arm state, heap/uptime. |
| **Controls** | Record switch channel (CH5-16/AUX), ON threshold, debounce, **record-on-arm + stop-on-disarm toggles**, Wi-Fi AP credentials, Wi-Fi radio switch channel. |
| **Camera** | Active connection status, saved-camera registry (select/remove, up to 4), discovery scan with **Pair & Save**, "show all nearby devices" fallback. |
| **OSD** | Assign content to Custom Message slots 1-4 with live previews. |
| **FC / System** | Betaflight identity (API/firmware/board), battery, arm state, read-only **MSP console** (passthrough to your FC), free heap/uptime/firmware version, reboot, **OTA firmware update** (.bin upload). |

### Navigating the Camera tab (pairing in detail)

The Camera tab has three cards:

1. **Active connection** - which camera is connected right now, its model,
   battery and link state.
2. **Saved cameras** - your NVS registry (max 4). Tap an entry to make it
   active and connect immediately; remove entries you no longer own.
3. **Discover new camera** - the pairing workflow:
   - Press **Scan for Cameras** (5-second one-shot window, results sorted by
     signal strength, type auto-detected).
   - Tap **Pair & Save** on your device. The ESP32 saves it to flash,
     selects it and connects immediately.
   - Nothing ever auto-connects except the *saved, active* camera - a
     camera that merely appears in a scan is never persisted.
   - If your camera doesn't match the auto-detection filters, use the
     **Show all nearby devices** link and identify it by RSSI.

First-connection approvals happen on the **camera's own screen** (one tap
for GoPro, sometimes one for DJI) - after that, reconnection is silent.

The UI is a single-page app embedded in the firmware (PROGMEM, ~33 KB):
frosted glass cards, backdrop blur, animated gradient background, smooth
transitions, and a dark/light mode toggle that persists in your browser.

### Can it configure Betaflight from the Web UI?

A full Configurator port is not realistic on an ESP32-C3 (the real
Configurator is a multi-megabyte desktop-class app and needs a serial/WebSocket
bridge). But the firmware already speaks MSP in both directions over the FC
UART, so a lightweight config panel - reading/writing selected settings via
MSP passthrough (think "Betaflight Lua scripts in a browser") - is absolutely
feasible as a future subtab. The FC/System tab's MSP console already
demonstrates live MSP data flowing from your FC (read-only allowlist today).

### Updating firmware over Wi-Fi (OTA)

FC / System tab → **Firmware Update (OTA)**: upload a `.bin` from a GitHub
Release and the ESP32 flashes it and reboots automatically. Don't close the
page or power off mid-upload; if it fails, re-flash over USB.

---

## Web Serial Bench Console

`docs/` (served as GitHub Pages, e.g. `https://<user>.github.io/shutterlink/`)
is a standalone page that talks to the ESP32 directly over USB using the
browser's [Web Serial API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Serial_API) —
no Wi-Fi network required. Useful for bench setup, and it shares a raw
`DBG()` log panel you don't get from the field Web UI.

**Requirements:** a Chromium-based desktop browser (Chrome, Edge, Opera) and
a secure context — `https://` (GitHub Pages is fine) or `http://localhost`.
Web Serial does **not** work opened as a `file://` URL. For local testing,
serve the folder (`python3 -m http.server` from `docs/`, then visit
`http://localhost:8000`).

**Protocol:** `serial_config.cpp` implements a line-delimited JSON protocol
over the same USB port used for flashing and `DBG()` output (`Serial`,
115200 baud). `DBG()` lines never start with `{`, so the console filters on
that to tell protocol replies from debug noise — both share the port with
no conflict.

| Host → device | Purpose |
|---|---|
| `{"path":"ping"}` | Liveness check — replies with device name + firmware version + which port answered (`"via":"usb"` or `"fc-uart"`) |
| `{"path":"status"}` | Same status payload as `GET /api/status` |
| `{"path":"settings", ...}` | Same fields as `POST /api/settings` |
| `{"path":"camera", ...}` | Same fields as `POST /api/camera` |
| `{"path":"command","cmd":"start"\|"stop"\|"reboot"}` | Same as `POST /api/command` |
| `{"path":"msp","cmd":<u8>}` | MSP passthrough — **unrestricted** here (unlike `/api/msp`'s read-only allowlist), since this channel requires a physical USB cable, a much higher trust bar than the Wi-Fi AP. Only works over the direct USB port — see below |
| `{"path":"ota","action":"begin","size":<u32, optional>}` | Start a firmware update — see "Flashing firmware over Web Serial" below |
| `{"path":"ota","action":"chunk","data":"<base64>"}` | Write one chunk (≤ `OTA_CHUNK_MAX_BYTES` raw bytes, base64-encoded) |
| `{"path":"ota","action":"end"}` | Finalize and reboot into the new firmware |
| `{"path":"ota","action":"abort"}` | Cancel an in-progress update, leaving the currently-running firmware untouched |

Only one request is in flight at a time — send a command and wait for the
next `{`-prefixed reply line before sending another. `/api/scan` is
intentionally out of scope for this transport (scan results are already
embedded in the status response).

### Reaching it without a USB cable to the C3 (Betaflight passthrough)

Once the ESP32-C3 is installed in the quad, its own USB-C port is often
buried or hard to reach. Since the C3 is already wired to a free FC UART for
MSP (see Step 1), the same bench-config protocol also listens on that UART
(`Serial1`), reachable through **Betaflight's serial passthrough** feature
via the FC's own, more accessible USB port — no extra cable, no
disassembly.

**One click, no Betaflight Configurator needed:** the bench console
(`docs/`) drives this itself. Expand **"C3 not reachable over USB? Connect
via Betaflight passthrough instead"**, enter the **FC UART number** wired to
the C3 (as printed on the Ports tab, e.g. `3` for UART3 — not a zero-based
index, the page converts that for you) and the baud rate (matches
`FC_UART_BAUD` in `config.h`, 115200 by default), then hit **Connect via FC
passthrough**. Behind the scenes the page:

1. Opens the FC's own USB serial port directly via Web Serial.
2. Sends `#` to force a live MSP connection into CLI mode (harmless if it's
   already there — Betaflight just reprints the prompt), the same nudge
   Betaflight Configurator's own CLI tab uses.
3. Sends `serial` and reads the reply to work out which argument style
   `serialpassthrough` wants on *this* firmware — see the version note
   below, this changed recently and the console no longer guesses.
4. Sends `serialpassthrough <target> <baud>` with whatever it just
   determined `<target>` to be, and watches for the FC's own
   `Forwarding, power cycle to exit.` confirmation (or an `Invalid port`
   error, which it surfaces instead of pressing on blind). Betaflight
   bridges its USB connection straight through to that UART and **stops
   flying** — this is bench-only, disarmed, never in the air.
5. Keeps using that *same* already-open connection as the ShutterLink JSON
   channel from that point on — no second app, no picking a different COM
   port, no reconnecting.

This is the same trick [ExpressLRS's own flashing tool](https://github.com/ExpressLRS/ExpressLRS)
uses to reach a receiver wired to an FC UART (`BFinitPassthrough.py`): plain
CLI automation, not a special binary MSP command. A `{"path":"ping"}` reply
with `"via":"fc-uart"` (shown in the console log right after connecting)
confirms you're actually talking to the C3 through the bridge and not, say,
a stale MSP session that never switched over.

> **`serialpassthrough` argument syntax varies by Betaflight version** —
> confirmed against real hardware, not just docs:
> - **pre-25.12:** a zero-based numeric port id, e.g. `serialpassthrough 2
>   115200` for UART3. The `serial` command's own listing was numeric-only
>   too.
> - **25.12+:** `serial` now names each port (`serial UART4 1 115200 …`)
>   and `serialpassthrough` takes that same name directly —
>   `serialpassthrough UART4 115200`. A bare numeric id now fails with
>   `Invalid port1`.
>
> The bench console runs `serial` itself first and reads which style this
> firmware actually prints, rather than assuming one — see
> `resolvePassthroughTarget()` in `docs/app.js` if you're debugging this by
> hand.

If you'd rather drive it by hand (e.g. debugging a passthrough issue, or a
browser build where the automated CLI nudge doesn't behave): open Betaflight
Configurator's CLI tab yourself, run `serial` to see which style your
firmware wants, then `serialpassthrough <target> <baud>` per the version
note above, close the Configurator so the OS COM port is free, then use the
plain **"Connect over USB"** button and pick that same COM port when
prompted — from the browser's side it's an identical serial connection
either way.

Caveats while a passthrough session is open: the FC isn't running its own
firmware, so the ESP32 loses live FC telemetry (arm state, RC-switch
polling, OSD pushes) until you **power-cycle the FC** — passthrough doesn't
end on its own, and a normal reboot/reconnect isn't enough — and
`{"path":"msp",...}` on this channel always replies with an error, since
there's no independent live FC left on the wire to bounce the MSP request
off. Use the direct USB cable for MSP passthrough; use the
FC-UART/passthrough route for everything else, including firmware updates
(`ping`/`status`/`settings`/`camera`/`command`/`ota`).

The logic behind both transports lives once, in `api_core.cpp` — `web_server.cpp`'s
HTTP handlers and `serial_config.cpp`'s dispatcher are both thin wrappers
around the same `apiBuildStatusJson()` / `apiApplySettings()` /
`apiApplyCamera()` / `apiApplyCommand()` functions, so the two can't drift
out of sync with each other.

### Flashing firmware over Web Serial (no unplugging required)

The bench console's **Firmware update** card flashes a new `firmware.bin`
straight over whichever serial connection is already open — the direct USB
cable, *or* a live Betaflight passthrough session. That second option is
the interesting one: once the C3 is installed in the quad, you can update
its firmware through the FC's own USB port, without ever unplugging the C3
or pulling it off the frame — the same idea
[ExpressLRS's own configurator](https://github.com/ExpressLRS/ExpressLRS)
uses to reflash a receiver wired to an FC UART.

This is **not** a ROM-bootloader flash (that needs hardware control of the
C3's boot-strap and reset pins, which a plain UART bridge can't reach) — it's
an application-level update using the same
[`Update.h`](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/update.html)
mechanism the Wi-Fi Web UI's OTA upload already uses, writing into the
board's spare OTA partition (`min_spiffs.csv` gives it two). Pick a `.bin`
built with `pio run`, hit **Flash firmware**, and the console drives
`{"path":"ota",...}` begin/chunk/end over the line protocol above — one
`OTA_CHUNK_MAX_BYTES`-sized, base64-encoded chunk per request/reply round
trip, same serialized command queue as every other bench command. The
device only switches over to the new image once the write verifies clean
(`Update.end(true)`), so a failed transfer or a cancel just leaves the
currently-running firmware in place.

What happens after a successful flash differs by connection: over the
direct USB cable, the reboot is a real USB re-enumeration, so reconnect
manually once the board reappears. Over an FC passthrough session, only the
C3 on the far end of the UART reboots — the FC's own USB connection to your
PC never drops — so the console just starts getting replies again on its
own once the new firmware's `setup()` runs.

Since this is the C3's *own* update mechanism, it has to already be running
ShutterLink firmware that includes the `ota` serial command before you can
use it this way — flash the first build via the normal USB/PlatformIO
workflow (Step 1), and every build after that can go over the air, on the
bench or through the FC.

---

## Camera Support Notes

### DJI Osmo Action (DUML over BLE)

Service `0xFFF0`; DUML frames written without response to `0xFFF5`,
notifications on `0xFFF4`. App-level pairing (`0x07/0x45`), not OS bonding -
approve prompts on the camera screen when they appear.

```
[0x55][len_lo][(ver<<2|len_hi)][crc8][sender][receiver]
[msg_id BE][flags][cmdSet][cmdId][payload...][crc16 LE]
```

Record = cmdSet `0x0A`, cmdId `0x0D`, payload `0x01` start / `0x00` stop.

### GoPro HERO8+ (Open GoPro BLE)

Official public API: service `0xFEA6`, command characteristic
`b5f90072-aa8d-11e3-9046-0002a5d5c51b`.

- Start:  `03 1A {%230%22shutter%22%3Atrue}`
- Stop:   `03 1B {%230%22shutter%22%3Afalse}`
- Keep-alive every ~3 s: `02 01 42`
- Status registration for battery % + encoding state.

First connection: put the camera into pairing mode and approve the prompt on
its screen once; the ESP32 bonds and reconnects silently afterwards.
HERO5-7 use a different legacy protocol and are not supported.

---

## Configuration

Runtime settings live in NVS and are edited from the Web UI (or bench
console). Compile-time defaults are in `src/config.h`:

| Define | Default | Purpose |
|---|---|---|
| `FC_UART_RX_PIN` / `FC_UART_TX_PIN` | 20 / 21 | UART pins to the FC |
| `DEFAULT_CAMERA_TYPE` | DJI | Initial camera backend |
| `DEFAULT_AUX_CHANNEL_INDEX` | 8 | RC channel used as record switch |
| `DEFAULT_RC_THRESHOLD_US` | 1500 | us above = ON |
| `DEFAULT_RC_DEBOUNCE_MS` | 300 | Switch debounce |
| `DEFAULT_RECORD_ON_ARM` | false | Auto-record on arming |
| `DEFAULT_STOP_ON_DISARM` | true | Stop when FC disarms |
| `DEFAULT_SCAN_ALL` | false | Show all BLE advertisers during discovery |
| `DEFAULT_WIFI_SWITCH_CH` | 255 (off) | AUX channel toggling the Wi-Fi AP |
| `WIFI_AP_DEFAULT_SSID` / `_PASS` | ShutterLink / shutterlink | Web UI hotspot |
| `DEFAULT_OSD_SLOT_1..4` | status/time/batt/link | Custom Message contents |
| `STATUS_LED_PIN` | 8 | Onboard LED |

## Repository Structure

```
shutterlink/
+-- platformio.ini          # ESP32-C3 build config + NimBLE dependency +
|                            #   native-USB CDC flags
+-- README.md
+-- docs/                   # GitHub Pages Web Serial bench console
|   +-- index.html
|   +-- style.css
|   +-- app.js
+-- .vscode/                # IntelliSense / debug configs (PlatformIO)
+-- src/
    +-- config.h            # Pins, defaults, timings, debug switch
    +-- settings.h/.cpp     # NVS-backed runtime configuration + saved-camera
    |                       #   registry types (ShutterSettings)
    +-- main.cpp            # Non-blocking loop orchestration
    +-- msp_protocol.h/.cpp # MSP v1 parser + MSP v2 SET_TEXT (CRC-DVB-S2)
    +-- fc_status.h/.cpp    # Arm detection, FC battery/identity polling
    +-- camera_common.h     # Shared camera types (backend interface)
    +-- dji_camera.h/.cpp   # DJI Osmo DUML-over-BLE backend + discovery
    |                       #   filters (name/OUI/service/mfr-data)
    +-- gopro_camera.h/.cpp # GoPro Open BLE backend
    +-- camera_manager.h/.cpp  # Backend dispatcher (runtime switching,
    |                          # user-initiated scans, reconnect kicks)
    +-- scan_results.h/.cpp    # In-RAM BLE scan results collector (Web UI
    |                          #   "Discovered cameras" card, RSSI-sorted,
    |                          #   TTL eviction, max 10)
    +-- cam_registry.h/.cpp   # Two-tier camera registry: in-RAM discovered
    |                          #   list + NVS "saved" list (Pair & Save only;
    |                          #   kills the ghost-device bug; max 4 saved)
    +-- recorder.h/.cpp     # Switch/arm/manual -> record decision engine
    +-- osd_slots.h/.cpp    # Custom Message 1-4 content manager
    +-- wifiswitch.h/.cpp   # AUX-switch power control for the Wi-Fi AP
    +-- json_scan.h/.cpp    # Tiny flat-JSON reader shared by both transports
    +-- api_core.h/.cpp     # Transport-agnostic status/settings/camera/
    |                       #   command logic shared by web_server + serial_config
    +-- web_server.h/.cpp   # SoftAP, captive DNS, REST API, OTA endpoint
    +-- serial_config.h/.cpp # Web Serial bench-config protocol (USB)
    +-- web_assets.h        # Embedded Glassmorphism Web UI (PROGMEM)
```

### REST API (used by the Web UI)

| Endpoint | Purpose |
|---|---|
| `GET /api/status` | Live telemetry snapshot (JSON) |
| `POST /api/settings` | Update + persist settings |
| `POST /api/camera` | `{"scan":true}` \| `{"pair":"MAC","type":0\|1}` \| `{"select":i}` \| `{"remove":i}` |
| `POST /api/command` | `{"cmd":"start"\|"stop"\|"reboot"}` |
| `POST /api/msp` | Read-only allowlisted MSP passthrough |
| `GET /api/scan` | Current scan results |
| `POST /api/ota` / `GET /api/ota/status` | OTA firmware update |

See "Web Serial Bench Console" above for the USB equivalent of this surface.

## Known Limitations (ESP32-C3)

- Wi-Fi and BLE share one radio; heavy Wi-Fi traffic can slightly delay BLE.
  Normal dashboard usage is no problem.
- GoPro telemetry depth depends on model firmware (battery %, encoding state;
  record timer is counted locally while encoding).
- DJI telemetry parsing beyond link state is still experimental upstream.
- Native-USB C3 boards ("Super Mini" and similar) require the
  `ARDUINO_USB_MODE` / `ARDUINO_USB_CDC_ON_BOOT` build flags (already set in
  this repo) — see the Hardware Requirements note above.

## Roadmap

- [x] GoPro profile (Open GoPro BLE)
- [x] Parallel info on all four custom messages
- [x] Glassmorphism Web UI with dark/light mode
- [x] Record-on-arm (+ stop-on-disarm)
- [x] Camera registry with discovery scan + Pair & Save
- [x] OTA firmware update from the Web UI
- [x] Wi-Fi power switch on a spare AUX channel
- [x] Web Serial bench console (GitHub Pages, USB transport)
- [x] OSD live preview in the bench console (renders Custom Messages through the real Betaflight OSD font)
- [ ] Lightweight MSP config panel ("configurator-lite" subtab)
- [ ] Profiles , Camera configuration.
- [x] Full DJI telemetry parse (battery %, rec time from DUML notifications)
- [x] OTA firmware flashing over Web Serial, including through a live Betaflight passthrough session (app-level chunked upload into the spare OTA partition — see "Web Serial Bench Console" below)

## Credits & References

- [yigitkonur/lib-osmo-ble](https://github.com/yigitkonur/lib-osmo-ble) - DUML-over-BLE wire format & pairing flow
- [KonradIT/osmosis](https://github.com/KonradIT/osmosis) - hardware-verified Osmo protocol map
- [gopro/OpenGoPro](https://github.com/gopro/OpenGoPro) - official Open GoPro BLE specification
- [rhoenschrat/DJI-Remote](https://github.com/rhoenschrat/DJI-Remote) - multicam BLE remote reference
- [Easy4Racing/bf_custom_osd_msg_example](https://github.com/Easy4Racing/bf_custom_osd_msg_example) - BF custom message reference
- [betaflight/betaflight](https://github.com/betaflight/betaflight) - MSP protocol source of truth
- [itsfpv CamLink](https://itsfpv.de/en-int/products/camlink) - the commercial product this project replicates
- [betaflight/betaflight-configurator](https://github.com/betaflight/betaflight-configurator) - the stock OSD font (`resources/osd/2/betaflight.mcm`, **GPL-3.0**) decoded into `docs/assets/osd-font.png` for the Bench Console's OSD live preview. That one asset is GPL-3.0, distinct from the rest of this MIT-licensed repo -- see the License section.

## License

MIT - do what you want, fly safe, and land your protocols responsibly.

**Exception:** `docs/assets/osd-font.png` is decoded from betaflight-configurator's
stock OSD font (`resources/osd/2/betaflight.mcm`) and remains **GPL-3.0**,
per the upstream project's license -- see Credits & References above. It's
a static image asset used only to render the Bench Console's OSD live
preview; it isn't linked into the firmware or required to build/run
anything else in this repo.