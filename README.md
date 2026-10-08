# SpiderBridge

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
- Bridge free memory and firmware version (bridge-level diagnostics)

**Home Assistant integration**

- **Automatic MQTT Discovery** — 154 entities per setup appear by themselves, grouped
  correctly into one device per controller plus one bridge device
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

## Installation

Three steps, no app and no cloud involved.

### 1. Flash the firmware (Wi-Fi credentials included)

1. Open the [web installer](https://matthias1232.github.io/spider-farmer-homeassistant/installer/).
2. Connect the ESP32 by USB and click **Connect** — the installer flashes the firmware.
3. When prompted, enter your **home Wi-Fi SSID and password** — the Improv Serial
   provisioner sends them to the ESP32 right after flashing. (Alternatively you can join
   the `SpiderBridge` hotspot with password `12345678` and configure Wi-Fi at
   `http://192.168.10.1`.)

The build the installer uses is always the latest successful build from this repository —
you can also build and flash any commit yourself (see *Build from source*).

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

Every controller gets its own device with the entities below (152 per controller), plus
2 bridge-level entities (bridge memory and firmware). The list is generated from the
firmware's discovery table (`firmware/ggs/main/ha_discovery_table.c`).

<!-- ENTITY-LIST:START -->
Grouped by function. *Kind*: Control = writable entity, Sensor = read-only.

| Entity | Platform | Kind | Unit |
|---|---|---|---|
| $N | fan | Control | — |
| $N | select | Control (config) | — |
| $N | select | Control | — |
| $N | time | Control (config) | — |
| $N | time | Control (config) | — |
| $N | select | Control (config) | — |
| $N | select | Control (config) | — |
| $N | time | Control (config) | — |
| $N | time | Control (config) | — |
| $N | time | Control (config) | — |
| $N | select | Control (config) | — |
| $N | fan | Control | — |
| $N | switch | Control | — |
| $N | fan | Control | — |
| $N | select | Control (config) | — |
| $N | select | Control | — |
| $N | time | Control (config) | — |
| $N | time | Control (config) | — |
| $N | select | Control (config) | — |
| $N | select | Control (config) | — |
| $N | time | Control (config) | — |
| $N | time | Control (config) | — |
| $N | time | Control (config) | — |
| $N | select | Control (config) | — |
| $N | switch | Control | — |
| $N | light | Control | — |
| $N | time | Control (config) | — |
| $N | time | Control (config) | — |
| $N | number | Control (config) | % |
| $N | select | Control (config) | — |
| $N | time | Control (config) | — |
| $N | time | Control (config) | — |
| $N | number | Control (config) | µmol/m²/s |
| $N | select | Control (config) | — |
| $N | number | Control (config) | % |
| $N | number | Control (config) | % |
| $N | select | Control (config) | — |
| $N | select | Control (config) | — |
| $N | light | Control | — |
| $N | time | Control (config) | — |
| $N | time | Control (config) | — |
| $N | number | Control (config) | % |
| $N | select | Control (config) | — |
| $N | time | Control (config) | — |
| $N | time | Control (config) | — |
| $N | number | Control (config) | µmol/m²/s |
| $N | select | Control (config) | — |
| $N | number | Control (config) | % |
| $N | number | Control (config) | % |
| $N | select | Control (config) | — |
| $N | select | Control (config) | — |
| $N | time | Control (config) | — |
| $N | time | Control (config) | — |
| $N | number | Control (config) | °C |
| $N | number | Control (config) | °C |
| $N | number | Control (config) | °C |
| $N | number | Control (config) | % |
| $N | number | Control (config) | % |
| $N | number | Control (config) | % |
| $N | number | Control (config) | ppm |
| $N | number | Control (config) | ppm |
| $N | number | Control (config) | ppm |
| $N | switch | Control | — |
| $N | switch | Control | — |
| $N | switch | Control | — |
| $N | switch | Control | — |
| $N | switch | Control | — |
| $N | switch | Control | — |
| $N | switch | Control | — |
| $N | switch | Control | — |
| $N | switch | Control | — |
| $N | switch | Control | — |
| $N | switch | Control | — |
| $N | switch | Control | — |
| $N | switch | Control | — |
| $N | number | Control (config) | °C |
| $N | number | Control (config) | % |
| $N | number | Control (config) | ppm |
| $N | number | Control (config) | — |
| $N | switch | Control | — |
| $N | sensor | Sensor | — |
| $N | sensor | Sensor | — |
| $N | sensor | Sensor | — |
| $N | switch | Control (config) | — |
| $N | number | Control (config) | °C |
| $N | number | Control (config) | °C |
| $N | switch | Control (config) | — |
| $N | number | Control (config) | % |
| $N | number | Control (config) | % |
| $N | switch | Control (config) | — |
| $N | number | Control (config) | kPa |
| $N | number | Control (config) | kPa |
| $N | switch | Control (config) | — |
| $N | number | Control (config) | ppm |
| $N | number | Control (config) | ppm |
| $N | switch | Control (config) | — |
| $N | number | Control (config) | µmol/m²/s |
| $N | switch | Control (config) | — |
| $N | number | Control (config) | °C |
| $N | number | Control (config) | °C |
| $N | switch | Control (config) | — |
| $N | number | Control (config) | % |
| $N | number | Control (config) | % |
| $N | switch | Control (config) | — |
| $N | number | Control (config) | mS/cm |
| $N | number | Control (config) | mS/cm |
| $N | switch | Control (config) | — |
| $N | switch | Control (config) | — |
| $N | switch | Control (config) | — |
| $N | switch | Control (config) | — |
| $N | switch | Control (config) | — |
| $N | switch | Control (config) | — |
| $N | sensor | Sensor (diagnostic) | — |
| $N | sensor | Sensor (diagnostic) | — |
| $N | sensor | Sensor (diagnostic) | — |
| $N | sensor | Sensor (diagnostic) | — |
| $N | sensor | Sensor (diagnostic) | — |
| $N | button | Control (config) | — |
| $N | switch | Control | — |
| $N | sensor | Sensor (diagnostic) | — |
| $N | sensor | Sensor (diagnostic) | — |
| $N | sensor | Sensor (diagnostic) | — |
| $N | sensor | Sensor (diagnostic) | — |
| $N | sensor | Sensor (diagnostic) | — |
| $N | sensor | Sensor (diagnostic) | d |
| $N | binary_sensor | Sensor (diagnostic) | — |
| $N | sensor | Sensor (diagnostic) | — |
| $N | button | Control (config) | — |
| $N | button | Control (config) | — |
| $N | text | Control (config) | — |
| $N | button | Control (config) | — |
| $N | switch | Control (config) | — |
| $N | switch | Control (config) | — |
| $N | sensor | Sensor | °C |
| $N | sensor | Sensor | % |
| $N | sensor | Sensor | ppm |
| $N | sensor | Sensor | kPa |
| $N | sensor | Sensor | µmol/m²/s |
| $N | sensor | Sensor | °C |
| $N | sensor | Sensor | % |
| $N | sensor | Sensor | mS/cm |
| $N | sensor | Sensor (diagnostic) | — |
| $N | sensor | Sensor (diagnostic) | — |
| $N | sensor | Sensor (diagnostic) | — |
| $N | sensor | Sensor (diagnostic) | — |
| $N | sensor | Sensor (diagnostic) | — |
| $N | sensor | Sensor (diagnostic) | — |
| $N | sensor | Sensor (diagnostic) | s |
| $N | sensor | Sensor (diagnostic) | — |
| $N | sensor | Sensor (diagnostic) | — |
| $N | sensor | Sensor (diagnostic) | dBm |
| $N | sensor | Sensor (diagnostic) | — |
| $B | sensor | Sensor (diagnostic) | — |
| $B | sensor | Sensor (diagnostic) | — |

**154 entities in total** — 152 per controller + 2 on the bridge itself (binary\_sensor × 1, button × 4, fan × 3, light × 2, number × 36, select × 18, sensor × 36, switch × 33, text × 1, time × 20). Generated from the firmware discovery table by `scripts/gen_entity_docs.py`; do not edit by hand.
<!-- ENTITY-LIST:END -->

## How it works

```
┌────────────────────┐   Bluetooth LE    ┌───────────────────────────────┐
│  GGS Controller    │◄─────────────────►│  ESP32 "SpiderBridge"         │
│  (sensors, lights, │  encrypted GGS    │  • ggs_ble.c     BLE client   │
│   fans, outlets…)  │   protocol        │  • ha_mqtt.c     MQTT + HA    │
└────────────────────┘                   │  • web interface (Settings,   │
                                         │    Control, Status, Network,  │
        ┌────────────────────────┐       │    Log, Firmware)            │
        │  Home Assistant        │       │  • MQTT Discovery, 154        │
        │  (auto-discovered      │◄──────│    entities                  │
        │   entities)            │ MQTT  └───────────────────────────────┘
        └────────────────────────┘
```

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
ESP32/ESP32-C6 firmwares can be added later. `firmware.json` describes what exists:

```json
[
  { "id": "ggs", "dir": "firmware/ggs", "targets": ["esp32"] }
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

SpiderBridge is a **learned project**: I am not a professional developer and **not a
single line of this code was written by hand**. Everything — firmware, web installer,
demo, this repository — was built through prompt engineering with AI coding agents
([opencode-go](https://github.com/sst/opencode) / Claude / Gemini) in the Antigravity
IDE and refined down to the last detail. The learnings, experiments and every fix came
from extensive troubleshooting and manual log analysis — finding every error, mapping
every function to the smallest detail of the firmware.

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
