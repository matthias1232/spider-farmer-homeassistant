<p align="center">
  <a href="#"><img src="docs/img/logo.png" alt="SpiderBridge" width="460"></a>
</p>

<p align="center">
  <strong>SpiderBridge â€” dein Spider Farmer Grow Controller, direkt in Home&nbsp;Assistant.</strong><br>
  Einmal flashen, einmal pairen â€” und jede Einstellung, jeder Wert deines GGS-Controllers lebt als native Home-Assistant-EntitÃ¤t. <b>Ohne Cloud, ohne App, ohne Raspberry Pi.</b>
</p>

<p align="center">
  <a href="https://matthias1232.github.io/spider-farmer-homeassistant/installer/"><img src="https://img.shields.io/badge/ðŸš€_Web_Installer-Jetzt_flashen-2ea44f?style=for-the-badge" alt="Web Installer"></a>
  &nbsp;
  <a href="https://matthias1232.github.io/spider-farmer-homeassistant/demo/"><img src="https://img.shields.io/badge/ðŸ‘ï¸_Live_Demo-Ansehen-1f6feb?style=for-the-badge" alt="Live Demo"></a>
  &nbsp;
  <a href="https://github.com/matthias1232/spider-farmer-homeassistant/stargazers"><img src="https://img.shields.io/github/stars/matthias1232/spider-farmer-homeassistant?style=for-the-badge" alt="Stars"></a>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Keine_Cloud-100%25_lokal-blue?style=flat-square" alt="Keine Cloud">
  <img src="https://img.shields.io/badge/Hardware-ab_~10â‚¬-blue?style=flat-square" alt="Hardware ~10â‚¬">
  <img src="https://img.shields.io/badge/Installation-3_Schritte-blue?style=flat-square" alt="3 Schritte">
  <img src="https://img.shields.io/badge/License-GPLv3-blue?style=flat-square" alt="GPLv3">
  <img src="https://img.shields.io/badge/Platform-ESP32_WROOM--32-blue?style=flat-square" alt="ESP32">
  <img src="https://img.shields.io/github/actions/workflow/status/matthias1232/spider-farmer-homeassistant/build.yml?style=flat-square" alt="Build">
</p>

---

## Warum SpiderBridge?

Spider Farmer verlangt fÃ¼r seinen GGS-Controller eine Cloud-Account-Pflicht, eine eigene App
und zwingt dich, deine Grow-Daten durch fremde Server zu schicken. **SpiderBridge dreht das um**:
dein Controller spricht nur noch per Bluetooth mit einem ~10&nbsp;â‚¬ teuren ESP32, und Home
Assistant sieht alles so, als wÃ¤re der Controller ein natives HA-GerÃ¤t.

| ðŸ”’ **100 % lokal** | ðŸ’¸ **Unter 15 â‚¬ Setup** | ðŸ§  **1:1 zur App** |
|---|---|---|
| Keine Spider-Farmer-Cloud, keine App, keine Account-Pflicht. Internet fÃ¼r den Controller lÃ¤sst sich komplett ausschalten. | Ein ESP32 (WROOM-32, 4 MB Flash) reicht. Kein Raspberry Pi, kein Docker, kein Kabel-Salat. | **183 HA-Entities** (153 pro Controller + 30 Bridge-Diagnose) â€” jede Licht-, LÃ¼fter-, Outlet-, Alarm- und Plan-Einstellung der App ist abgedildet. |
| Controller â†” Bridge per verschlÃ¼sseltem Bluetooth LE. Home Assistant â†” Bridge per MQTT im LAN. | Flash via USB-Datenkabel, einstecken, fertig. Kein LÃ¶ten, kein Terminal, keine Toolchain. | Tag/Nacht-Ziele, 13 Alarmtypen, Grow-PlÃ¤ne mit Templates, Sensor-Kalibrierung, Bluetooth-Pairing â€” komplett. |

---

## âš¡ Installation im Browser â€” kein Terminal, kein LÃ¶ten

<p align="center">
  <a href="https://matthias1232.github.io/spider-farmer-homeassistant/installer/"><img src="docs/img/web/settings.png" alt="SpiderBridge Web Installer â€” Wi-Fi, Hotspot, Bluetooth und MQTT direkt im Browser" width="100%"></a>
</p>

**So schnell geht's:**

