# SpiderBridge

<p align="center"><img src="docs/img/logo.png" alt="SpiderBridge — a spider web crossed by an ESP32 chip" width="460"></p>

**Bridge your Spider Farmer GGS grow controller to Home Assistant — with an ESP32 instead of a Raspberry Pi.**

[![License: GPL-3.0-or-later](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Platform: ESP32](https://img.shields.io/badge/Platform-ESP32--WROOM--32-blue)](firmware/ggs)
[![Build firmware](https://github.com/matthias1232/spider-farmer-homeassistant/actions/workflows/build.yml/badge.svg)](https://github.com/matthias1232/spider-farmer-homeassistant/actions/workflows/build.yml)

> **Everything the Spider Farmer app can do is reproduced 1:1 in Home Assistant — and the
> installation, setup and everyday operation of your grow controller work completely
> **without the app and without any cloud**. No Spider Farmer account, no vendor server,
> no phone in the loop: flash once, pair once over Bluetooth, and every value and every
> setting of your controller appears as native Home Assistant entities.

**[Live demo of the web interface](https://matthias1232.github.io/spider-farmer-homeassistant/demo/)** ·
**[Web installer](https://matthias1232.github.io/spider-farmer-homeassistant/installer/)** ·
[Donate (PayPal)](https://www.paypal.com/paypalme/matthias1232) ·
[Supported ESP32 board (Amazon)](https://www.amazon.de/dp/B0DHRV7784?&linkCode=ll2&tag=matthias1232-20&linkId=c71aee711cb280677528abe8e058e53c&ref_=as_li_ss_tl)

<p align="center">
  <a href="docs/img/ha/controller-page.png"><img src="docs/img/ha/controller-page.png" alt="The GGS controller's complete device page in Home Assistant — all 153 entities in one capture" width="100%"></a>
</p>

<p align="center">
  <a href="docs/img/ha/bridge-page.png"><img src="docs/img/ha/bridge-page.png" alt="The SpiderBridge bridge's complete device page in Home Assistant — all 30 entities in one capture" width="100%"></a>
</p>

*Screenshots are from Home Assistant. Device and area names are demo names; the readings and timestamps are the values the bridge reported at capture time. Both device pages are captured whole — one image each, top to bottom — see the screenshot section.*

---

## What is SpiderBridge?

SpiderBridge is an ESP32 firmware that talks to a **Spider Farmer GGS controller**
(CB / PS5 / PS10 / LC) over **Bluetooth Low Energy** and mirrors its complete state and
settings into **Home Assistant via MQTT Discovery**. The controller keeps working exactly
as before — the bridge only *reads* what the controller reports and *writes* the settings
you change in Home Assistant.

No app. No cloud. No extra Raspberry Pi, no extra cables, no Docker. A ~€10 ESP32 board
with 4 MB flash is enough.

## Hardware

The firmware was developed and tested with this board:

<p align="center"><img src="docs/img/esp32-board.png" alt="ESP32-WROOM-32 board illustration" width="420"></p>

**→ [QIQIAZI ESP32 NodeMCU Development Board (2-pack, ESP32-WROOM-32, 4 MB, USB-C) — Amazon](https://www.amazon.de/dp/B0DHRV7784?&linkCode=ll2&tag=matthias1232-20&linkId=c71aee711cb280677528abe8e058e53c&ref_=as_li_ss_tl)**

The firmware was developed and tested with exactly this board (ESP32-WROOM-32, 4 MB flash).
Any other ESP32 board with 4 MB flash works as well. If you order through this link I'd be
happy — it supports the project at no extra cost to you.

Additionally required:

- A **Spider Farmer GGS controller** with Bluetooth (all current models)
- A **Home Assistant** instance with the MQTT integration (any broker, e.g. Mosquitto)
- The controller does **not** need Wi-Fi credentials or the Spider Farmer app — it is
  provisioned entirely over Bluetooth through the bridge

## Features

**Installation & setup**

- **Browser installer** — flash the firmware directly from [the installer page](https://matthias1232.github.io/spider-farmer-homeassistant/installer/) (ESP Web Tools), **including your Wi-Fi credentials** via Improv Serial over Web Serial — the ESP32 joins your network while you install, no separate configuration step
- **Bluetooth provisioning** — the bridge scans for GGS controllers, sends them your
  hotspot credentials, activates the device and pairs it, all from the web interface or
  with a single Home Assistant button press
- **Own hotspot** during setup: `SpiderBridge` / password `12345678` / `192.168.10.1`
  (the hotspot password can be changed or generated in the web interface)
- Web interface with **settings, control, status, network, MQTT log and firmware update**

**Full controller coverage (everything the app can do, 1:1)**

- **Both lights**: on/off, brightness, Manual / Schedule / PPFD mode, window, target
  brightness, fade time, PPFD target with min/max brightness, dim and off-threshold
- **Both fans**: circulation fan (1–10) and exhaust fan (25–100 %), on/off, speed,
  Manual / Schedule / Cycle / Environment modes (temperature- or humidity-prioritised,
  temperature-only, humidity-only, both), schedule window and speed, cycle window,
  run/off time and repeats, standby speed, oscillation, natural wind, CO₂-close while the
  blower runs
- **Climate accessories**: heater, humidifier, dehumidifier — on/off
- **Outlets 1–10** of the power strips — on/off
- **Day/night cycle**: start and end time, temperature, humidity and CO₂ targets for day
  and night, plus deadbands
- **Sensor calibration** offsets for temperature, humidity, CO₂ and PPFD
- **Sensor cleaning** (including phase status, cooling-down and remaining time)
- **Alarms** for all 13 alarm types (air and substrate temperature/humidity, VPD, CO₂,
  PPFD, sensor offline, light over-temperature, dehumidifier tank full, humidifier water
  low, water leak, water shortage) with min/max ranges
- **Grow plan**: multi-stage plan from templates, stage labels with start/end dates,
  colors, reminders, per-stage marker — mirrored as entities, visible per controller
- **Time**: timezone, automatic daylight-saving (by zone rules or forced on/off),
  one-click time sync to the controller
- **Controller internet access**: switch the controller's own cloud connection on/off
  (the app keeps working through the bridge regardless)
- **Bluetooth pairing controls**: pair / unpair buttons per controller

**Live values (sensors)**

- Air temperature, humidity, CO₂, VPD, PPFD
- Substrate temperature, humidity, EC (averages)
- Controller clock, firmware/hardware version, build date, uptime, restarts, free memory,
  Wi-Fi signal strength, timezone rules
- Bridge diagnostics: uplink network, address, signal and quality, uplink drops, hotspot clients, free memory, uptime and firmware version

**Home Assistant integration**

- **Automatic MQTT Discovery** — 183 entities (153 per controller + 30 on the bridge) appear by
  themselves, grouped correctly into one device per controller plus one bridge device
- Full **availability** handling (bridge + per controller)
- Every setting is a native HA entity (switch/select/number/time/light/fan/…), so
  automations, dashboards, voice assistants and the Energy dashboard all work

**Operation & maintenance**

- MQTT live log (up/down/HA traffic) and full syslog in the web interface
- Configuration backup and restore via the web interface
- Firmware update over the web interface, plus remote update check against a manifest URL
- Syslog forwarding to an external server
- MAC/IP filter for hotspot clients and web access rules

## Web interface (try it live)

The web interface runs entirely on the bridge — six pages, all of them in the
**[live demo](https://matthias1232.github.io/spider-farmer-homeassistant/demo/)**:

| Page | What it does |
| --- | --- |
| **Settings** | Home Wi-Fi, static IP, hotspot, Bluetooth provisioning, MQTT broker, syslog/TLS, what to publish, NAT, DNS/NTP, clock, backup/restore, factory reset |
| **Control** | Every controller: lights, fans, climate accessories, outlets, day/night targets, calibration, alarms, plans and templates — the complete app feature set |
| **Status** | Wi-Fi, hotspot, WAN, MQTT, syslog, devices, clock, firmware, memory, stack, BLE |
| **Network** | Hotspot clients, MAC and IP filter |
| **MQTT log** | Live up/down/discovery traffic |
| **Firmware** | Running version, URL update check, `.bin` upload, restart |

The demo is the **real interface, 1:1**, rendered offline from this repository
(`scripts/extract_gui.py`) with all data replaced by random demo values — no
personal data, and nothing you click is stored anywhere.

<p align="center">
  <a href="docs/img/web/settings.png"><img src="docs/img/web/settings.png" alt="Settings page: Wi-Fi, hotspot, Bluetooth, MQTT" width="32%"></a>
  <a href="docs/img/web/control.png"><img src="docs/img/web/control.png" alt="Control page: lights, fans, climate, outlets" width="32%"></a>
  <a href="docs/img/web/status.png"><img src="docs/img/web/status.png" alt="Status page: uplink, hotspot, memory, system log" width="32%"></a>
</p>

## Installation

Three steps, no app and no cloud involved.

> **Already using the Spider Farmer app?** You can keep it. The only thing that changes is
> that the controller's Wi-Fi is pointed at the bridge's hotspot once — after that the app
> still reaches the controller through the bridge (the controller's own cloud link can even
> be switched off entirely in the web interface), while Home Assistant sees everything at
> the same time.

### 1. Flash the firmware (Wi-Fi credentials included)

1. Open the [web installer](https://matthias1232.github.io/spider-farmer-homeassistant/installer/)
   in **Chrome or Edge** on a desktop computer and plug the board in with a USB *data* cable.
2. **New board: click "⚡ Quick Connect".** Enter your home Wi-Fi and press *Select board & start*;
   Chrome opens its serial port list, you choose the board, and the wizard then
   - installs the newest firmware (erasing the board first, if you leave that ticked),
   - joins your home Wi-Fi,
   - gives the bridge's own hotspot a **new random password** (shown once at the end: write it down),
   - and, if the Bluetooth box is ticked, makes the bridge **search for your Spider Farmer GGS
     controller on its first start and connect it to the hotspot**. The controller stays visible over
     Bluetooth, so you can still pair your phone and use the Spider Farmer app afterwards. Only
     controllers heard clearly (stronger than −75 dBm) are touched, and the switch is used up after
     that one start.
3. **Already flashed, or want single steps:** click *Open installer & device tools*. The window it opens
   offers *Install*, *IP addresses & status* (home network, hotspot, gateway, signal),
   *Send Wi-Fi to device*, *Connect Wi-Fi & randomize SpiderBridge Wi-Fi password*,
   *Randomize SpiderBridge Wi-Fi password* and *Logs & Console*. (You can also join the
   `SpiderBridge` hotspot and configure Wi-Fi at `http://192.168.10.1`.)
4. Leave **Module** on *Auto-detect* (or pick your module, like the board list in Tasmota's installer).

> **Install fails with "Failed to initialize"?** Many ESP32 boards cannot enter download mode on
> their own. Hold the **BOOT** button while you press *Install* (or *Select board & start*) and
> release it when the progress bar starts.

The wizard's extra commands (IP addresses, hotspot password, quick connect) are SpiderBridge extensions of
[Improv Wi-Fi Serial](https://www.improv-wifi.com/serial/) (commands `0x40`–`0x43`, see
`firmware/ggs/main/improv_serial.c`); other Improv clients ignore them. The firmware side is covered by a
host test (`python firmware/ggs/host_test/run.py`) and the installer by `node tests/installer_quickconnect.test.mjs`;
both run in CI before the firmware is built.
The build the installer uses is always the latest successful build from this repository —
you can also build and flash any commit yourself (see *Build from source*).

*Auto-detect* reads the chip family from the connected board and installs the matching build.
The **Module** list only offers modules that were really built; today that is the classic
**ESP32** (WROOM-32 / WROVER / DevKitC / NodeMCU, 4 MB flash or more). ESP32-S2/S3/C3/C6 boards
are not supported yet, so the installer says so instead of flashing a wrong image.

### 2. Connect the bridge to Home Assistant

1. In Home Assistant: **Settings → Devices & services → MQTT** (install the Mosquitto
   add-on or point HA at your broker).
2. Enter your broker in the bridge's web interface (Settings → MQTT) — or let the bridge
   run its own broker on the hotspot.
3. The bridge announces itself over MQTT Discovery; the diagnostics entities
   (bridge memory/firmware) appear immediately.

### 3. Pair the GGS controller (Bluetooth)

1. In the bridge's web interface open **Control → Bluetooth: scan**
   (or press the *Pair Bluetooth* button in Home Assistant).
2. The bridge briefly restarts into Bluetooth-only mode, scans, and lists the controllers
   it found — with signal strength, product code and whether they are already known.
3. Select your controller. The bridge hands over the hotspot credentials, activates the
   device on the Bluetooth link and reboots straight back into normal operation.
4. A few seconds later the controller shows up in Home Assistant as its own device with
   the full set of entities (the table below).

That's it — no Spider Farmer app was opened at any point.

## Home Assistant entities

Every controller gets its own device with 153 entities, and the bridge has its own device with
30 more (clock and time zone, network switches, uplink and hotspot diagnostics, restart,
factory reset). The list below is generated from the firmware
(`firmware/ggs/main/ha_discovery_table.c` and `firmware/ggs/main/ha_mqtt.c`).

<details>
<summary><strong>All 183 entities</strong> — 153 per controller, 30 on the bridge (kind: Control = writable, Sensor = read-only)</summary>

<!-- ENTITY-LIST:START -->
Grouped by function. *Kind*: Control = writable entity, Sensor = read-only.

| Entity | Platform | Kind | Unit |
|---|---|---|---|
| Fan | fan | Control | — |
| Fan Cycle Off Time | time | Control (config) | — |
| Fan Cycle Repeats | select | Control (config) | — |
| Fan Cycle Run Time | time | Control (config) | — |
| Fan Cycle Start Time | time | Control (config) | — |
| Fan Mode | select | Control | — |
| Fan Natural Wind | switch | Control | — |
| Fan Oscillation | fan | Control | — |
| Fan Schedule End Time | time | Control (config) | — |
| Fan Schedule Speed | select | Control (config) | — |
| Fan Schedule Start Time | time | Control (config) | — |
| Fan Speed | select | Control (config) | — |
| Fan Standby Speed | select | Control (config) | — |
| Close CO2 While Blower Runs | switch | Control | — |
| Fan Exhaust | fan | Control | — |
| Fan Exhaust Cycle Off Time | time | Control (config) | — |
| Fan Exhaust Cycle Repeats | select | Control (config) | — |
| Fan Exhaust Cycle Run Time | time | Control (config) | — |
| Fan Exhaust Cycle Start Time | time | Control (config) | — |
| Fan Exhaust Mode | select | Control | — |
| Fan Exhaust Schedule End Time | time | Control (config) | — |
| Fan Exhaust Schedule Speed | select | Control (config) | — |
| Fan Exhaust Schedule Start Time | time | Control (config) | — |
| Fan Exhaust Speed | select | Control (config) | — |
| Fan Exhaust Standby Speed | select | Control (config) | — |
| Light 1 | light | Control | — |
| Light 1 Dim Threshold | select | Control (config) | — |
| Light 1 Off Threshold | select | Control (config) | — |
| Light 1 PPFD End Time | time | Control (config) | — |
| Light 1 PPFD Fade Time | select | Control (config) | — |
| Light 1 PPFD Max Brightness | number | Control (config) | % |
| Light 1 PPFD Min Brightness | number | Control (config) | % |
| Light 1 PPFD Start Time | time | Control (config) | — |
| Light 1 PPFD Target | number | Control (config) | µmol/m²/s |
| Light 1 Schedule Brightness | number | Control (config) | % |
| Light 1 Schedule End Time | time | Control (config) | — |
| Light 1 Schedule Fade Time | select | Control (config) | — |
| Light 1 Schedule Start Time | time | Control (config) | — |
| Light 2 | light | Control | — |
| Light 2 Dim Threshold | select | Control (config) | — |
| Light 2 Off Threshold | select | Control (config) | — |
| Light 2 PPFD End Time | time | Control (config) | — |
| Light 2 PPFD Fade Time | select | Control (config) | — |
| Light 2 PPFD Max Brightness | number | Control (config) | % |
| Light 2 PPFD Min Brightness | number | Control (config) | % |
| Light 2 PPFD Start Time | time | Control (config) | — |
| Light 2 PPFD Target | number | Control (config) | µmol/m²/s |
| Light 2 Schedule Brightness | number | Control (config) | % |
| Light 2 Schedule End Time | time | Control (config) | — |
| Light 2 Schedule Fade Time | select | Control (config) | — |
| Light 2 Schedule Start Time | time | Control (config) | — |
| Day Cycle End | time | Control (config) | — |
| Day Cycle Start | time | Control (config) | — |
| Target CO2 Day | number | Control (config) | ppm |
| Target CO2 Deadband | number | Control (config) | ppm |
| Target CO2 Night | number | Control (config) | ppm |
| Target Humidity Day | number | Control (config) | % |
| Target Humidity Deadband | number | Control (config) | % |
| Target Humidity Night | number | Control (config) | % |
| Target Temperature Day | number | Control (config) | °C |
| Target Temperature Deadband | number | Control (config) | °C |
| Target Temperature Night | number | Control (config) | °C |
| Outlet 1 | switch | Control | — |
| Outlet 10 | switch | Control | — |
| Outlet 2 | switch | Control | — |
| Outlet 3 | switch | Control | — |
| Outlet 4 | switch | Control | — |
| Outlet 5 | switch | Control | — |
| Outlet 6 | switch | Control | — |
| Outlet 7 | switch | Control | — |
| Outlet 8 | switch | Control | — |
| Outlet 9 | switch | Control | — |
| Switch Dehumidifier | switch | Control | — |
| Switch Heater | switch | Control | — |
| Switch Humidifier | switch | Control | — |
| CO2 Offset | number | Control (config) | ppm |
| Humidity Offset | number | Control (config) | % |
| PPFD Offset | number | Control (config) | — |
| Temperature Offset | number | Control (config) | °C |
| Sensor Cleaning | switch | Control | — |
| Sensor Cleaning Phase Ends | sensor | Sensor | — |
| Sensor Cleaning Status | sensor | Sensor | — |
| Sensor Cleaning Time Left | sensor | Sensor | — |
| Alarm Air Humidity | switch | Control (config) | — |
| Alarm Air Humidity Maximum | number | Control (config) | % |
| Alarm Air Humidity Minimum | number | Control (config) | % |
| Alarm Air Temperature | switch | Control (config) | — |
| Alarm Air Temperature Maximum | number | Control (config) | °C |
| Alarm Air Temperature Minimum | number | Control (config) | °C |
| Alarm CO2 | switch | Control (config) | — |
| Alarm CO2 Maximum | number | Control (config) | ppm |
| Alarm CO2 Minimum | number | Control (config) | ppm |
| Alarm Dehumidifier Water Tank Full | switch | Control (config) | — |
| Alarm Humidifier Water Low | switch | Control (config) | — |
| Alarm Light Over-Temperature | switch | Control (config) | — |
| Alarm PPFD | switch | Control (config) | — |
| Alarm PPFD Maximum | number | Control (config) | µmol/m²/s |
| Alarm Sensor Offline | switch | Control (config) | — |
| Alarm Substrate EC | switch | Control (config) | — |
| Alarm Substrate EC Maximum | number | Control (config) | mS/cm |
| Alarm Substrate EC Minimum | number | Control (config) | mS/cm |
| Alarm Substrate Moisture | switch | Control (config) | — |
| Alarm Substrate Moisture Maximum | number | Control (config) | % |
| Alarm Substrate Moisture Minimum | number | Control (config) | % |
| Alarm Substrate Temperature | switch | Control (config) | — |
| Alarm Substrate Temperature Maximum | number | Control (config) | °C |
| Alarm Substrate Temperature Minimum | number | Control (config) | °C |
| Alarm VPD | switch | Control (config) | — |
| Alarm VPD Maximum | number | Control (config) | kPa |
| Alarm VPD Minimum | number | Control (config) | kPa |
| Alarm Water Leak | switch | Control (config) | — |
| Alarm Water Shortage | switch | Control (config) | — |
| Last Alarm Device (code) | sensor | Sensor (diagnostic) | — |
| Last Alarm Number | sensor | Sensor (diagnostic) | — |
| Last Alarm Time | sensor | Sensor (diagnostic) | — |
| Last Alarm Time (epoch) | sensor | Sensor (diagnostic) | — |
| Last Alarm Type (code) | sensor | Sensor (diagnostic) | — |
| Add Plan Stage From Template | button | Control (config) | — |
| Grow Plan | switch | Control | — |
| Plan Kept On Bridge | binary_sensor | Sensor (diagnostic) | — |
| Plan Stage | sensor | Sensor (diagnostic) | — |
| Plan Stage Color | sensor | Sensor (diagnostic) | — |
| Plan Stage Day | sensor | Sensor (diagnostic) | d |
| Plan Stage End | sensor | Sensor (diagnostic) | — |
| Plan Stage Reminder | sensor | Sensor (diagnostic) | — |
| Plan Stage Start | sensor | Sensor (diagnostic) | — |
| Plan Stages | sensor | Sensor (diagnostic) | — |
| Plan Template | select | Control (config) | — |
| Pair Bluetooth (stop advertising) | button | Control (config) | — |
| Unpair Bluetooth | button | Control (config) | — |
| Time Zone | text | Control (config) | — |
| Sync Device Time | button | Control (config) | — |
| Summer Time | switch | Control (config) | — |
| Controller Internet Access | switch | Control (config) | — |
| Air CO2 | sensor | Sensor | ppm |
| Air Humidity | sensor | Sensor | % |
| Air PPFD | sensor | Sensor | µmol/m²/s |
| Air Temperature | sensor | Sensor | °C |
| Air VPD | sensor | Sensor | kPa |
| Soil Average EC | sensor | Sensor | mS/cm |
| Soil Average Humidity | sensor | Sensor | % |
| Soil Average Temperature | sensor | Sensor | °C |
| Controller Firmware | sensor | Sensor (diagnostic) | — |
| Controller Free Memory | sensor | Sensor (diagnostic) | — |
| Controller Hardware | sensor | Sensor (diagnostic) | — |
| Controller Restarts | sensor | Sensor (diagnostic) | — |
| Controller Signal | sensor | Sensor (diagnostic) | dBm |
| Controller Time | sensor | Sensor (diagnostic) | — |
| Controller Uptime | sensor | Sensor (diagnostic) | — |
| Controller Uptime Seconds | sensor | Sensor (diagnostic) | s |
| Daylight Saving Rules | sensor | Sensor (diagnostic) | — |
| Firmware Built | sensor | Sensor (diagnostic) | — |
| Firmware Updated | sensor | Sensor (diagnostic) | — |
| Apply clock to all controllers | switch | Control (config) | — |
| Bridge time | sensor | Sensor | — |
| Clock synchronised | binary_sensor | Sensor | — |
| Daylight saving | select | Control | — |
| Follow the zone rules | switch | Control | — |
| NTP server | text | Control | — |
| Redirect time requests | switch | Control (config) | — |
| Summer time | switch | Control | — |
| Time sync while offline | switch | Control (config) | — |
| Time zone | select | Control | — |
| Time zone (type any) | text | Control (config) | — |
| Internet for controllers | switch | Control (config) | — |
| Mirror to the Spider Farmer cloud | switch | Control (config) | — |
| Name resolution while offline | switch | Control (config) | — |
| Redirect name lookups | switch | Control (config) | — |
| Hotspot clients | sensor | Sensor (diagnostic) | — |
| Uplink address | sensor | Sensor (diagnostic) | — |
| Uplink connected | binary_sensor | Sensor (diagnostic) | — |
| Uplink drops | sensor | Sensor (diagnostic) | — |
| Uplink network | sensor | Sensor (diagnostic) | — |
| Uplink quality | sensor | Sensor (diagnostic) | % |
| Uplink signal | sensor | Sensor (diagnostic) | dBm |
| Bridge Firmware | sensor | Sensor (diagnostic) | — |
| Bridge Free Memory | sensor | Sensor (diagnostic) | — |
| Firmware version | sensor | Sensor (diagnostic) | — |
| Free memory | sensor | Sensor (diagnostic) | kB |
| Uptime | sensor | Sensor (diagnostic) | s |
| Device name | text | Control (config) | — |
| Factory reset (type RESET) | text | Control (config) | — |
| Restart bridge | button | Control (config) | — |

**183 entities in total** — 153 per controller + 30 on the bridge itself (binary\_sensor × 3, button × 5, fan × 3, light × 2, number × 36, select × 21, sensor × 46, switch × 42, text × 5, time × 20). Generated from the firmware (`ha_discovery_table.c` and `ha_mqtt.c`) by `scripts/gen_entity_docs.py`; do not edit by hand.
<!-- ENTITY-LIST:END -->
</details>

<details>
<summary><strong>Home Assistant screenshots</strong> — the complete device pages, entity by entity</summary>

Each device page is captured in full — one image, top to bottom, nothing cut off. Counts vary with
firmware versions and Home Assistant configuration.

| | entities | complete page |
|---|---|---|
| **GGS controller** | 153 entities | [controller-page.png](docs/img/ha/controller-page.png) — 1915 × 9612 |
| **SpiderBridge bridge** | 30 entities | [bridge-page.png](docs/img/ha/bridge-page.png) — 1918 × 2456 |

Both complete pages are tall by definition, so they are the images worth looking at. Each is shown
below in full width, one under the other, and links to the unclipped original.

<details>
<summary><strong>About the counts</strong></summary>

The entity table above lists everything the firmware offers: 153 entities per controller and
30 on the bridge, 183 in total. They match what Home Assistant shows on the two device pages
below. The controller entities come from the discovery table in
`firmware/ggs/main/ha_discovery_table.c`; the bridge's own entities (clock, time zone, network
switches, uplink and hotspot diagnostics, restart, factory reset) are published by
`publish_bridge_discovery()` in `firmware/ggs/main/ha_mqtt.c`. The two sensors *Bridge Free
Memory* and *Bridge Firmware* come from the discovery table but belong to the bridge device.

</details>

**GGS controller — [controller-page.png](docs/img/ha/controller-page.png), all 153 entities in one image**

<p align="center">
  <a href="docs/img/ha/controller-page.png"><img src="docs/img/ha/controller-page.png" alt="The GGS controller's complete device page in Home Assistant, captured whole: fans, lights, outlets, sensors, alarms, calibration, plans and schedule entities" width="100%"></a>
</p>

**SpiderBridge — [bridge-page.png](docs/img/ha/bridge-page.png), all 30 entities in one image**

<p align="center">
  <a href="docs/img/ha/bridge-page.png"><img src="docs/img/ha/bridge-page.png" alt="The SpiderBridge bridge's complete device page in Home Assistant, captured whole: time sync, network, MQTT and diagnostics rows" width="100%"></a>
</p>

</details>

## How it works

![How SpiderBridge works — the controller talks to the ESP32 over Bluetooth, the bridge talks to Home Assistant over MQTT, and controllers on the hotspot can optionally be relayed to the Spider Farmer cloud](docs/img/architecture.png)

<details>
<summary>Text diagram</summary>

```
┌────────────────────┐   Bluetooth LE    ┌───────────────────────────────┐
│  GGS Controller    │◄─────────────────►│  ESP32 "SpiderBridge"         │
│  (sensors, lights, │  encrypted GGS    │  • ggs_ble.c     BLE client   │
│   fans, outlets…)  │   protocol        │  • ha_mqtt.c     MQTT + HA    │
└────────────────────┘                   │  • web interface (Settings,   │
                                         │    Control, Status, Network,  │
        ┌────────────────────────┐       │    Log, Firmware)            │
        │  Home Assistant        │       │  • MQTT Discovery, 183        │
        │  (auto-discovered      │◄──────│    entities                  │
        │   entities)            │ MQTT  └───────────────────────────────┘
        └────────────────────────┘
```

</details>

- **`ggs_ble.c`** — BLE client for the controller's GATT service (UUID `0x00FF`), the
  encrypted GGS protocol, product-code-specific AES keys, two-stage boot (radio + RAM
  for the scan/provisioning phase, BLE memory released to the system afterwards).
- **`sf_normalizer.c` / `sf_command_handler.c`** — translate the controller's frames to/from
  Home-Assistant-friendly values and commands.
- **`ha_mqtt.c` + `ha_discovery_table.c`** — publish state topics and the discovery payloads
  that make HA show the controller natively.
- **`mitm_proxy.c` + `wan_gate.c`** — optional path for controllers that connect to the
  bridge's Wi-Fi hotspot: the bridge terminates their TLS session locally and relays to
  the real Spider Farmer cloud, which can be switched off entirely (*Controller internet
  access = Off*). The certificates for this path ship with the repository — see
  [`firmware/ggs/certs/README.md`](firmware/ggs/certs/README.md) for their origin.

## Build from source

The repository is laid out for one directory per supported controller family, so more
firmwares can be added later. `firmware.json` describes what exists; every listed target
is built by CI and shows up in the installer's **Module** list:

```json
[
  { "id": "ggs", "dir": "firmware/ggs", "targets": ["esp32"],
    "modules": { "esp32": { "label": "ESP32 (WROOM-32 / WROVER / DevKitC / NodeMCU)" } } }
]
```

**GitHub Actions** (this repository) generates the flashable builds — after *every* code
change, immediately:

- push to `main` → build + publish (installer and demo pages always serve the newest build)
- Actions tab → **Run workflow** (`workflow_dispatch`) → build on demand, no push needed
- tag `v*` → build + the merged images are attached to the release

Merge binaries and per-target manifests are uploaded as workflow artifacts, so you can
always flash the current state without a local toolchain.

Locally, ESP-IDF v5.5 is enough:

```bash
cd firmware/ggs
idf.py set-target esp32
idf.py build
# or with a factory image:
./build.sh 1.0.0
```

The build produces `spiderbridge_esp32_v2.bin` for a 4 MB ESP32 (flashing offsets:
bootloader `0x1000`, partition table `0x8000`, OTA data `0xf000`, app `0x20000`).

## Certificates (origin)

The TLS material used by the optional WLAN relay path (`firmware/ggs/certs/`) is not fresh
for this project: it is the Spider Farmer certificate pair originally extracted for the
Python bridge this ESP32 port is based on, and it is publicly available there:
**[github.com/iceboerg00/spiderfarmer-bridge](https://github.com/iceboerg00/spiderfarmer-bridge)**
(`certs/`). Details: [`firmware/ggs/certs/README.md`](firmware/ggs/certs/README.md).

## Credits

Credits where credit is due:

- The original Python bridge this firmware is based on:
  [iceboerg00/spiderfarmer-bridge](https://github.com/iceboerg00/spiderfarmer-bridge)
  (including the TLS certificate material in `certs/`)
- The Spider Farmer app's protocol, reverse-engineered while building the bridge
- ESP-IDF, ESP Web Tools and the Improv Serial SDK — all open source

## Support the project

The firmware, the installer and the documentation are free. A donation is completely
voluntary — this is a private freetime project.

**[☕ Donate via PayPal](https://www.paypal.com/paypalme/matthias1232)** · the GitHub
Sponsor button in the sidebar points to the same place.

And if you still need hardware: **[the ESP32 board this firmware was developed and
tested with (Amazon)](https://www.amazon.de/dp/B0DHRV7784?&linkCode=ll2&tag=matthias1232-20&linkId=c71aee711cb280677528abe8e058e53c&ref_=as_li_ss_tl)**
— ordering through this affiliate link makes me happy and supports further development.

## License

GPL-3.0-or-later — see [LICENSE](LICENSE).
