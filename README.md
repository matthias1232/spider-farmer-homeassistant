<p align="center">
  <a href="#"><img src="docs/img/ggs-sketch.png" alt="SpiderBridge system overview: Complete grow tent setup with Spider Farmer GGS controller, SF-4000 light, inline fan, 5L humidifier with 2-way splitter and dehumidifier inside a closed local network shield. China cloud connection is optional and switchable in Home Assistant, with dual app options (HA native and Spider Farmer app), ESP32 MITM MQTT proxy, and browser web installer." width="100%"></a>
</p>

<p align="center">
  <strong>SpiderBridge — your Spider Farmer grow controller, native in Home&nbsp;Assistant.</strong><br>
  Flash once, pair once — every value and every setting of your GGS controller lives as a native Home Assistant entity. <b>No cloud. No app. No Raspberry Pi.</b>
</p>

<p align="center">
  <a href="https://matthias1232.github.io/spider-farmer-homeassistant/installer/"><img src="https://img.shields.io/badge/🚀_Web_Installer-Flash_in_60s-2ea44f?style=for-the-badge" alt="Web Installer"></a>
  &nbsp;
  <a href="https://matthias1232.github.io/spider-farmer-homeassistant/demo/"><img src="https://img.shields.io/badge/👁️_Live_Demo-Try_it-1f6feb?style=for-the-badge" alt="Live Demo"></a>
  &nbsp;
  <a href="https://github.com/matthias1232/spider-farmer-homeassistant/stargazers"><img src="https://img.shields.io/github/stars/matthias1232/spider-farmer-homeassistant?style=for-the-badge" alt="Stars"></a>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/100%25_local-no_cloud-2ea44f?style=flat-square" alt="No cloud">
  <img src="https://img.shields.io/badge/Hardware-from_~€10-blue?style=flat-square" alt="~10€ hardware">
  <img src="https://img.shields.io/badge/Setup-3_steps-blue?style=flat-square" alt="3 steps">
  <img src="https://img.shields.io/badge/License-GPLv3-blue?style=flat-square" alt="GPLv3">
  <img src="https://img.shields.io/badge/Platform-ESP32_WROOM--32-blue?style=flat-square" alt="ESP32">
  <img src="https://img.shields.io/github/actions/workflow/status/matthias1232/spider-farmer-homeassistant/build.yml?style=flat-square" alt="Build">
</p>

<p align="center">
  <a href="#-deutsche-version">🇩🇪 Deutsche Version ansehen</a>
</p>

<p align="center">
  <a href="https://matthias1232.github.io/spider-farmer-homeassistant/demo/"><img src="docs/img/preview-web-gui.png" alt="SpiderBridge web GUI preview — controller dashboard with day/night targets, fans, outlets and sensor readings" width="49%"></a>
  <a href="https://matthias1232.github.io/spider-farmer-homeassistant/installer/"><img src="docs/img/preview-web-installer.png" alt="Open the SpiderBridge web installer — flash firmware and set up the ESP32 in the browser" width="49%"></a>
</p>

---

## How it works