1. ðŸŒ **Installer Ã¶ffnen** â†’ [matthias1232.github.io/spider-farmer-homeassistant/installer/](https://matthias1232.github.io/spider-farmer-homeassistant/installer/) (Chrome oder Edge, Desktop)
2. ðŸ”Œ **Board anstecken** â†’ per USB-Datenkabel, dann *Select board & start* klicken
3. ðŸ“¶ **WLAN eingeben** â†’ Improv Serial schickt deine Heimnetz-Credentials direkt an den ESP32
4. ðŸ”— **(Optional) Quick Connect** â†’ koppelt deinen GGS-Controller per Bluetooth mit dem Bridge-Hotspot

Das war's. Keine Kommandozeile, kein `idf.py`, keine YAML-Dateien. Der ESP32 holt sich die
Firmware direkt aus dem Browser, tritt deinem WLAN bei und ist ab da Ã¼ber `http://spiderbridge.local`
erreichbar.

> ðŸ’¡ **Tipp:** Wenn der Flash mit *Failed to initialize* scheitert, halte die **BOOT**-Taste
> auf dem Board gedrÃ¼ckt, wÃ¤hrend du *Install* klickst, und lass sie los, sobald der
> Fortschrittsbalken startet.

**UnterstÃ¼tzte Hardware:** Jeder ESP32 mit **4 MB Flash** (WROOM-32, WROVER, DevKitC, NodeMCU).
Entwickelt und getestet mit dem
[QIQIAZI ESP32 NodeMCU Development Board (2-pack, USB-C)](https://www.amazon.de/dp/B0DHRV7784?&linkCode=ll2&tag=matthias1232-20&linkId=c71aee711cb280677528abe8e058e53c&ref_=as_li_ss_tl) â€” Ã¼ber den
Affiliate-Link unterstÃ¼tzt du das Projekt ohne Mehrkosten.

---

## Was es macht

- **Beide Lichter** â€” on/off, Helligkeit, Manuell / Schedule / PPFD-Modus, Fade-Time, Dim- und
  Off-Threshold, PPFD-Ziel mit Min/Max-Helligkeit
- **Beide LÃ¼fter** â€” Zirkulation und Abluft, alle Modi (Manuell / Schedule / Cycle / Environment
  mit Temp-/Feuchte-Priorisierung), Oszillation, Naturwind, COâ‚‚-SchlieÃŸung wÃ¤hrend Blowbetrieb
- **Klima-ZubehÃ¶r** â€” Heizung, Befeuchter, Entfeuchter einzeln schaltbar
- **10 Outlets** der Power-Strips
- **Tag/Nacht-Zyklus** â€” eigene Zielwerte fÃ¼r Temperatur, Feuchte und COâ‚‚, plus Totband
- **13 Alarmtypen** â€” Luft-/Substrat-Temp.&Feuchte, VPD, COâ‚‚, PPFD, Sensor-Offline,
  Licht-Ãœbertemperatur, Entfeuchter-Tank, Befeuchter-Wasser, Wasserleck, Wassermangel
- **Grow-PlÃ¤ne** â€” mehrstufige PlÃ¤ne aus Templates, Stage-Labels mit Start/Ende, Farben,
  Erinnerungen
- **Sensor-Kalibrierung** â€” Offsets fÃ¼r Temperatur, Feuchte, COâ‚‚ und PPFD
- **Sensor-Reinigung** â€” inkl. Phasenstatus, AbkÃ¼hlzeit und Restlaufzeit
- **Zeit & Zeitzone** â€” automatische Sommerzeit per Zonenregeln oder manuell, Ein-Klick-Sync
  zum Controller
- **Bluetooth-Pairing** â€” Pair/Unpair-Button pro Controller
- **Live-Werte** â€” Lufttemperatur/-feuchte, COâ‚‚, VPD, PPFD, Substrat-Temp./Feuchte/EC,
  Controller-Uptime, WiFi-Signal, freier Speicher
- **MQTT Discovery** â€” **183 native HA-Entities** ohne YAML-Konfiguration
- **Wartung** â€” Live-Log, Syslog, Backup/Restore, OTA-Update, Watchdogs + Safe Mode

<p align="center">
  <a href="docs/img/web/settings.png"><img src="docs/img/web/settings.png" alt="Settings: Wi-Fi, Hotspot, Bluetooth, MQTT" width="32%"></a>
  <a href="docs/img/web/control.png"><img src="docs/img/web/control.png" alt="Control: Lichter, LÃ¼fter, Klima, Outlets" width="32%"></a>
  <a href="docs/img/web/status.png"><img src="docs/img/web/status.png" alt="Status: Uplink, Hotspot, Speicher, Systemlog" width="32%"></a>
</p>

---

## Schnellstart in 3 Schritten

```
1ï¸âƒ£  Flashen    â†’  Installer Ã¶ffnen, WLAN eingeben, "Install" klicken.
2ï¸âƒ£  MQTT       â†’  Broker in der Bridge eintragen (oder eigenen Mosquitto-Add-on nutzen).
3ï¸âƒ£  Pairen     â†’  "Control â†’ Bluetooth: scan" oder "Pair Bluetooth"-Button in HA drÃ¼cken.
```

Die ausfÃ¼hrliche Schritt-fÃ¼r-Schritt-Anleitung mit Troubleshooting findest du unten im
[technischen Anhang](#technische-details-fÃ¼r-bastler).

---

## ðŸ  Home Assistant â€” vollstÃ¤ndig integriert

Jeder GGS-Controller erscheint als **eigenes GerÃ¤t mit 153 Entities** in Home Assistant, die
Bridge bringt **30 weitere Diagnose-Entities** mit (Uhrzeit, Zeitzone, Netzwerk-Switches,
Uplink-/Hotspot-Diagnose, Restart, Factory-Reset). Alle Entities sind nativ â€” du kannst sie in
Dashboards, Automations, Sprachassistenten, Energie-Dashboard und Szenen verwenden, ohne YAML.

**GGS-Controller â€” [controller-page.png](docs/img/ha/controller-page.png)** (alle 153 Entities in einer Aufnahme):

<p align="center">
  <a href="docs/img/ha/controller-page.png"><img src="docs/img/ha/controller-page.png" alt="VollstÃ¤ndige Device-Page des GGS-Controllers in Home Assistant, 153 Entities" width="100%"></a>
</p>

**SpiderBridge â€” [bridge-page.png](docs/img/ha/bridge-page.png)** (alle 30 Entities in einer Aufnahme):

<p align="center">
  <a href="docs/img/ha/bridge-page.png"><img src="docs/img/ha/bridge-page.png" alt="VollstÃ¤ndige Device-Page der Bridge in Home Assistant, 30 Entities" width="100%"></a>
</p>

---

## So funktioniert es

![Architektur â€” Controller spricht per Bluetooth mit dem ESP32, die Bridge spricht per MQTT mit Home Assistant](docs/img/architecture.png)

<details>
<summary>Text-Diagramm</summary>

```
â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”   Bluetooth LE    â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
â”‚  GGS Controller    â”‚â—„â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â–ºâ”‚  ESP32 "SpiderBridge"         â”‚
â”‚  (sensors, lights, â”‚  encrypted GGS    â”‚  â€¢ ggs_ble.c     BLE client   â”‚
â”‚   fans, outletsâ€¦)  â”‚   protocol        â”‚  â€¢ ha_mqtt.c     MQTT + HA    â”‚
â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜                   â”‚  â€¢ Web-UI (Settings, Control, â”‚
                                         â”‚    Status, Network, Log,      â”‚
        â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”       â”‚    Firmware)                  â”‚
        â”‚  Home Assistant        â”‚       â”‚  â€¢ MQTT Discovery, 183        â”‚
        â”‚  (auto-discovered      â”‚â—„â”€â”€â”€â”€â”€â”€â”‚    Entities                  â”‚
        â”‚   Entities)            â”‚ MQTT  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
        â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
```

</details>

- **`ggs_ble.c`** â€” BLE-Client fÃ¼r den GATT-Service des Controllers (UUID `0x00FF`),
  verschlÃ¼sseltes GGS-Protokoll, produktspezifische AES-Keys.
- **`sf_normalizer.c` / `sf_command_handler.c`** â€” Ã¼bersetzt die Controller-Frames in
  HA-freundliche Werte und Befehle.
- **`ha_mqtt.c` + `ha_discovery_table.c`** â€” verÃ¶ffentlicht State-Topics und
  Discovery-Payloads, sodass HA den Controller nativ erkennt.
- **`mitm_proxy.c` + `wan_gate.c`** â€” optionaler Pfad fÃ¼r Controller, die sich mit dem
  Bridge-Hotspot verbinden: die Bridge beendet deren TLS-Sitzung lokal und reicht sie an
  die echte Spider-Farmer-Cloud weiter. *LÃ¤sst sich komplett ausschalten.*

---

## ðŸ›¡ï¸ Die Bridge kommt von alleine wieder

Pullen am Stromkabel ist **nie** nÃ¶tig. Mehrere unabhÃ¤ngige Schichten fÃ¼hren jeweils zu einem
automatischen Neustart, und die USB-Konsole antwortet durchgehend:

- **Hardware-Watchdogs** â€” Task-Watchdog (8 s, beide Cores), Interrupt-Watchdog (800 ms),
  Bootloader-Watchdog
- **Supervisor** (`firmware/ggs/main/supervisor.c`) â€” Core-Tasks melden sich regelmÃ¤ÃŸig;
  hÃ¤ngende Tasks oder Speicherlecks (< 20 KB freier Heap fÃ¼r 90 s) lÃ¶sen Neustart aus
- **Boot-Loop-Schutz** â€” drei Neustarts in Folge ohne 2 Minuten stabilen Lauf starten den
  **Safe Mode** (nur Hotspot + Web-UI + USB-Konsole, kein Controller-Connect) â€” du kannst
  ihn weiterhin konfigurieren und flashen
- **Rollback bei schlechtem Update** â€” das vorherige Firmware-Image wird wiederhergestellt,
  wenn die neue Firmware nicht startet
- **USB-Konsole schaltet nie ab** â€” nach jedem Flash, Restart oder Crash antwortet sie wieder

Auf einem echten Board getestet mit `python scripts/reboot_soak.py COM3 --mode fault` (crash,
hang, watchdog, leak, safe mode) und `--mode command` (30Ã— Neustart, ~11 s je Zyklus, Speicher
stabil).

---

## â­ Wenn dir das Projekt hilft

- Gib einen **Stern** â­ â€” hilft anderen, das Repo zu finden.
- â˜• **[Donate via PayPal](https://www.paypal.com/paypalme/matthias1232)** â€” freiwillig,
  PayPal-me-Button in der Sidebar verlinkt auf dasselbe Konto.
- ðŸ“¦ **[Supported ESP32 board (Amazon)](https://www.amazon.de/dp/B0DHRV7784?&linkCode=ll2&tag=matthias1232-20&linkId=c71aee711cb280677528abe8e058e53c&ref_=as_li_ss_tl)**
  â€” Ã¼ber den Affiliate-Link ohne Mehrkosten fÃ¼r dich.

---

## Technische Details (fÃ¼r Bastler)

<details>
<summary><strong>AusfÃ¼hrliche Installations-Anleitung (3 Schritte)</strong></summary>

### 1. Firmware flashen (WLAN-Credentials inklusive)

1. Ã–ffne den [Web-Installer](https://matthias1232.github.io/spider-farmer-homeassistant/installer/)
   in **Chrome oder Edge** auf einem Desktop und stecke das Board mit einem USB-*Daten*-Kabel an.
2. *Quick Connect* hat zwei Buttons mit identischem Ablauf; nur der Flash-Schritt unterscheidet sich:
   - **"New board: install + set up"** installiert die neueste Firmware (lÃ¶scht das Board, falls
     angekreuzt) und richtet sie anschlieÃŸend ein.
   - **"Board already flashed: set up only"** flasht nichts. Es startet das Board neu, prÃ¼ft, dass
     SpiderBridge lÃ¤uft, und richtet es dann ein. Nutze es direkt nach einem Flash oder fÃ¼r jede
     Bridge, die noch keinen Controller kennt.

   Gib dein Heim-WLAN ein und klicke *Select board & start*; Chrome Ã¶ffnet die
   Seriell-Port-Liste, du wÃ¤hlst das Board, und der Wizard
   - verbindet die Bridge mit deinem Heim-WLAN,
   - vergibt dem Bridge-Hotspot ein **neues zufÃ¤lliges Passwort** (wird am Ende einmal angezeigt:
     aufschreiben!),
   - und verbindet â€” wenn die Bluetooth-Box angekreuzt ist â€” den GGS-Controller mit dem Hotspot.
     Der Controller bleibt Ã¼ber Bluetooth sichtbar, du kannst also weiterhin die Spider-Farmer-App
     nutzen. Nur Controller mit starkem Signal (< -75 dBm) werden angefasst, und der Schalter ist
     nach diesem einen Start verbraucht.

   Eine Bridge, die **bereits in Betrieb** ist (Heimnetz gesetzt und Controller bekannt), hÃ¶rt
   absichtlich nicht auf USB-Befehle, weil dieser Speicher fÃ¼r die Controller-Verbindung gebraucht
   wird. *Set up only* sagt das dann und bietet *Erase, install firmware and continue*; alternativ
   Ã¤ndere WLAN und Hotspot-Passwort in der Bridge-Web-UI. Ein Board mit Ã¤lterer oder fremder
   Firmware bekommt stattdessen *Install firmware and continue* angezeigt.
3. **Bereits geflasht, oder Einzelschritte gewÃ¼nscht:** klicke *Open installer & device tools*. Das
   Fenster bietet *Install*, *IP addresses & status*, *Send Wi-Fi to GGS Controller* (sucht den
   Spider-Farmer-Controller per Bluetooth und schickt ihm das Hotspot-WLAN), *Home Wi-Fi for the
   bridge*, *Connect Wi-Fi & randomize SpiderBridge Wi-Fi password*, *Randomize SpiderBridge Wi-Fi
   password* und *Logs & Console*. (Du kannst auch dem `SpiderBridge`-Hotspot beitreten und unter
   `http://192.168.10.1` konfigurieren.)
4. **Module** auf *Auto-detect* lassen (oder dein Modul wÃ¤hlen, z. B. ESP32-WROOM-32).

> ðŸ’¡ **Install fails with "Failed to initialize"?** Viele ESP32-Boards kÃ¶nnen den
> Download-Modus nicht selbstÃ¤ndig starten. Halte die **BOOT**-Taste gedrÃ¼ckt, wÃ¤hrend du *Install*
> (oder *Select board & start*) klickst, und lass sie los, sobald der Fortschrittsbalken startet.

Die zusÃ¤tzlichen Wizard-Befehle (IP-Adressen, Hotspot-Passwort, Quick Connect) sind
SpiderBridge-Erweiterungen von [Improv Wi-Fi Serial](https://www.improv-wifi.com/serial/)
(Befehle `0x40`â€“`0x46` in `firmware/ggs/main/improv_serial.c`); andere Improv-Clients ignorieren
sie. Die Firmware-Seite ist durch einen Host-Test gedeckt
(`python firmware/ggs/host_test/run.py`), der Installer durch
`node tests/installer_quickconnect.test.mjs` und `node tests/installer_console.test.mjs` â€” beide
laufen in CI, bevor die Firmware gebaut wird.

Der Installer bietet immer den **letzten erfolgreichen CI-Build** an; du kannst auch jeden
Commit selbst bauen (siehe *Build from source*).

*Auto-detect* liest den Chip-Family vom verbundenen Board und installiert das passende Build. Die
**Module**-Liste zeigt nur Module, die wirklich gebaut wurden â€” aktuell **ESP32**
(WROOM-32 / WROVER / DevKitC / NodeMCU, â‰¥ 4 MB). ESP32-S2/S3/C3/C6 werden noch nicht
unterstÃ¼tzt; der Installer weist darauf hin, statt das falsche Image zu flashen.

### 2. Bridge mit Home Assistant verbinden

1. In Home Assistant: **Einstellungen â†’ GerÃ¤te & Dienste â†’ MQTT** (Mosquitto-Add-on installieren
   oder HA auf einen bestehenden Broker zeigen lassen).
2. Broker in der Bridge-Web-UI eintragen (Settings â†’ MQTT) â€” oder die Bridge ihren eigenen
   Broker auf dem Hotspot betreiben lassen.
3. Die Bridge meldet sich per MQTT Discovery an; die Diagnose-Entities (Bridge-Speicher, Firmware)
   erscheinen sofort.

### 3. GGS-Controller koppeln (Bluetooth)

1. In der Bridge-Web-UI **Control â†’ Bluetooth: scan** Ã¶ffnen
   (oder den *Pair Bluetooth*-Button in Home Assistant drÃ¼cken).
2. Die Bridge startet kurz in den Bluetooth-only-Modus, sucht und listet die gefundenen
   Controller â€” mit SignalstÃ¤rke, Produkt-Code und ob sie schon bekannt sind.
3. Controller auswÃ¤hlen. Die Bridge Ã¼bergibt die Hotspot-Credentials, aktiviert das GerÃ¤t auf
   dem Bluetooth-Link und bootet direkt zurÃ¼ck in den Normalbetrieb.
4. Wenige Sekunden spÃ¤ter erscheint der Controller als eigenes GerÃ¤t mit allen 153 Entities in
   Home Assistant.

**Fertig** â€” die Spider-Farmer-App wurde kein einziges Mal geÃ¶ffnet.

> ðŸ’¡ **Du nutzt die Spider-Farmer-App bereits?** Du kannst sie weiter nutzen. Einmal nach der
> Installation zeigt der Controller per WLAN auf den Bridge-Hotspot â€” danach erreicht die App
> den Controller Ã¼ber die Bridge (und die Cloud-Verbindung des Controllers kannst du in der
> Web-UI komplett abschalten), wÃ¤hrend Home Assistant alles parallel sieht.

</details>

<details>
<summary><strong>Hardware-Empfehlung</strong></summary>

Das Board, mit dem die Firmware entwickelt und getestet wurde:

<p align="center"><img src="docs/img/esp32-board.png" alt="ESP32-WROOM-32 Board" width="420"></p>

**â†’ [QIQIAZI ESP32 NodeMCU Development Board (2-pack, ESP32-WROOM-32, 4 MB, USB-C) â€” Amazon](https://www.amazon.de/dp/B0DHRV7784?&linkCode=ll2&tag=matthias1232-20&linkId=c71aee711cb280677528abe8e058e53c&ref_=as_li_ss_tl)**

Jedes andere ESP32-Board mit â‰¥ 4 MB Flash funktioniert ebenso. Ãœber den Affiliate-Link
unterstÃ¼tzt du das Projekt ohne Mehrkosten.

ZusÃ¤tzlich nÃ¶tig:

- **Spider Farmer GGS Controller** mit Bluetooth (alle aktuellen Modelle: CB / PS5 / PS10 / LC)
- **Home Assistant** mit MQTT-Integration (jeder Broker, z. B. Mosquitto)
- Der Controller braucht **keine** Wi-Fi-Credentials und **keine** Spider-Farmer-App â€” er wird
  komplett Ã¼ber Bluetooth durch die Bridge provisioniert

</details>

<details>
<summary><strong>Alle 183 HA-Entities</strong> â€” 153 pro Controller, 30 auf der Bridge (Control = schreibbar, Sensor = read-only)</summary>

<!-- ENTITY-LIST:START -->
Grouped by function. *Kind*: Control = writable entity, Sensor = read-only.

| Entity | Platform | Kind | Unit |
|---|---|---|---|
| Fan | fan | Control | â€” |
| Fan Cycle Off Time | time | Control (config) | â€” |
| Fan Cycle Repeats | select | Control (config) | â€” |
| Fan Cycle Run Time | time | Control (config) | â€” |
| Fan Cycle Start Time | time | Control (config) | â€” |
| Fan Mode | select | Control | â€” |
| Fan Natural Wind | switch | Control | â€” |
| Fan Oscillation | fan | Control | â€” |
| Fan Schedule End Time | time | Control (config) | â€” |
| Fan Schedule Speed | select | Control (config) | â€” |
| Fan Schedule Start Time | time | Control (config) | â€” |
| Fan Speed | select | Control (config) | â€” |
| Fan Standby Speed | select | Control (config) | â€” |
| Close CO2 While Blower Runs | switch | Control | â€” |
| Fan Exhaust | fan | Control | â€” |
| Fan Exhaust Cycle Off Time | time | Control (config) | â€” |
| Fan Exhaust Cycle Repeats | select | Control (config) | â€” |
| Fan Exhaust Cycle Run Time | time | Control (config) | â€” |
| Fan Exhaust Cycle Start Time | time | Control (config) | â€” |
| Fan Exhaust Mode | select | Control | â€” |
| Fan Exhaust Schedule End Time | time | Control (config) | â€” |
| Fan Exhaust Schedule Speed | select | Control (config) | â€” |
| Fan Exhaust Schedule Start Time | time | Control (config) | â€” |
| Fan Exhaust Speed | select | Control (config) | â€” |
| Fan Exhaust Standby Speed | select | Control (config) | â€” |
| Light 1 | light | Control | â€” |
| Light 1 Dim Threshold | select | Control (config) | â€” |
| Light 1 Off Threshold | select | Control (config) | â€” |
| Light 1 PPFD End Time | time | Control (config) | â€” |
| Light 1 PPFD Fade Time | select | Control (config) | â€” |
| Light 1 PPFD Max Brightness | number | Control (config) | % |
| Light 1 PPFD Min Brightness | number | Control (config) | % |
| Light 1 PPFD Start Time | time | Control (config) | â€” |
| Light 1 PPFD Target | number | Control (config) | Âµmol/mÂ²/s |
| Light 1 Schedule Brightness | number | Control (config) | % |
| Light 1 Schedule End Time | time | Control (config) | â€” |
| Light 1 Schedule Fade Time | select | Control (config) | â€” |
| Light 1 Schedule Start Time | time | Control (config) | â€” |
| Light 2 | light | Control | â€” |
| Light 2 Dim Threshold | select | Control (config) | â€” |
| Light 2 Off Threshold | select | Control (config) | â€” |
| Light 2 PPFD End Time | time | Control (config) | â€” |
| Light 2 PPFD Fade Time | select | Control (config) | â€” |
| Light 2 PPFD Max Brightness | number | Control (config) | % |
| Light 2 PPFD Min Brightness | number | Control (config) | % |
| Light 2 PPFD Start Time | time | Control (config) | â€” |
| Light 2 PPFD Target | number | Control (config) | Âµmol/mÂ²/s |
| Light 2 Schedule Brightness | number | Control (config) | % |
| Light 2 Schedule End Time | time | Control (config) | â€” |
| Light 2 Schedule Fade Time | select | Control (config) | â€” |
| Light 2 Schedule Start Time | time | Control (config) | â€” |
| Day Cycle End | time | Control (config) | â€” |
| Day Cycle Start | time | Control (config) | â€” |
| Target CO2 Day | number | Control (config) | ppm |
| Target CO2 Deadband | number | Control (config) | ppm |
| Target CO2 Night | number | Control (config) | ppm |
| Target Humidity Day | number | Control (config) | % |
| Target Humidity Deadband | number | Control (config) | % |
| Target Humidity Night | number | Control (config) | % |
| Target Temperature Day | number | Control (config) | Â°C |
| Target Temperature Deadband | number | Control (config) | Â°C |
| Target Temperature Night | number | Control (config) | Â°C |
| Outlet 1 | switch | Control | â€” |
| Outlet 10 | switch | Control | â€” |
| Outlet 2 | switch | Control | â€” |
| Outlet 3 | switch | Control | â€” |
| Outlet 4 | switch | Control | â€” |
| Outlet 5 | switch | Control | â€” |
| Outlet 6 | switch | Control | â€” |
| Outlet 7 | switch | Control | â€” |
| Outlet 8 | switch | Control | â€” |
| Outlet 9 | switch | Control | â€” |
| Switch Dehumidifier | switch | Control | â€” |
| Switch Heater | switch | Control | â€” |
| Switch Humidifier | switch | Control | â€” |
| CO2 Offset | number | Control (config) | ppm |
| Humidity Offset | number | Control (config) | % |
| PPFD Offset | number | Control (config) | â€” |
| Temperature Offset | number | Control (config) | Â°C |
| Sensor Cleaning | switch | Control | â€” |
| Sensor Cleaning Phase Ends | sensor | Sensor | â€” |
| Sensor Cleaning Status | sensor | Sensor | â€” |
| Sensor Cleaning Time Left | sensor | Sensor | â€” |
| Alarm Air Humidity | switch | Control (config) | â€” |
| Alarm Air Humidity Maximum | number | Control (config) | % |
| Alarm Air Humidity Minimum | number | Control (config) | % |
| Alarm Air Temperature | switch | Control (config) | â€” |
| Alarm Air Temperature Maximum | number | Control (config) | Â°C |
| Alarm Air Temperature Minimum | number | Control (config) | Â°C |
| Alarm CO2 | switch | Control (config) | â€” |
| Alarm CO2 Maximum | number | Control (config) | ppm |
| Alarm CO2 Minimum | number | Control (config) | ppm |
| Alarm Dehumidifier Water Tank Full | switch | Control (config) | â€” |
| Alarm Humidifier Water Low | switch | Control (config) | â€” |
| Alarm Light Over-Temperature | switch | Control (config) | â€” |
| Alarm PPFD | switch | Control (config) | â€” |
| Alarm PPFD Maximum | number | Control (config) | Âµmol/mÂ²/s |
| Alarm PPFD Minimum | number | Control (config) | Âµmol/mÂ²/s |
| Alarm Sensor Offline | switch | Control (config) | â€” |
| Alarm Substrate EC | switch | Control (config) | â€” |
| Alarm Substrate EC Maximum | number | Control (config) | mS/cm |
| Alarm Substrate EC Minimum | number | Control (config) | mS/cm |
| Alarm Substrate Moisture | switch | Control (config) | â€” |
| Alarm Substrate Moisture Maximum | number | Control (config) | % |
| Alarm Substrate Moisture Minimum | number | Control (config) | % |
| Alarm Substrate Temperature | switch | Control (config) | â€” |
| Alarm Substrate Temperature Maximum | number | Control (config) | Â°C |
| Alarm Substrate Temperature Minimum | number | Control (config) | Â°C |
| Alarm VPD | switch | Control (config) | â€” |
| Alarm VPD Maximum | number | Control (config) | kPa |
| Alarm VPD Minimum | number | Control (config) | kPa |
| Alarm Water Leak | switch | Control (config) | â€” |
| Alarm Water Shortage | switch | Control (config) | â€” |
| Last Alarm Device (code) | sensor | Sensor (diagnostic) | â€” |
| Last Alarm Number | sensor | Sensor (diagnostic) | â€” |
| Last Alarm Time | sensor | Sensor (diagnostic) | â€” |
| Last Alarm Time (epoch) | sensor | Sensor (diagnostic) | â€” |
| Last Alarm Type (code) | sensor | Sensor (diagnostic) | â€” |
| Add Plan Stage From Template | button | Control (config) | â€” |
| Grow Plan | switch | Control | â€” |
| Plan Kept On Bridge | binary_sensor | Sensor (diagnostic) | â€” |
| Plan Stage | sensor | Sensor (diagnostic) | â€” |
| Plan Stage Color | sensor | Sensor (diagnostic) | â€” |
| Plan Stage Day | sensor | Sensor (diagnostic) | d |
| Plan Stage End | sensor | Sensor (diagnostic) | â€” |
| Plan Stage Reminder | sensor | Sensor (diagnostic) | â€” |
| Plan Stage Start | sensor | Sensor (diagnostic) | â€” |
| Plan Stages | sensor | Sensor (diagnostic) | â€” |
| Plan Template | select | Control (config) | â€” |
| Pair Bluetooth (stop advertising) | button | Control (config) | â€” |
| Unpair Bluetooth | button | Control (config) | â€” |
| Time Zone | text | Control (config) | â€” |
| Sync Device Time | button | Control (config) | â€” |
| Summer Time | switch | Control (config) | â€” |
| Controller Internet Access | switch | Control (config) | â€” |
| Air CO2 | sensor | Sensor | ppm |
| Air Humidity | sensor | Sensor | % |
| Air PPFD | sensor | Sensor | Âµmol/mÂ²/s |
| Air Temperature | sensor | Sensor | Â°C |
| Air VPD | sensor | Sensor | kPa |
| Soil Average EC | sensor | Sensor | mS/cm |
| Soil Average Humidity | sensor | Sensor | % |
| Soil Average Temperature | sensor | Sensor | Â°C |
| Controller Firmware | sensor | Sensor (diagnostic) | â€” |
| Controller Free Memory | sensor | Sensor (diagnostic) | â€” |
| Controller Hardware | sensor | Sensor (diagnostic) | â€” |
| Controller Restarts | sensor | Sensor (diagnostic) | â€” |
| Controller Signal | sensor | Sensor (diagnostic) | dBm |
| Controller Time | sensor | Sensor (diagnostic) | â€” |
| Controller Uptime | sensor | Sensor (diagnostic) | â€” |
| Controller Uptime Seconds | sensor | Sensor (diagnostic) | s |
| Daylight Saving Rules | sensor | Sensor (diagnostic) | â€” |
| Firmware Built | sensor | Sensor (diagnostic) | â€” |
| Firmware Updated | sensor | Sensor (diagnostic) | â€” |
| Apply clock to all controllers | switch | Control (config) | â€” |
| Bridge time | sensor | Sensor | â€” |
| Clock synchronised | binary_sensor | Sensor | â€” |
| Daylight saving | select | Control | â€” |
| Follow the zone rules | switch | Control | â€” |
| NTP server | text | Control | â€” |
| Redirect time requests | switch | Control (config) | â€” |
| Summer time | switch | Control | â€” |
| Time sync while offline | switch | Control (config) | â€” |
| Time zone | select | Control | â€” |
| Time zone (type any) | text | Control (config) | â€” |
| Internet for controllers | switch | Control (config) | â€” |
| Mirror to the Spider Farmer cloud | switch | Control (config) | â€” |
| Name resolution while offline | switch | Control (config) | â€” |
| Redirect name lookups | switch | Control (config) | â€” |
| Hotspot clients | sensor | Sensor (diagnostic) | â€” |
| Uplink address | sensor | Sensor (diagnostic) | â€” |
| Uplink connected | binary_sensor | Sensor (diagnostic) | â€” |
| Uplink drops | sensor | Sensor (diagnostic) | â€” |
| Uplink network | sensor | Sensor (diagnostic) | â€” |
| Uplink quality | sensor | Sensor (diagnostic) | % |
| Uplink signal | sensor | Sensor (diagnostic) | dBm |
| Bridge Firmware | sensor | Sensor (diagnostic) | â€” |
| Bridge Free Memory | sensor | Sensor (diagnostic) | â€” |
| Firmware version | sensor | Sensor (diagnostic) | â€” |
| Free memory | sensor | Sensor (diagnostic) | kB |
| Uptime | sensor | Sensor (diagnostic) | s |
| Device name | text | Control (config) | â€” |
| Factory reset (type RESET) | text | Control (config) | â€” |
| Restart bridge | button | Control (config) | â€” |

**183 Entities insgesamt** â€” 153 pro Controller + 30 auf der Bridge (binary_sensor Ã— 3, button Ã— 5, fan Ã— 3, light Ã— 2, number Ã— 36, select Ã— 21, sensor Ã— 46, switch Ã— 42, text Ã— 5, time Ã— 20). Generiert aus der Firmware (`ha_discovery_table.c` und `ha_mqtt.c`) durch `scripts/gen_entity_docs.py`; nicht manuell editieren.
<!-- ENTITY-LIST:END -->
</details>

<details>
<summary><strong>Build from source</strong></summary>

Das Repository ist so aufgebaut, dass pro unterstÃ¼tzter Controller-Familie ein eigenes
Verzeichnis existiert. `firmware.json` beschreibt, was es gibt; jedes gelistete Target wird von
CI gebaut und erscheint in der **Module**-Liste des Installers:

```json
[
  { "id": "ggs", "dir": "firmware/ggs", "targets": ["esp32"],
    "modules": { "esp32": { "label": "ESP32 (WROOM-32 / WROVER / DevKitC / NodeMCU)" } } }
]
```

**GitHub Actions** (dieses Repository) erzeugt die flashbaren Builds â€” nach *jeder* Code-Ã„nderung
sofort:

- Push auf `main` â†’ Build + Publish (Installer und Demo zeigen immer den neuesten Build)
- Actions-Tab â†’ **Run workflow** (`workflow_dispatch`) â†’ Build on demand, ohne Push
- Tag `v*` â†’ Build + die gemergten Images werden ans Release angehÃ¤ngt

Merge-Binaries und Per-Target-Manifeste werden als Workflow-Artifacts hochgeladen, du kannst
also immer den aktuellen Stand flashen, ohne lokale Toolchain.

Lokal reicht ESP-IDF v5.5:

```bash
cd firmware/ggs
idf.py set-target esp32
idf.py build
# oder mit Factory-Image:
./build.sh 1.0.0
```

Der Build erzeugt `spiderbridge_esp32_v2.bin` fÃ¼r einen 4-MB-ESP32 (Flash-Offsets: Bootloader
`0x1000`, Partition Table `0x8000`, OTA Data `0xf000`, App `0x20000`).

</details>

<details>
<summary><strong>Zertifikate (Herkunft)</strong></summary>

Das TLS-Material fÃ¼r den optionalen WLAN-Relay-Pfad (`firmware/ggs/certs/`) ist nicht frisch
fÃ¼r dieses Projekt: es ist das Spider-Farmer-Zertifikatspaar, das ursprÃ¼nglich fÃ¼r die
Python-Bridge extrahiert wurde, auf der dieser ESP32-Port basiert, und es ist dort Ã¶ffentlich
verfÃ¼gbar: **[github.com/iceboerg00/spiderfarmer-bridge](https://github.com/iceboerg00/spiderfarmer-bridge)**
(`certs/`). Details: [`firmware/ggs/certs/README.md`](firmware/ggs/certs/README.md).

</details>

<details>
<summary><strong>Credits</strong></summary>

- Die ursprÃ¼ngliche Python-Bridge, auf der diese Firmware basiert:
  [iceboerg00/spiderfarmer-bridge](https://github.com/iceboerg00/spiderfarmer-bridge)
  (inklusive des TLS-Zertifikatsmaterials in `certs/`)
- Das Protokoll der Spider-Farmer-App, beim Bau der Bridge reverse-engineered
- ESP-IDF, ESP Web Tools und Improv Serial SDK â€” alle Open Source

</details>

---

## Lizenz

[GPL-3.0-or-later](LICENSE) â€” siehe [LICENSE](LICENSE).
