#pragma once

// ============================================================================
// SpiderBridge-ESP32 v2 — minimal rebuild
//
// This firmware does exactly four things: run a Wi-Fi hotspot for the GGS
// controller, terminate its TLS connection, forward the raw MQTT packets to
// a Home Assistant broker, and inject commands back into the controller
// session. The controller traffic is also mirrored 1:1 to the real Spider
// Farmer cloud so the vendor app keeps working.
//
// Nothing else. No kill switch, no live log, no MAC filter, no IPv6, no
// NTP/DNS redirection, no multi-device registry, no remote console, no
// Wi-Fi firmware upload. Those all existed in the previous version and were
// removed to cut down the surface area for bugs.
// ============================================================================

// --- Hotspot for the grow controller (AP interface) ---
// First boot and after a factory reset: a known hotspot, so the settings
// page is always reachable. Change the password there (Generate button).
#define SB_AP_SSID_DEFAULT        "SpiderBridge"
#define SB_AP_PASS_DEFAULT        "12345678"
#define SB_AP_CHANNEL_DEFAULT     6
#define SB_AP_IP_DEFAULT          "192.168.10.1"
#define SB_AP_MAX_CONN            4

// --- Uplink into the home network (STA interface) ---
// Empty on purpose: values come from NVS, set through the configuration
// portal at http://192.168.10.1. The factory image carries no credentials.
#define SB_STA_SSID_DEFAULT       ""
#define SB_STA_PASS_DEFAULT       ""

// --- Spider Farmer cloud ---
// The hostname the controller resolves on its own and tries to reach
// directly. The bridge hijacks the DNS answer for this one name so the
// controller connects to US instead, and separately mirrors the traffic
// onward to the real address so the vendor app keeps working.
#define SB_SF_CLOUD_HOST          "sf.mqtt.spider-farmer.com"
#define SB_SF_CLOUD_PORT          8883

// Local TLS listener the controller actually connects to.
#define SB_LOCAL_TLS_PORT         8883

// --- Home Assistant MQTT broker ---
#define SB_HA_MQTT_URI_DEFAULT    ""
#define SB_HA_MQTT_USER_DEFAULT   ""
#define SB_HA_MQTT_PASS_DEFAULT   ""

// --- Device identity ---
#define SB_DEVICE_ID_DEFAULT      "ggs_1"

// Display name shown for the device in Home Assistant.
#define SB_HA_DEVICE_NAME         "GGS"

// --- Buffer and task sizes ---
// Deliberately tight: this board has no PSRAM, and every byte here is
// internal SRAM shared with the two simultaneous mbedTLS contexts (server
// to the controller, client to the real cloud) that are the actual memory
// bottleneck of this project.
// 6144: a grow plan stage is ~1 KB of JSON; five stages (the cap below)
// are ~5.1 KB, the setConfigField envelope ~5.3 KB, and the controller
// sends the whole plan as one TLS record. 2048 read-failed on a third
// stage and ended the session in a loop. Matches
// MBEDTLS_SSL_IN_CONTENT_LEN.
#define SB_TLS_RX_BUF_SIZE        6144

// The most stages a plan may have, and the largest frame a plan command
// may produce (both checked before anything is cached or sent).
#define SB_PLAN_MAX_STAGES        5
#define SB_PLAN_FRAME_MAX         (SB_TLS_RX_BUF_SIZE - 160)

// 0: the grow plan lives on the controller only. The bridge never writes
// the controller a partial "window" of a longer plan kept in its flash --
// that path replaced the controller's stages with two and lost the rest
// when switched off again. The bridge sends a plan only as the result of
// an explicit stage save/delete, always with the full current stage list.
#define SB_LONG_PLAN              0

// The relay buffers live on the heap, but mbedTLS still uses a lot of
// stack inside mbedtls_ssl_read() and the handshake, more so with TLS 1.3.
// 6 KB overflowed the session task right after the cloud link came up.
#define SB_PROXY_TASK_STACK       (10 * 1024)
// The listener only accepts; the handshake runs in the session task.
#define SB_PROXY_LISTENER_STACK   3072

// Refuse a new controller connection when free internal heap drops below
// this, rather than risk a handshake failing halfway through. Measured:
// a handshake plus the cloud leg takes ~26 KB (83 -> 57 KB); idle heap
// with MQTT up is ~64 KB. 70 KB therefore refused every connection in a
// loop once the plan support added its buffers; 56 KB leaves ~30 KB
// after the handshake, above the web server's own 24 KB floor.
#define SB_MIN_HEAP_FOR_SESSION   (56 * 1024)

// Refuse a web request below this, rather than fail partway through
// rendering it. Enforced in web_auth_check(), which every page handler
// already calls.
//
// Much lower than the TLS threshold above on purpose: a page render is
// a far smaller and shorter-lived allocation than a TLS session, and
// the web interface is how the device gets diagnosed and recovered, so
// it should be the last thing to stop working, not the first.
#define SB_MIN_HEAP_FOR_WEB       (24 * 1024)