![Architecture — the controller joins the bridge's own Wi-Fi hotspot, the bridge talks to Home Assistant over MQTT](docs/img/architecture.png)

SpiderBridge is one ESP32 that sits between your Spider Farmer GGS controller and your Home
Assistant. Four links are involved and they do completely different jobs — and the order matters,
because each one sets up the next.

### 1. USB — your browser → the ESP32 (setup)

Everything starts with a **USB data cable** and Google Chrome or Microsoft Edge. The installer in
your browser talks to the board over the **Web Serial API** (`navigator.serial`) — the same cable you
plugged in. Nothing else is used to reach the board. Over that one cable the installer:

- **flashes the firmware** with [esptool-js](https://github.com/espressif/esptool-js), the library
  ESP Web Tools is built on,
- **sends your home Wi-Fi name and password** using the
  [Improv Wi-Fi Serial](https://www.improv-wifi.com/serial/) protocol, which the bridge answers on
  UART0 — its USB port — next to the normal log output,
- **gives the bridge's own hotspot a new random password**, and
- can **ask the bridge to go and find your GGS controller**.

**Bluetooth is not involved on this path at all.** Your browser never opens a Bluetooth connection
(`navigator.bluetooth` is never used, not by the installer, not by any page in this repository), and
your computer's Bluetooth is never paired with anything. The board is reached by cable, full stop.
That is deliberate: flashing, Wi-Fi setup and the console log all work on machines with no Bluetooth
at all.

Once setup is done this path is quiet — the bridge still answers on USB for logs, diagnostics and
firmware updates, but nothing about your grow flows through it.

### 2. Bluetooth — the ESP32's own radio → the controller (once)

The Bluetooth that matters here is the **ESP32's own Bluetooth adapter**, not your computer's. The
bridge is the BLE *central*: when you ask it to, it restarts into a short Bluetooth-only boot,
scans for controllers nearby, and connects to the one you pick.

Over that link it does exactly one job: **it hands the controller its new Wi-Fi.** The GGS exposes
a setup service (`0x00FF`) with a write characteristic (`0xFF02`) and a notify characteristic
(`0xFF01`). The bridge writes one AES-128-CBC encrypted JSON frame to it:

```json
{"method":"setWifi","params":{"ssid":"SpiderBridge-xxxx","pass":"<hotspot password>"},"msgId":"…","pid":"…","uid":"…"}
```

SSID in, password in — that is the entire Bluetooth story. **Bluetooth is not the data channel and
never carries a single grow value.** After that one-time handshake the bridge is done with
Bluetooth and the controller switches to Wi-Fi.

Two details worth knowing:

- **The controller keeps working with the Spider Farmer app.** Right after `setWifi` the controller
  stops advertising, so the bridge can send `setDevDeactive` and make it advertise again — your
  phone can then pair with it exactly as before.
- **Why the bridge reboots for this.** Bluetooth and the controller's TLS session compete for the
  same internal RAM on an ESP32. So Bluetooth is only switched on for the seconds it is needed, the
  result is kept, and the bridge boots back into normal mode where it hands all of that memory to
  the TLS session. In the installer you will see the board go quiet on USB while that happens.

### 3. Wi-Fi — controller → ESP32 hotspot → Home Assistant (all the data)

This is the real data path, and it has nothing to do with Bluetooth. The bridge runs **AP + STA at
the same time**: it is its own Wi-Fi access point (`SpiderBridge-…`) *and* a normal client on your
home network.

```text
   GGS controller ──Wi-Fi──▶ ESP32 hotspot ──DNS hijack──▶ TLS terminated locally
                                                            │
                                                     MQTT packets parsed
                                                            │
                                                    normalised + published
                                                            │
                                              your MQTT broker ──▶ Home Assistant
                                                            ▲
                                     commands go back down the same path
```

The controller was built to talk to `sf.mqtt.spider-farmer.com:8883`. The bridge's DNS proxy
answers that one name with its own address, so the controller opens its MQTT connection **to the
bridge**. From there:

1. The bridge **terminates the controller's TLS session locally**, using the Spider Farmer
   certificate material (`firmware/ggs/certs/`), so the controller believes it is talking to the
   vendor's broker.
2. Its **MQTT 3.1.1 packets are parsed** on the wire — `CONNECT`, `PUBLISH`, `SUBSCRIBE`,
   `PINGREQ` and the rest.
3. The JSON inside the publishes (`getDevSta`, `getConfigField`, …) is **normalised** into plain
   state values and published to *your* broker as retained topics:
   `spiderfarmer/<id>/state/temperature`, `spiderfarmer/<id>/state/fan`, `spiderfarmer/<id>/state/plan`, …
   Every one of them is announced through **MQTT Discovery**, so Home Assistant shows a native
   device with no YAML at all.
4. **Commands travel the same road in reverse.** Flip a switch in Home Assistant and the bridge
   receives `spiderfarmer/<id>/command/<field>/set`, rebuilds the **complete** module block the
   controller expects — it silently discards partial blocks — and injects the matching
   `setConfigField` frame into the very same session. The controller's next status frame confirms
   the change, and the entities update.

This path works even with **no MQTT broker configured**: the bridge then simply sends the commands
through the intercepted session instead, and the web interface keeps working.

One controller per bridge at a time: the firmware supports a single active controller session.

### 4. Cloud mirroring — optional, and switchable

If you want the Spider Farmer app or cloud to keep reaching the controller, the bridge can mirror
the session onward to the real broker. This is two independent switches, because they stop
different things:

| Switch | What it controls | Where you find it |
|---|---|---|
| **WAN gate** | NAT routing for hotspot clients. Off = the controller cannot reach anything past the bridge. | web UI, `switch.internet_for_controllers`, `spiderfarmer/<id>/command/wan/set` |
| **Cloud forward** | Whether the bridge relays the session to the real Spider Farmer cloud. Off = the bridge answers the controller itself. | web UI, Home Assistant |

Turning NAT off alone does **not** make the setup local-only — the bridge's own connection to the
cloud runs over your home network and bypasses NAT entirely. Both switches have to be off for a
genuinely offline setup. With cloud forwarding off the bridge answers the controller itself
(`CONNACK`, `SUBACK`, `PUBACK`, `PINGRESP`), so it stays connected, keeps publishing its status and
never settles into a reconnect loop. Home Assistant control keeps working either way — it never
used the cloud path.

### Summary

| # | Link | Transport | What it carries | Always on? |
|---|---|---|---|---|
| 1 | Browser → ESP32 | **USB** cable (Web Serial, esptool-js + Improv Serial) | Flash firmware, home Wi-Fi, hotspot password, logs | Setup and maintenance |
| 2 | ESP32 → controller | **Bluetooth LE** (the ESP32's own radio) | Hotspot SSID + password, once | One-time only |
| 3 | Controller → ESP32 → HA | **Wi-Fi**: hotspot + DNS hijack + local TLS + MQTT | Every value and every command | Always |
| 4 | ESP32 → Spider Farmer cloud | Wi-Fi uplink, mTLS relay | Optional mirroring | Switchable, off = fully local |

### Firmware modules

- **`improv_serial.c`** — Improv Wi-Fi Serial on the USB port, plus SpiderBridge's own extensions
  (commands `0x40`–`0x49`): addresses, hotspot password, Bluetooth jobs, restart, diagnostics.
- **`ggs_ble.c`** — BLE central for the GGS setup service (`0x00FF` / `0xFF02` / `0xFF01`), AES-128-CBC
  framing, `setWifi` / `setDevActive` / `setDevDeactive`.
- **`wifi_apsta.c`** — AP + STA at once: the controller's hotspot and your home network.
- **`dns_hijack.c` + `mitm_proxy.c`** — the answer that sends the controller's broker lookups to the
  bridge, the locally terminated TLS session, and the MQTT wire parser.
- **`mqtt_send.c` + `sf_command_handler.c`** — the way back: Home Assistant command → complete
  controller module block → injected frame.
- **`sf_normalizer.c`** — the controller's JSON → Home Assistant state topics, byte-compatible with
  the original Python bridge.
- **`ha_mqtt.c` + `ha_discovery_table.c`** — the connection to your broker and the MQTT Discovery
  payloads that make the entities native.
- **`mitm_proxy.c` + `wan_gate.c`** — the optional relay to the real Spider Farmer cloud, and the two
  switches that turn it off.

---

## Why SpiderBridge?

Spider Farmer forces a cloud account, a vendor app, and routes your grow data through their servers. **SpiderBridge turns that around**: your controller never speaks to a Spider Farmer server again — it talks to a ~€10 ESP32 on your own Wi-Fi, and Home Assistant sees everything as if the controller were a native HA device.

| 🔒 **100% local** | 💸 **Under €15 setup** | 🧠 **1:1 with the app** |
|---|---|---|
| No Spider Farmer cloud, no app, no account. The controller's own internet can be switched off entirely — two switches, both off, and nothing leaves your network. | One ESP32 (WROOM-32, 4 MB flash) is all you need. No Raspberry Pi, no Docker, no cable mess. | **183 HA entities** (153 per controller + 30 bridge diagnostics) — every light, fan, outlet, alarm and grow-plan setting is covered. |
| Set up over the **USB cable** with Chrome or Edge; Bluetooth is used exactly once, to give the controller its Wi-Fi. Home Assistant ↔ Bridge: MQTT on your LAN. | Flash via a USB data cable, plug it in, done. No soldering, no terminal, no toolchain. | Day/night targets, 13 alarm types, grow plans with templates, sensor calibration, one-time Bluetooth provisioning — complete. |

---

## ⚡ Install in your browser — no terminal, no soldering

<p align="center">
  <a href="https://matthias1232.github.io/spider-farmer-homeassistant/installer/"><img src="docs/img/web/settings.png" alt="SpiderBridge Web Installer — Wi-Fi, Hotspot, Bluetooth and MQTT in your browser" width="100%"></a>
</p>

<p align="center">
  <a href="https://matthias1232.github.io/spider-farmer-homeassistant/installer/"><img src="https://img.shields.io/badge/Open_installer_%26_device_tools-Flash_%C2%B7_Wi--Fi_%C2%B7_Quick_Connect-2ea44f?style=for-the-badge" alt="Open installer and device tools" height="44"></a>
  &nbsp;
  <a href="https://matthias1232.github.io/spider-farmer-homeassistant/demo/"><img src="https://img.shields.io/badge-Try_the_web_demo-1f6feb?style=for-the-badge" alt="Try the web demo" height="44"></a>
</p>

**Four steps. That's it.**

1. 🌐 **Open the installer** → [matthias1232.github.io/spider-farmer-homeassistant/installer/](https://matthias1232.github.io/spider-farmer-homeassistant/installer/) (Chrome or Edge, desktop)
2. 🔌 **Plug in the board** → with a USB data cable, then click *Select board & start*
3. 📶 **Enter your Wi-Fi** → esptool-js flashes the firmware over USB, and Improv Wi-Fi Serial sends your home network credentials over the same cable
4. 🔗 **(Optional) Quick Connect** → the bridge then does the Bluetooth work itself: it finds your GGS controller and hands it the hotspot's SSID and password

That's it. No command line, no `idf.py`, no YAML files. The ESP32 fetches the firmware straight from your browser over the USB cable, joins your Wi-Fi, and is reachable from then on at **`http://192.168.10.1`** on its own `SpiderBridge` hotspot, or at the address it received on your home network — the installer's *IP addresses & status* tool shows you both.

> 💡 **Tip:** If the flash fails with *Failed to initialize*, hold the **BOOT** button on the board while clicking *Install*, and release it as soon as the progress bar starts.

**Supported hardware:** Any ESP32 with **4 MB flash** (WROOM-32, WROVER, DevKitC, NodeMCU). Developed and tested with the
[QIQIAZI ESP32 NodeMCU Development Board (2-pack, USB-C)](https://www.amazon.de/dp/B0DHRV7784?&linkCode=ll2&tag=matthias1232-20&linkId=c71aee711cb280677528abe8e058e53c&ref_=as_li_ss_tl) — the affiliate link supports the project at no extra cost to you.

---

## What it does

- **Both lights** — on/off, brightness, Manual / Schedule / PPFD mode, fade time, dim & off threshold, PPFD target with min/max brightness
- **Both fans** — circulation and exhaust, all modes (Manual / Schedule / Cycle / Environment with temp/humidity priority), oscillation, natural wind, CO₂-closed-while-blower-runs
- **Climate accessories** — heater, humidifier, dehumidifier, individually switchable
- **10 outlets** of the power strips
- **Day/night cycle** — separate temperature, humidity and CO₂ targets plus deadband
- **13 alarm types** — air/substrate temperature & humidity, VPD, CO₂, PPFD, sensor offline, light over-temperature, dehumidifier tank, humidifier water, water leak, water shortage
- **Grow plans** — multi-stage plans from templates, stage labels with start/end, colors, reminders
- **Sensor calibration** — offsets for temperature, humidity, CO₂ and PPFD
- **Sensor cleaning** — with phase status, cool-down time and remaining time
- **Time & timezone** — automatic daylight saving per zone rules or manual, one-click sync to the controller
- **Bluetooth pairing** — pair/unpair button per controller
- **Live values** — air temperature/humidity, CO₂, VPD, PPFD, substrate temperature/humidity/EC, controller uptime, Wi-Fi signal, free memory
- **MQTT Discovery** — **183 native HA entities** with zero YAML configuration
- **Maintenance** — live log, syslog, backup/restore, OTA update, watchdogs + safe mode

<p align="center">
  <a href="docs/img/web/settings.png"><img src="docs/img/web/settings.png" alt="Settings: Wi-Fi, Hotspot, Bluetooth, MQTT" width="32%"></a>
  <a href="docs/img/web/control.png"><img src="docs/img/web/control.png" alt="Control: lights, fans, climate, outlets" width="32%"></a>
  <a href="docs/img/web/status.png"><img src="docs/img/web/status.png" alt="Status: uplink, hotspot, memory, system log" width="32%"></a>
</p>

---

## Quickstart in 3 steps

```
1. Flash    →  Open the installer, enter Wi-Fi, click "Install".
2. MQTT     →  Point the bridge at your broker (or use the Mosquitto add-on).
3. Pair     →  "Control → Bluetooth: scan" or the "Pair Bluetooth" button in HA.
```

The full step-by-step guide with troubleshooting lives below in the
[technical appendix](#technical-details-for-tinkerers).

---

## 🏠 Home Assistant — fully integrated

One bridge drives **one controller at a time**, and that controller shows up as its **own device with 153 entities** in Home Assistant, while the bridge adds **30 more diagnostic entities** (clock, timezone, network switches, uplink/hotspot diagnostics, restart, factory reset). All entities are native — use them in dashboards, automations, voice assistants, the energy dashboard and scenes without any YAML.

**GGS Controller — [controller-page.png](docs/img/ha/controller-page.png)** (all 153 entities in one capture):

<p align="center">
  <a href="docs/img/ha/controller-page.png"><img src="docs/img/ha/controller-page.png" alt="Complete device page of the GGS controller in Home Assistant, 153 entities" width="100%"></a>
</p>

**SpiderBridge — [bridge-page.png](docs/img/ha/bridge-page.png)** (all 30 entities in one capture):

<p align="center">
  <a href="docs/img/ha/bridge-page.png"><img src="docs/img/ha/bridge-page.png" alt="Complete device page of the bridge in Home Assistant, 30 entities" width="100%"></a>
</p>

---

## 🛡️ The bridge always comes back on its own

Pulling the power plug is **never** the way to restart it. Several independent layers each end in an automatic restart, and the USB console keeps answering throughout:

- **Hardware watchdogs** — task watchdog (8 s, both cores), interrupt watchdog (800 ms), bootloader watchdog
- **Supervisor** (`firmware/ggs/main/supervisor.c`) — core tasks report in regularly; hanging tasks or memory leaks (< 20 KB free heap for 90 s) trigger a restart
- **Boot-loop protection** — three restarts in a row without 2 minutes of healthy running start the **Safe Mode** (hotspot + Web UI + USB console only, no controller connect) — you can still reach, configure and flash it
- **Rollback on bad update** — the previous firmware image is restored if the new firmware does not boot
- **USB console never switches off** — after every flash, restart or crash it answers again

Tested on a real board with `python scripts/reboot_soak.py COM3 --mode fault` (crash, hang, watchdog, leak, safe mode) and `--mode command` (30× restart, ~11 s each, memory stable).

---

## ⭐ If this project helps you

- Give a **star** ⭐ — helps others find the repo.
- ☕ **[Buy me a coffee](https://www.buymeacoffee.com/matthias1232)** — voluntary, and the same link the firmware's *About* page shows.
- 📦 **[Supported ESP32 board (Amazon)](https://www.amazon.de/dp/B0DHRV7784?&linkCode=ll2&tag=matthias1232-20&linkId=c71aee711cb280677528abe8e058e53c&ref_=as_li_ss_tl)**
  — affiliate link, no extra cost to you.

---

## Technical details (for tinkerers)

<details>
<summary><strong>Full installation guide (3 steps)</strong></summary>

### 1. Flash the firmware (Wi-Fi credentials included)

1. Open the [Web Installer](https://matthias1232.github.io/spider-farmer-homeassistant/installer/)
   in **Chrome or Edge** on a desktop and plug the board in with a USB *data* cable.
2. *Quick Connect* has two buttons with the same flow; only the flash step differs:
   - **"New board: install + set up"** installs the newest firmware (erasing the board first if
     ticked) and then sets it up.
   - **"Board already flashed: set up only"** flashes nothing. It restarts the board, checks that
     SpiderBridge runs, and then sets it up. Use it right after a flash, or for any bridge that
     has not met a controller yet.

   Enter your home Wi-Fi and click *Select board & start*; Chrome opens its serial-port list, you
   pick the board, and the wizard
   - joins your home Wi-Fi,
   - gives the bridge's hotspot a **new random password** (shown once at the end: write it down),
   - and, if the Bluetooth box is ticked, makes the bridge **search for your Spider Farmer GGS
     controller and connect it to the hotspot**. The controller stays visible over Bluetooth, so
     you can still pair your phone and use the Spider Farmer app afterwards. Only controllers
     heard clearly (stronger than -75 dBm) are touched, and the switch is used up after that one
     start.

   A bridge that is **already in use** (home network set and a controller known) deliberately does
   not listen for USB commands, because that memory is needed for the controller's connection.
   *Set up only* then says so and offers *Erase, install firmware and continue*; otherwise change
   Wi-Fi and the hotspot password in the bridge's Web UI. A board with older firmware, or with
   other firmware, gets an *Install firmware and continue* button instead.
3. **Already flashed, or want single steps:** click *Open installer & device tools*. The window it
   opens offers *Install*, *IP addresses & status*, *Send Wi-Fi to GGS Controller* (searches for
   your Spider Farmer controller over Bluetooth and sends it the bridge's hotspot Wi-Fi),
   *Home Wi-Fi for the bridge*, *Connect Wi-Fi & randomize SpiderBridge Wi-Fi password*,
   *Randomize SpiderBridge Wi-Fi password* and *Logs & Console*. (You can also join the
   `SpiderBridge` hotspot and configure Wi-Fi at `http://192.168.10.1`.)
4. Leave **Module** on *Auto-detect* (or pick your module, like the board list in Tasmota's installer).

> 💡 **Install fails with "Failed to initialize"?** Many ESP32 boards cannot enter download mode on
> their own. Hold the **BOOT** button while you press *Install* (or *Select board & start*) and
> release it when the progress bar starts.

The wizard's extra commands (IP addresses, hotspot password, quick connect) are SpiderBridge
extensions of [Improv Wi-Fi Serial](https://www.improv-wifi.com/serial/) (commands `0x40`–`0x49`,
see `firmware/ggs/main/improv_serial.c`); other Improv clients ignore them. All of them travel over
the same USB serial link as the flashing and the Wi-Fi credentials. The firmware side is
covered by a host test (`python firmware/ggs/host_test/run.py`) and the installer by
`node tests/installer_quickconnect.test.mjs` and `node tests/installer_console.test.mjs`; both run
in CI before the firmware is built.

The build the installer uses is always the latest successful build from this repository — you
can also build and flash any commit yourself (see *Build from source*).

*Auto-detect* reads the chip family from the connected board and installs the matching build.
The **Module** list only offers modules that were really built; today that is the classic
**ESP32** (WROOM-32 / WROVER / DevKitC / NodeMCU, 4 MB flash or more). ESP32-S2/S3/C3/C6 boards
are not supported yet, so the installer says so instead of flashing a wrong image.

### 2. Connect the bridge to Home Assistant

1. In Home Assistant: **Settings → Devices & services → MQTT** (install the Mosquitto add-on or
   point HA at your broker).
2. Enter your broker in the bridge's Web UI (Settings → MQTT). A broker is what gives you the 183
   entities, history and automations; the bridge connects to it as a normal client, over `mqtt://`
   or `mqtts://` with a pinned CA if you want TLS. **The bridge does not run a broker itself.**
3. The bridge announces itself over MQTT Discovery; the diagnostics entities
   (bridge memory/firmware) appear immediately.
4. No broker at all? That works too: control then goes straight through the intercepted controller
   session, and the bridge's own web UI is the interface.

### 3. Give the GGS controller its Wi-Fi (Bluetooth, once)

1. In the bridge's Web UI open **Control → Bluetooth: scan** (or press the *Pair Bluetooth* button
   in Home Assistant). You can also do it from the installer with *Send Wi-Fi to GGS Controller* —
   the bridge does the Bluetooth work in both cases, over the same USB cable.
2. The bridge briefly restarts into Bluetooth-only mode, scans, and lists the controllers it found
   — with signal strength, product code and whether they are already known. It is silent on USB
   while that boot runs, which can take 10 to 120 seconds.
3. Select your controller. The bridge writes it the hotspot's SSID and password over the
   controller's setup characteristic, watches until the controller reports its Wi-Fi link is up,
   and reboots straight back into normal operation.
4. From that moment the controller is on the bridge's hotspot, its MQTT session goes to the bridge,
   and it appears in Home Assistant as its own device with the full set of entities.

**Done** — the Spider Farmer app was never opened.

> 💡 **Already using the Spider Farmer app?** You can keep it. Once after installation the
> controller's Wi-Fi is pointed at the bridge's hotspot — after that the app still reaches the
> controller through the bridge (and the controller's own cloud link can even be switched off
> entirely in the Web UI), while Home Assistant sees everything at the same time.

</details>

<details>
<summary><strong>Hardware recommendation</strong></summary>

The board the firmware was developed and tested with:

<p align="center"><img src="docs/img/esp32-board.png" alt="ESP32-WROOM-32 board" width="420"></p>

**→ [QIQIAZI ESP32 NodeMCU Development Board (2-pack, ESP32-WROOM-32, 4 MB, USB-C) — Amazon](https://www.amazon.de/dp/B0DHRV7784?&linkCode=ll2&tag=matthias1232-20&linkId=c71aee711cb280677528abe8e058e53c&ref_=as_li_ss_tl)**

Any other ESP32 board with 4 MB flash works as well. The affiliate link supports the project at
no extra cost to you.

Additionally required:

- **Spider Farmer GGS controller** with Bluetooth (all current models: CB / PS5 / PS10 / LC)
- **Home Assistant** with the MQTT integration (any broker, e.g. Mosquitto)
- The controller does **not** need Wi-Fi credentials or the Spider Farmer app — it is provisioned
  entirely over Bluetooth through the bridge

</details>

<details>
<summary><strong>All 183 HA entities</strong> — 153 per controller, 30 on the bridge (Control = writable, Sensor = read-only)</summary>

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
<summary><strong>Build from source</strong></summary>

The repository is laid out for one directory per supported controller family, so more firmwares
can be added later. `firmware.json` describes what exists; every listed target is built by CI and
shows up in the installer's **Module** list:

```json
[
  { "id": "ggs", "dir": "firmware/ggs", "targets": ["esp32"],
    "modules": { "esp32": { "label": "ESP32 (WROOM-32 / WROVER / DevKitC / NodeMCU)" } } }
]
```

**GitHub Actions** (this repository) generates the flashable builds — after *every* code change,
immediately:

- push to `main` → build + publish (installer and demo pages always serve the newest build)
- Actions tab → **Run workflow** (`workflow_dispatch`) → build on demand, no push needed
- tag `v*` → build + the merged images are attached to the release

Merge binaries and per-target manifests are uploaded as workflow artifacts, so you can always
flash the current state without a local toolchain.

Locally, ESP-IDF v5.5 is enough:

```bash
cd firmware/ggs
idf.py set-target esp32
idf.py build
# or with a factory image:
./build.sh 1.0.0
```

The build produces `spiderbridge_esp32_v2.bin` for a 4 MB ESP32 (flashing offsets: bootloader
`0x1000`, partition table `0x8000`, OTA data `0xf000`, app `0x20000`).

</details>

<details>
<summary><strong>Certificates (origin)</strong></summary>

The TLS material used by the optional WLAN relay path (`firmware/ggs/certs/`) is not fresh for
this project: it is the Spider Farmer certificate pair originally extracted for the Python bridge
this ESP32 port is based on, and it is publicly available there:
**[github.com/iceboerg00/spiderfarmer-bridge](https://github.com/iceboerg00/spiderfarmer-bridge)**
(`certs/`). Details: [`firmware/ggs/certs/README.md`](firmware/ggs/certs/README.md).

</details>

<details>
<summary><strong>Credits</strong></summary>

- The original Python bridge this firmware is based on:
  [iceboerg00/spiderfarmer-bridge](https://github.com/iceboerg00/spiderfarmer-bridge)
  (including the TLS certificate material in `certs/`)
- The Spider Farmer app's protocol, reverse-engineered while building the bridge
- ESP-IDF, ESP Web Tools and the Improv Serial SDK — all open source

</details>

---

## 🇩🇪 Deutsche Version

<details>
<summary><strong>SpiderBridge — dein Spider Farmer Grow Controller, direkt in Home Assistant. (Hier klicken zum Ausklappen)</strong></summary>

<p align="center">
  <strong>SpiderBridge — dein Spider Farmer Grow Controller, direkt in Home&nbsp;Assistant.</strong><br>
  Einmal flashen, einmal pairen — und jede Einstellung, jeder Wert deines GGS-Controllers lebt als native Home-Assistant-Entität. <b>Ohne Cloud. Ohne App. Ohne Raspberry Pi.</b>
</p>

<p align="center">
  <a href="https://matthias1232.github.io/spider-farmer-homeassistant/installer/"><img src="https://img.shields.io/badge/🚀_Web_Installer-Jetzt_flashen-2ea44f?style=for-the-badge" alt="Web Installer"></a>
  &nbsp;
  <a href="https://matthias1232.github.io/spider-farmer-homeassistant/demo/"><img src="https://img.shields.io/badge/👁️_Live_Demo-Ansehen-1f6feb?style=for-the-badge" alt="Live Demo"></a>
</p>

### Wie es funktioniert

SpiderBridge ist ein einziger ESP32, der zwischen deinem Spider-Farmer-GGS-Controller und deinem
Home Assistant sitzt. Drei Verbindungen sind im Spiel, und sie machen komplett unterschiedliche
Dinge — die Reihenfolge ist wichtig, denn jede richtet die nächste ein.

**1. USB — dein Browser → der ESP32 (Einrichtung)**

Alles beginnt mit einem **USB-Datenkabel** und Google Chrome oder Microsoft Edge. Der Installer in
deinem Browser spricht über die **Web-Serial-API** (`navigator.serial`) mit dem Board — also über
genau das Kabel, das du eingesteckt hast. Nichts anderes wird benutzt, um das Board zu erreichen.
Über dieses eine Kabel

- **flasht der Installer die Firmware** mit
  [esptool-js](https://github.com/espressif/esptool-js), der Bibliothek, auf der ESP Web Tools
  aufbaut,
- **sendet er Name und Passwort deines Heim-WLANs** mit dem
  [Improv-Wi-Fi-Serial](https://www.improv-wifi.com/serial/)-Protokoll, das die Bridge auf UART0 —
  also ihrem USB-Port — neben der normalen Log-Ausgabe beantwortet,
- **vergibt er ein neues Zufallspasswort für den Hotspot der Bridge** und
- kann **die Bridge bitten, deinen GGS-Controller zu suchen**.

**An diesem Pfad ist überhaupt kein Bluetooth beteiligt.** Dein Browser öffnet nie eine
Bluetooth-Verbindung (`navigator.bluetooth` wird weder vom Installer noch von irgendeiner Seite in
diesem Repository benutzt), und das Bluetooth deines Computers wird mit nichts gepaart. Das Board
wird per Kabel erreicht, Punkt. Das ist Absicht: Flashen, WLAN-Einrichtung und die Konsole
funktionieren auch auf Rechnern ganz ohne Bluetooth.

**2. Bluetooth — der eigene Funk des ESP32 → der Controller (einmalig)**

Das Bluetooth, das hier zählt, ist der **eigene Bluetooth-Adapter des ESP32**, nicht der deines
Computers. Die Bridge ist der BLE-*Central*: Wenn du sie bittest, startet sie in einen kurzen
Nur-Bluetooth-Boot, scannt die Umgebung nach Controllern und verbindet sich mit dem, den du
auswählst.

Über diese Verbindung macht sie genau eine Sache: **Sie gibt dem Controller sein neues WLAN.** Der
GGS stellt einen Setup-Service bereit (`0x00FF`) mit einer Write-Characteristic (`0xFF02`) und einer
Notify-Characteristic (`0xFF01`). Die Bridge schreibt einen einzigen AES-128-CBC-verschlüsselten
JSON-Frame hinein:

```json
{"method":"setWifi","params":{"ssid":"SpiderBridge-xxxx","pass":"<Hotspot-Passwort>"},"msgId":"…","pid":"…","uid":"…"}
```

SSID rein, Passwort rein — das ist die ganze Bluetooth-Geschichte. **Bluetooth ist nicht der
Datenkanal und überträgt keinen einzigen Grow-Wert.** Nach diesem einmaligen Handshake ist die
Bridge mit Bluetooth fertig und der Controller wechselt auf WLAN.

Zwei Details, die du kennen solltest:

- **Der Controller läuft mit der Spider-Farmer-App weiter.** Direkt nach `setWifi` hört der
  Controller auf zu advertisen, deshalb kann die Bridge `setDevDeactive` senden und ihn wieder
  sichtbar machen — dein Handy kann ihn dann pairen wie bisher.
- **Warum die Bridge dafür neu startet.** Bluetooth und die TLS-Sitzung des Controllers brauchen auf
  einem ESP32 denselben internen RAM. Bluetooth wird daher nur für die wenigen Sekunden
  eingeschaltet, die es braucht; das Ergebnis wird gespeichert, und die Bridge bootet zurück in den
  Normalbetrieb, wo sie diesen Speicher komplett der TLS-Sitzung gibt. Im Installer siehst du, wie
  das Board dabei auf USB schweigt.

Nach diesem einmaligen Handshake ist die Bridge mit Bluetooth fertig — der Rest läuft über WLAN.

**3. WLAN — Controller → ESP32-Hotspot → Home Assistant (alle Daten)**

Das ist der echte Datenpfad, und mit Bluetooth hat er nichts zu tun. Die Bridge betreibt **AP + STA
gleichzeitig**: Sie ist ihr eigener WLAN-Access-Point (`SpiderBridge-…`) *und* normaler Client in
deinem Heimnetz.

```text
   GGS-Controller ──WLAN──▶ ESP32-Hotspot ──DNS-Hijack──▶ TLS lokal beendet
                                     │                              │
                                     │                     MQTT-Pakete geparst
                                     │                              │
                              optionaler Cloud-Mirror       normalisiert + publiziert
                                     │                              │
                           Spider-Farmer-Cloud ──?──▶ HA-Broker ──▶ Home Assistant
```

Der Controller ist dafür gebaut, `sf.mqtt.spider-farmer.com:8883` anzusprechen. Der DNS-Proxy der
Bridge beantwortet genau diesen einen Namen mit ihrer eigenen Adresse, damit der Controller seine
MQTT-Verbindung **zur Bridge** aufbaut. Ab da:

1. **Beendet die Bridge die TLS-Sitzung des Controllers lokal**, mit dem
   Spider-Farmer-Zertifikatsmaterial (`firmware/ggs/certs/`), sodass der Controller glaubt, er rede
   mit dem Broker des Herstellers.
2. **Seine MQTT-3.1.1-Pakete werden auf dem Draht geparst** — `CONNECT`, `PUBLISH`, `SUBSCRIBE`,
   `PINGREQ` und der Rest.
3. Das JSON in den Publishes (`getDevSta`, `getConfigField`, …) wird **normalisiert** und als
   retainte Topics an *deinen* Broker veröffentlicht:
   `spiderfarmer/<id>/state/temperature`, `spiderfarmer/<id>/state/fan`,
   `spiderfarmer/<id>/state/plan`, … Jedes einzelne wird über **MQTT Discovery** angekündigt, damit
   Home Assistant ohne jede YAML-Zeile ein natives Gerät daraus macht.
4. **Befehle fahren denselben Weg zurück.** Ein Schalter in Home Assistant, und die Bridge empfängt
   `spiderfarmer/<id>/command/<field>/set`, baut den **vollständigen** Modul-Block, den der
   Controller erwartet — Teilblöcke verwirft er stillschweigend — und injiziert den passenden
   `setConfigField`-Frame in genau dieselbe Sitzung. Der nächste Status-Frame des Controllers
   bestätigt die Änderung, und die Entities aktualisieren sich.

Dieser Pfad funktioniert sogar **ohne konfigurierten MQTT-Broker**: Die Bridge schickt die Befehle
dann einfach durch die abgefangene Sitzung, und das Web-UI der Bridge bleibt die Bedienoberfläche.

Ein Controller pro Bridge: Die Firmware unterstützt genau eine aktive Controller-Sitzung.

**4. Cloud-Mirroring — optional und abschaltbar**

Wenn du möchtest, dass die Spider-Farmer-App oder -Cloud den Controller weiter erreicht, kann die
Bridge die Sitzung an den echten Broker weiterspiegeln. Es gibt zwei unabhängige Schalter, denn sie
stoppen unterschiedliche Dinge:

| Schalter | Was er steuert | Wo du ihn findest |
|---|---|---|
| **WAN-Gate** | NAT-Routing für Hotspot-Clients. Aus = der Controller kommt über die Bridge nicht hinaus. | Web-UI, `switch.internet_for_controllers`, `spiderfarmer/<id>/command/wan/set` |
| **Cloud-Forward** | Ob die Bridge die Sitzung an die echte Spider-Farmer-Cloud weiterleitet. Aus = die Bridge antwortet dem Controller selbst. | Web-UI, Home Assistant |

NAT allein abzuschalten macht das Setup **nicht** lokal — die eigene Verbindung der Bridge zur Cloud
läuft über dein Heimnetz und umgeht NAT vollständig. Für ein wirklich offline betriebenes Setup
müssen beide Schalter aus sein. Mit abgeschaltetem Cloud-Forward antwortet die Bridge dem Controller
selbst (`CONNACK`, `SUBACK`, `PUBACK`, `PINGRESP`), sodass er verbunden bleibt, weiterhin Status
veröffentlicht und nicht in eine Reconnect-Schleife fällt. Die Steuerung aus Home Assistant
funktioniert in beiden Fällen — sie hat den Cloud-Weg nie benutzt.

### Warum SpiderBridge?

Spider Farmer erzwingt für seinen GGS-Controller einen Cloud-Account, eine eigene App und schickt deine Grow-Daten durch fremde Server. **SpiderBridge dreht das um**: Dein Controller redet nie wieder mit einem Spider-Farmer-Server — er spricht im eigenen WLAN mit einem ~10 € teuren ESP32, und Home Assistant sieht alles so, als wäre der Controller ein natives HA-Gerät.

| 🔒 **100 % lokal** | 💸 **Unter 15 € Setup** | 🧠 **1:1 zur App** |
|---|---|---|
| Keine Spider-Farmer-Cloud, keine App, keine Account-Pflicht. Die Internet-Verbindung des Controllers lässt sich komplett ausschalten — zwei Schalter, beide aus, und nichts verlässt dein Netzwerk. | Ein ESP32 (WROOM-32, 4 MB Flash) reicht. Kein Raspberry Pi, kein Docker, kein Kabel-Salat. | **183 HA-Entities** (153 pro Controller + 30 Bridge-Diagnose) — jede Licht-, Lüfter-, Outlet-, Alarm- und Plan-Einstellung der App ist abgedeckt. |
| Einrichtung über das **USB-Kabel** mit Chrome oder Edge; Bluetooth wird genau einmal benutzt, um dem Controller sein WLAN zu geben. Home Assistant ↔ Bridge per MQTT im LAN. | Flash via USB-Datenkabel, einstecken, fertig. Kein Löten, kein Terminal, keine Toolchain. | Tag/Nacht-Ziele, 13 Alarmtypen, Grow-Pläne mit Templates, Sensor-Kalibrierung, einmalige Bluetooth-Provisionierung — komplett. |

### ⚡ Installation im Browser — kein Terminal, kein Löten

<p align="center">
  <a href="https://matthias1232.github.io/spider-farmer-homeassistant/installer/"><img src="docs/img/web/settings.png" alt="SpiderBridge Web Installer — Wi-Fi, Hotspot, Bluetooth und MQTT direkt im Browser" width="100%"></a>
</p>

**So schnell geht's:**

1. 🌐 **Installer öffnen** → [matthias1232.github.io/spider-farmer-homeassistant/installer/](https://matthias1232.github.io/spider-farmer-homeassistant/installer/) (Chrome oder Edge, Desktop)
2. 🔌 **Board anstecken** → per USB-Datenkabel, dann *Select board & start* klicken
3. 📶 **WLAN eingeben** → esptool-js flasht die Firmware über USB, und Improv Wi-Fi Serial schickt deine Heimnetz-Credentials über dasselbe Kabel
4. 🔗 **(Optional) Quick Connect** → die Bridge macht die Bluetooth-Arbeit danach selbst: Sie sucht deinen GGS-Controller und gibt ihm SSID und Passwort des Hotspots

Das war's. Keine Kommandozeile, kein `idf.py`, keine YAML-Dateien. Der ESP32 holt sich die Firmware direkt aus deinem Browser über das USB-Kabel, tritt deinem WLAN bei und ist ab da unter **`http://192.168.10.1`** auf seinem eigenen `SpiderBridge`-Hotspot erreichbar — oder unter der Adresse, die er in deinem Heimnetz bekommen hat. Beides zeigt dir im Installer das Werkzeug *IP addresses & status*.

> 💡 **Tipp:** Wenn der Flash mit *Failed to initialize* scheitert, halte die **BOOT**-Taste auf dem Board gedrückt, während du *Install* klickst, und lass sie los, sobald der Fortschrittsbalken startet.

**Unterstützte Hardware:** Jeder ESP32 mit **4 MB Flash** (WROOM-32, WROVER, DevKitC, NodeMCU). Entwickelt und getestet mit dem [QIQIAZI ESP32 NodeMCU Development Board (2-pack, USB-C)](https://www.amazon.de/dp/B0DHRV7784?&linkCode=ll2&tag=matthias1232-20&linkId=c71aee711cb280677528abe8e058e53c&ref_=as_li_ss_tl) — über den Affiliate-Link unterstützt du das Projekt ohne Mehrkosten.

### Was es macht

- **Beide Lichter** — on/off, Helligkeit, Manuell / Schedule / PPFD-Modus, Fade-Time, Dim- und Off-Threshold, PPFD-Ziel mit Min/Max-Helligkeit
- **Beide Lüfter** — Zirkulation und Abluft, alle Modi (Manuell / Schedule / Cycle / Environment mit Temp-/Feuchte-Priorisierung), Oszillation, Naturwind, CO₂-Schließung während Blowbetrieb
- **Klima-Zubehör** — Heizung, Befeuchter, Entfeuchter einzeln schaltbar
- **10 Outlets** der Power-Strips
- **Tag/Nacht-Zyklus** — eigene Zielwerte für Temperatur, Feuchte und CO₂, plus Totband
- **13 Alarmtypen** — Luft-/Substrat-Temp. & -Feuchte, VPD, CO₂, PPFD, Sensor-Offline, Licht-Übertemperatur, Entfeuchter-Tank, Befeuchter-Wasser, Wasserleck, Wassermangel
- **Grow-Pläne** — mehrstufige Pläne aus Templates, Stage-Labels mit Start/Ende, Farben, Erinnerungen
- **Sensor-Kalibrierung** — Offsets für Temperatur, Feuchte, CO₂ und PPFD
- **Sensor-Reinigung** — inkl. Phasenstatus, Abkühlzeit und Restlaufzeit
- **Zeit & Zeitzone** — automatische Sommerzeit per Zonenregeln oder manuell, Ein-Klick-Sync zum Controller
- **Bluetooth-Pairing** — Pair/Unpair-Button pro Controller
- **Live-Werte** — Lufttemperatur/-feuchte, CO₂, VPD, PPFD, Substrat-Temp./Feuchte/EC, Controller-Uptime, WiFi-Signal, freier Speicher
- **MQTT Discovery** — **183 native HA-Entities** ohne YAML-Konfiguration
- **Wartung** — Live-Log, Syslog, Backup/Restore, OTA-Update, Watchdogs + Safe Mode

<p align="center">
  <a href="docs/img/web/settings.png"><img src="docs/img/web/settings.png" alt="Einstellungen: Wi-Fi, Hotspot, Bluetooth, MQTT" width="32%"></a>
  <a href="docs/img/web/control.png"><img src="docs/img/web/control.png" alt="Steuerung: Lichter, Lüfter, Klima, Outlets" width="32%"></a>
  <a href="docs/img/web/status.png"><img src="docs/img/web/status.png" alt="Status: Uplink, Hotspot, Speicher, Systemlog" width="32%"></a>
</p>

### Schnellstart in 3 Schritten

```
1. Flashen   →  Installer öffnen, WLAN eingeben, "Install" klicken.
2. MQTT      →  Broker in der Bridge eintragen (oder Mosquitto-Add-on nutzen).
3. Pairen    →  "Control → Bluetooth: scan" oder "Pair Bluetooth"-Button in HA.
```

### 🏠 Home Assistant — vollständig integriert

Jeder GGS-Controller erscheint als **eigenes Gerät mit 153 Entities** in Home Assistant, die Bridge bringt **30 weitere Diagnose-Entities** mit (Uhrzeit, Zeitzone, Netzwerk-Switches, Uplink-/Hotspot-Diagnose, Restart, Factory-Reset). Alle Entities sind nativ — du kannst sie in Dashboards, Automations, Sprachassistenten, Energie-Dashboard und Szenen verwenden, ohne YAML.

Die vollständigen Entity-Tabellen, die ausführliche Installations-Anleitung, Architektur-Details, Build-Anleitung und Credits findest du oben in der [englischen Version](#technical-details-for-tinkerers) — der Inhalt ist 1:1, nur die Sprache ist anders.

### 🛡️ Die Bridge kommt von alleine wieder

Pullen am Stromkabel ist **nie** nötig. Mehrere unabhängige Schichten führen jeweils zu einem automatischen Neustart, und die USB-Konsole antwortet durchgehend:

- **Hardware-Watchdogs** — Task-Watchdog (8 s, beide Cores), Interrupt-Watchdog (800 ms), Bootloader-Watchdog
- **Supervisor** (`firmware/ggs/main/supervisor.c`) — Core-Tasks melden sich regelmäßig; hängende Tasks oder Speicherlecks (< 20 KB freier Heap für 90 s) lösen Neustart aus
- **Boot-Loop-Schutz** — drei Neustarts in Folge ohne 2 Minuten stabilen Lauf starten den **Safe Mode** (nur Hotspot + Web-UI + USB-Konsole, kein Controller-Connect) — du kannst ihn weiterhin konfigurieren und flashen
- **Rollback bei schlechtem Update** — das vorherige Firmware-Image wird wiederhergestellt, wenn die neue Firmware nicht startet
- **USB-Konsole schaltet nie ab** — nach jedem Flash, Restart oder Crash antwortet sie wieder

### ⭐ Wenn dir das Projekt hilft

- Gib einen **Stern** ⭐ — hilft anderen, das Repo zu finden.
- ☕ **[Buy me a coffee](https://www.buymeacoffee.com/matthias1232)** — freiwillig, und derselbe Link, den die *About*-Seite der Firmware zeigt.
- 📦 **[Supported ESP32 board (Amazon)](https://www.amazon.de/dp/B0DHRV7784?&linkCode=ll2&tag=matthias1232-20&linkId=c71aee711cb280677528abe8e058e53c&ref_=as_li_ss_tl)** — Affiliate-Link, ohne Mehrkosten für dich.

</details>

---

## License

[GPL-3.0-or-later](LICENSE) — see [LICENSE](LICENSE).