// --- Multiple controllers ---------------------------------------------
// How many controllers the bridge remembers and can serve.
//
// Each one costs a cache slot of a few kilobytes. Sessions are the real
// limit: every active TLS session holds two mbedTLS contexts and roughly
// 64 KB of internal RAM, so two run comfortably and a third gets tight.
// Remembering more than that is cheap, and a remembered controller that
// is not currently connected still shows its last known values.
#define SB_MAX_DEVICES            4
#define SB_MAX_SESSIONS           2

// How long a controller may stay silent before Home Assistant is told it
// is unavailable.
//
// The controller publishes roughly every five seconds and never sends an
// MQTT keepalive — measured, not assumed: 180 PUBLISH and zero PINGREQ
// over 150 s. So liveness is derived from that publish cadence instead.
//
// The limit is eight observed intervals, clamped to this range. The
// floor keeps brief gaps during reconnects from flagging a healthy
// device; the ceiling bounds how long a departed one lingers as online.
#define SB_LIVENESS_MIN_MS        (45 * 1000)
#define SB_LIVENESS_FALLBACK_MS   (120 * 1000)

// --- What gets published to the broker --------------------------------
//
//   SB_PUB_HA    Home Assistant layer: discovery entities plus normalised
//                state topics. Commands arrive on
//                spiderfarmer/<id>/command/<field>[/<subfield>]/set
//   SB_PUB_RAW   Raw relay: every controller packet verbatim on
//                spiderfarmer/<id>/raw. Commands on .../command/raw
//   SB_PUB_BOTH  Both at once.
//
// Switching modes needs a restart because the MQTT subscriptions change.
#define SB_PUB_HA     1
#define SB_PUB_RAW    2
#define SB_PUB_BOTH   3

#define SB_PUBLISH_MODE_DEFAULT   SB_PUB_BOTH

// How often the stored configuration (schedules, cycles, PPFD) is
// re-read from the controller. Status frames do not carry it.
#define SB_CONFIG_POLL_INTERVAL_S 600

// How long the uplink may stay down before the device restarts itself.
//
// The Wi-Fi driver retries on its own and usually recovers, but it can
// give up internally, and a router that returns on a different channel
// sometimes leaves the station wedged. This device is meant to run
// unattended, so a two-second reboot beats staying dark until someone
// notices. Ten minutes is long enough to ride out an ordinary router
// restart without bouncing.
#define SB_UPLINK_REBOOT_AFTER_S  600

// --- Web interface protection -----------------------------------------
// Empty means no login. When set, every page requires HTTP basic auth
// with user "admin" and this password.
#define SB_ADMIN_PASS_DEFAULT     ""

// --- Internet access for the controller -------------------------------
// Two separate switches, because they do different things:
//
//   wan_open      NAT routing for the hotspot. With it off the controller
//                 cannot reach anything beyond the bridge.
//   cloud_forward Whether the proxy mirrors the controller's MQTT session
//                 to the real Spider Farmer cloud.
//
// Turning NAT off alone would not stop the data flow: the proxy's cloud
// connection runs over the bridge's own uplink and bypasses NAT entirely.
// Both have to be off for a genuinely local-only setup.
//
// With cloud forwarding off the bridge answers the controller itself
// (CONNACK, SUBACK, PUBACK, PINGRESP), otherwise it would consider the
// session dead and reconnect every few seconds.
#define SB_WAN_OPEN_DEFAULT       true
#define SB_CLOUD_FORWARD_DEFAULT  true

// --- Static address on the home network -------------------------------
// Empty IP means DHCP. A fixed address keeps the web interface at the
// same URL once the bridge is mounted out of reach.
#define SB_STATIC_IP_DEFAULT      ""
#define SB_STATIC_GW_DEFAULT      ""
#define SB_STATIC_MASK_DEFAULT    "255.255.255.0"

// --- Name and time service for hotspot clients ------------------------
// Each target may be an IP address or a hostname.
//
// DNS: empty means "use the resolver from the uplink's DHCP lease", which
// follows the router automatically. With dns_redirect on, every client
// lookup is answered with the configured address instead of being
// relayed, pinning clients to one service.
//
// NTP: off by default, so clients reach whichever time server they were
// built to use. Turn it on to send them somewhere local, which keeps the
// controller's clock correct even with internet access switched off.
#define SB_DNS_TARGET_DEFAULT     ""
#define SB_DNS_REDIRECT_DEFAULT   false
#define SB_NTP_TARGET_DEFAULT     "pool.ntp.org"
#define SB_NTP_REDIRECT_DEFAULT   false

// --- Exceptions while internet access is off --------------------------
// Without name resolution the controller cannot resolve anything, not
// even the bridge. Without time sync its clock drifts and schedules fire
// at the wrong time. Both stay available by default.
#define SB_ALLOW_DNS_OFFLINE_DEFAULT  true
#define SB_ALLOW_NTP_OFFLINE_DEFAULT  true

// --- Updates from a URL -----------------------------------------------
// Empty by default: nothing is contacted unless an address is entered.
// Checking is off by default too, so the bridge never reaches out on its
// own without being asked.
#define SB_OTA_URL_DEFAULT        ""
#define SB_OTA_CHECK_DEFAULT      false
#define SB_OTA_AUTO_DEFAULT       false

// Checking happens at boot, once the uplink is up. This is how long to
// wait for the network before giving up on the attempt.
#define SB_OTA_BOOT_DELAY_S       20

// --- Clock ------------------------------------------------------------
// The default TZ string is Central European Time with the EU daylight
// saving rules: forward on the last Sunday in March, back on the last
// Sunday in October. Encoding the rules rather than a fixed offset means
// the switch happens on its own, with no internet lookup.
#define SB_NTP_SERVER_DEFAULT     "pool.ntp.org"
#define SB_TZ_DEFAULT             "CET-1CEST,M3.5.0,M10.5.0/3"

#define SB_DST_AUTO               0
#define SB_DST_STANDARD           1
#define SB_DST_SUMMER             2
#define SB_DST_MODE_DEFAULT       SB_DST_AUTO

// The zone label the controller displays, alongside the POSIX rules.
#define SB_TZ_NAME_DEFAULT        "Europe/Berlin"

// Off by default: taking over a controller's clock settings is a visible
// change to a device that was working, so it should be asked for rather
// than assumed.
#define SB_TZ_PUSH_DEFAULT        false

// --- Live MQTT log (http://<bridge-ip>/log) ---------------------------
// Ring buffer of recent packets.
//
// The controller publishes roughly four packets per second, and each one
// is recorded twice (once as received, once as forwarded). At 32 entries
// the buffer wrapped in about four seconds, which was short enough that
// an injected command had already scrolled out before the page polled
// for it.
//
// The ring costs entries x payload bytes, allocated once at startup and
// held for the lifetime of the device. Measured free internal heap with
// one controller connected is about 49 KB, not the 157 KB an earlier
// comment here claimed — that figure predated the second TLS session and
// the status and system-log features.
//
// Kept small, because this buffer competes directly with the TLS
// sessions.
//
// At 18 x 700 (12.6 KB) free internal heap fell to about 62 KB, below
// the 70 KB the proxy requires before accepting a connection. The
// controller could associate and resolve names but every TLS handshake
// was refused — a diagnostic buffer had made the device unable to do
// its job.
//
// Reduced again, to 8, trading log depth for stability headroom.
//
// This ring is the largest remaining optional allocation. Removing the
// IP filter, DHCP reservations and MAC filter entirely was considered
// and measured first: together they come to only 1.2 KB, so losing
// three working security features would have bought almost nothing.
// Halving this buffer gives back more than twice that on its own.
//
// Eight entries still captures a command and the replies around it,
// which is what the log view is actually used for.
#define SB_LOG_RING_ENTRIES       8

// The log no longer stores fixed slots of SB_LOG_ENTRY_PAYLOAD each: it
// packs records into one arena at their real size. Eight fixed slots held
// eight entries no matter how short; every status frame publishes dozens of
// ~70-byte Home Assistant state updates, so a command was overwritten before
// the log page could fetch it. The same memory as the old ring (~4.2 KB plus
// a small index) now holds roughly a hundred typical entries.
// 8 KB, but only while a log page is open (see live_log.c): payloads are
// now kept whole, so the arena has to hold a few full controller frames
// (up to ~1.7 KB each) next to the many short state updates.
#define SB_LOG_ARENA_BYTES        8192
#define SB_LOG_MAX_RECORDS        96

// Enough of each payload to be useful when probing by hand.
//
// At 160 bytes a getDevSta reply was cut off before its interesting
// parts — the alarm block, for one, sat past the cut and could not be
// read at all.
//
// A getSysSta reply runs to about 640 bytes, so the tail of the longest
// frames is lost. That is the right trade: every field the firmware
// actually uses is parsed from the live JSON as it arrives, not from
// this buffer, which exists only for watching traffic by hand. The
// truncation is flagged in the log view, so nothing is silently hidden.
// Longest payload kept in the log. Raised from 448 so nothing is cut: the
// largest frames seen are getDevSta (~1.2 KB) and the alarm/config blocks
// (~500 B). Only the log's arena holds these, and only while a viewer is
// open; the HTTP handler streams them in pieces without a buffer this size.
#define SB_LOG_ENTRY_PAYLOAD      2048
