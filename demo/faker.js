/*
 * SpiderBridge demo faker (GitHub Pages demo of the bridge web interface).
 *
 * The demo pages are the REAL interface pages, extracted from the firmware
 * sources (scripts/extract_gui.py). This file intercepts window.fetch and
 * answers every JSON endpoint of the bridge with randomized demo data, so the
 * pages render and behave like a live bridge — without any hardware.
 * POST requests are simulated as successful.
 *
 * Endpoint shapes mirror the firmware handlers (status_page.c, control_page.c,
 * network_page.c, config_portal.c, device_cache.c build_device()).
 */
(function () {
  'use strict';

  var CALLS = 0;
  function rnd(a, b) { return a + Math.random() * (b - a); }
  function pick(a) { return a[Math.floor(Math.random() * a.length)]; }
  function ri(a, b) { return Math.floor(rnd(a, b + 1)); }

  var NOW0 = Date.now();
  var sysSeq = 0;
  var logSeq = 0;
  var logDir = 0;

  // ------------------------------------------------------------------ syslog
  var SYSLOG_SAMPLES = [
    'mqtt: connected to broker (session present)',
    'ggs_ble: scan finished, 1 device found',
    'ha_mqtt: published 154 discovery payloads',
    'device_cache: ggs_1 status frame updated',
    'config_poll: 14 fields refreshed for ggs_1',
    'mitm_proxy: session for 7C:2C:67:F0:3D:AC alive (keepalive 12s)',
    'syslog_fwd: 128 lines sent',
    'ota_update: remote check: up to date'
  ];

  function syslogLines(after) {
    var lines = [];
    var n = ri(1, 3);
    for (var i = 0; i < n; i++) {
      sysSeq++;
      lines.push({
        seq: sysSeq,
        ms: Math.floor(rnd(1000, 4000000)),
        text: pick(SYSLOG_SAMPLES)
      });
    }
    return lines;
  }

  // -------------------------------------------------------------- MQTT log
  var LOG_TOPICS = [
    ['spiderfarmer/ggs_1/state/fan', '{"state":"ON","percentage":60,"mode_label":"Manual"}'],
    ['spiderfarmer/ggs_1/state/light', '{"state":"ON","brightness":80,"effect":"Manual"}'],
    ['spiderfarmer/ggs_1/state/temperature', '24.8'],
    ['spiderfarmer/ggs_1/state/humidity', '63'],
    ['spiderfarmer/ggs_1/state/co2', '980'],
    ['spiderfarmer/ggs_1/command/fan/preset_mode/set', 'Cycle'],
    ['spiderfarmer/ggs_1/command/outlet_3/set', 'ON'],
    ['spiderfarmer/ggs_1/state/outlet_3', 'ON'],
    ['homeassistant/fan/spiderfarmer_ggs_1_fan/config', '{...discovery...}']
  ];
  var LOG_DIRS = ['UP', 'DOWN', 'HA_OUT', 'HA_IN'];

  function logEvents() {
    var e = [];
    var n = ri(2, 5);
    for (var i = 0; i < n; i++) {
      var t = pick(LOG_TOPICS);
      logSeq++;
      logDir = (logDir + 1) % 4;
      e.push({
        s: logSeq,
        m: Math.floor(rnd(200, 900000)),
        d: logDir,
        t: pick(['pub', 'sub', 'disc', 'cmd']),
        o: t[0],
        p: t[1]
      });
    }
    return e;
  }

  // ------------------------------------------------------------------ status
  function statusData() {
    var q = ri(45, 95);
    var iplike = function (a) { return a + '.' + ri(1, 254); };
    var mac = function () {
      var h = '0123456789ABCDEF';
      return Array.from({ length: 6 }, function () {
        return pick(h.split('')).toString() + pick(h.split(''));
      }).join(':');
    };
    return {
      wifi: {
        sta_enabled: true,
        sta_has_ip: true,
        ssid: 'MyHomeWiFi',
        bssid: mac(),
        channel: ri(1, 11),
        rssi: -ri(38, 72),
        quality: q,
        ip: iplike(192) + '.' + ri(1, 254),
        netmask: '255.255.255.0',
        gateway: '192.168.1.1',
        dns: '192.168.1.1',
        disconnects: ri(0, 6),
        ap_ssid: 'SpiderBridge',
        ap_ip: '192.168.10.1',
        ap_subnet: '255.255.255.0',
        ap_channel: 6,
        ap_clients: ri(1, 2),
        ap_mac: mac()
      },
      wan: Math.random() < 0.5,
      mqtt: {
        configured: true,
        connected: true,
        broker: 'mqtt://homeassistant.local:1883',
        last_error: '',
        connects: ri(2, 9),
        disconnects: ri(0, 2),
        publishes: ri(4000, 90000),
        uptime_s: ri(300, 90000),
        hist: Array.from({ length: 4 }, function (_, i) {
          return { s: i + 1, ms: 1234 + i * 500, w: 'CD', d: 0, p: ri(1, 40), pub: ri(1, 400) };
        })
      },
      syslog: {
        enabled: true,
        connected: Math.random() < 0.7,
        proto: 0,
        host: '192.168.1.10',
        port: 514,
        sent: ri(200, 5000),
        dropped: ri(0, 3),
        reconnects: ri(0, 2),
        last_error: ''
      },
      devices: [{
        name: 'GGS',
        mac: '7C:2C:67:F0:3D:AC',
        slug: 'ggs_1',
        online: true,
        silence_s: ri(2, 12),
        keepalive_s: 12
      }],
      time: {
        synced: true,
        now: new Date().toISOString().slice(0, 19),
        zone: 'CET-1CEST,M3.5.0,M10.5.0/3',
        abbrev: 'CEST',
        is_dst: true,
        utc_offset_min: 120,
        server: 'pool.ntp.org',
        name: 'Europe/Berlin',
        dst_mode: pick([0, 1, 2])
      },
      tz_push: true,
      fw: {
        version: 'v1.4.2',
        built: 'Oct  8 2026 19:04:11',
        slot: 'ota_0',
        ota_url: 'https://matthias1232.github.io/spider-farmer-homeassistant/firmware.json',
        ota_checked: true,
        ota_available: Math.random() < 0.3,
        ota_remote: 'v1.5.0',
        ota_last: 'Oct  8 2026 20:31:00',
        ota_error: ''
      },
      uptime_s: ri(3600, 900000),
      heap_free: ri(60000, 180000),
      heap_min: ri(40000, 120000),
      heap_low_note: 'none',
      heap_largest: ri(40000, 100000),
      web: { served: ri(1000, 9000), shed: ri(0, 40), busy: 0 },
      stack_free: { mqtt_task: ri(800, 3000), sb_session: ri(1200, 4000), httpd: ri(1500, 5000), config_poll: ri(1000, 4000), devsave: ri(2000, 4000) },
      reset_reason: 'SW reboot (software reset)',
      ble: {
        released: true,
        pending: false,
        found: 1,
        last: 'connected GGS (PS5)'
      }
    };
  }

  // ----------------------------------------------------------------- control
  function fanBlock(circ) {
    var lvl = circ ? ri(1, 10) : ri(25, 100);
    return {
      on: Math.random() < 0.8,
      level: lvl,
      shake: circ ? ri(0, 10) : undefined,
      shake_last: circ ? ri(0, 10) : undefined,
      natural: Math.random() < 0.3,
      co2: circ ? undefined : Math.random() < 0.5,
      maxSpeed: circ ? String(ri(1, 10)) : String(ri(25, 100)),
      minSpeed: circ ? 'Off' : pick(['Off', String(ri(25, 100))]),
      mode_label: pick(['Manual', 'Schedule', 'Cycle', 'Environment: Prioritize temperature', 'Environment: Temperature only']),
      sched_start: '06:00',
      sched_end: '22:00',
      cyc_start: '08:00',
      cyc_run: 15,
      cyc_off: 45,
      cyc_run_t: '00:15:00',
      cyc_off_t: '00:45:00',
      cyc_times: ri(1, 24)
    };
  }

  function lightBlock() {
    return {
      on: Math.random() < 0.7,
      level: ri(11, 100),
      mode_label: pick(['Manual', 'Schedule', 'PPFD']),
      dark: ri(0, 30),
      offT: ri(0, 30),
      ppfd_min: ri(11, 40),
      ppfd_max: ri(70, 100),
      sched_start: '06:00',
      sched_end: '24:00',
      sched_bri: ri(40, 100),
      fade: pick([0, 15, 30, 60]),
      ppfd_start: '06:00',
      ppfd_end: '23:30',
      ppfd: ri(500, 900),
      ppfd_fade: pick([0, 15, 30])
    };
  }

  function buildDevice() {
    return {
      mac: '7C:2C:67:F0:3D:AC',
      slug: 'ggs_1',
      name: 'GGS',
      def_name: 'GGS',
      def_slug: 'ggs_1',
      online: true,
      fan: fanBlock(true),
      blower: fanBlock(false),
      light: lightBlock(),
      light2: lightBlock(),
      climate: [
        { key: 'heater', name: 'Heater', on: Math.random() < 0.4 },
        { key: 'humidifier', name: 'Humidifier', on: Math.random() < 0.4 },
        { key: 'dehumidifier', name: 'Dehumidifier', on: Math.random() < 0.3 }
      ],
      target: {
        dayTime: { startTime: '06:00', endTime: '22:00' },
        temp: { targetDay: 25, targetNight: 18, deadband: 2 },
        humi: { targetDay: 65, targetNight: 55, deadband: 5 },
        co2: { targetDay: 1000, targetNight: 800, deadband: 100 }
      },
      has_plan: true,
      lplan_active: false,
      today: (2026 << 16) | (10 << 8) | 8,
      cal: { temp: 0, humi: 0, co2: 0, ppfd: 0 },
      clean: { on: false, known: true, phase: 0, remain: 0 },
      alarm: {
        temp: { enabled: true, min: 18, max: 30 },
        humi: { enabled: true, min: 40, max: 80 },
        co2: { enabled: false, min: 400, max: 1500 }
      },
      alarm_spec: {
        r: [
          { k: 'temp', l: 'Air Temperature', u: '°C', mn: true, nlo: 0, nhi: 30, xlo: 18, xhi: 50, st: 1 },
          { k: 'co2', l: 'CO2', u: 'ppm', mn: true, nlo: 200, nhi: 1500, xlo: 450, xhi: 5000, st: 10 }
        ],
        s: [
          { k: 'devOffline', l: 'Sensor Offline' },
          { k: 'waterLeak', l: 'Water Leak' }
        ]
      },
      tz_name: 'Europe/Berlin',
      tz_posix: 'CET-1CEST,M3.5.0,M10.5.0/3',
      sys: { fwVersion: 'v1.4.0', hwVersion: 'v2.1', date: 'Oct  8 2026' },
      sensors: {
        temperature: (+rnd(21, 28).toFixed(1)) + ' °C',
        humidity: ri(50, 75) + ' %',
        co2: ri(600, 1200) + ' ppm',
        vpd: (+rnd(0.4, 1.6).toFixed(2)) + ' kPa',
        ppfd: ri(400, 950) + ' µmol/m²/s',
        tempSoil: (+rnd(18, 24).toFixed(1)) + ' °C',
        humiSoil: ri(45, 70) + ' %',
        ecSoil: (+rnd(1.0, 3.2).toFixed(1)) + ' mS/cm'
      },
      outlets: Array.from({ length: 4 }, function (_, i) {
        return { key: 'outlet_' + (i + 1), name: 'Outlet ' + (i + 1), on: Math.random() < 0.5 };
      }),
      plan: ''
    };
  }

  function controlData() {
    return {
      ver: ri(1, 999),
      plan_max: 12,
      session: true,
      tz_push: true,
      bridge_tz: 'Europe/Berlin',
      bridge_name: 'SpiderBridge',
      bridge_def: 'SpiderBridge',
      bridge_is_def: true,
      devices: [buildDevice()]
    };
  }

  // ----------------------------------------------------------------- network
  function networkData() {
    return {
      clients: [{
        mac: '7C:2C:67:F0:3D:AC',
        rssi: -ri(40, 70),
        listed: true,
        ip: '192.168.10.101',
        name: 'GGS'
      }],
      mac_mode: 1,
      mac_list: [{ mac: '7C:2C:67:F0:3D:AC', label: 'GGS' }],
      ip_mode: 0,
      ip_list: [{ idx: 0, cidr: '192.168.10.0/24', note: 'hotspot clients' }]
    };
  }

  // --------------------------------------------------------------------- ble
  function bleData() {
    return {
      running: false,
      scanned: true,
      busy: '',
      result: 'connected GGS',
      trace: '',
      ap_ssid: 'SpiderBridge',
      dev: [{
        addr: '7c:2c:67:f0:3d:ac',
        name: 'GGS Controller (PS5)',
        rssi: -ri(45, 75),
        known_as: 'GGS',
        session: true,
        wifi_mac: '7C:2C:67:F0:3D:AC',
        pcode: 1004,
        adv_active: true,
        adv_wifi: true,
        adv_cloud: false
      }]
    };
  }

  // --------------------------------------------------------------------- log
  function logData() {
    return { e: logEvents() };
  }

  var GETTERS = {
    '/status/data': function () { return statusData(); },
    '/syslog/data': function () { return { busy: false, lines: syslogLines() }; },
    '/control/data': function () { return controlData(); },
    '/control/zones': function () { return null; },
    '/control/templates': function () {
      return [
        { key: 'seedling', name: 'Seedling', days: 14, stages: [] },
        { key: 'veg', name: 'Vegetative', days: 28, stages: [] },
        { key: 'flower', name: 'Flowering', days: 56, stages: [] }
      ];
    },
    '/control/ver': function () { return { app: 'v1.4.2', plan: 1 }; },
    '/control/lplan': function () { return ''; },
    '/network/data': function () { return networkData(); },
    '/ble/data': function () { return bleData(); },
    '/ble/log': function () { return { lines: [] }; },
    '/log/data': function () { return logData(); },
    '/wifi/scan': function () {
      return Array.from({ length: 5 }, function () {
        var ssid = pick(['MyHomeWiFi', 'FRITZ!Box 7590', 'RepeaterEG', 'GGS-Net', 'IoT']);
        return { ssid: ssid, rssi: -ri(35, 89), ch: ri(1, 11), open: ssid === 'IoT' };
      });
    },
    '/wifi/status': function () {
      return { running: false, ok: true, msg: 'connected' };
    },
    '/update/check': function () {
      return { ok: true, available: Math.random() < 0.3, remote: 'v1.5.0' };
    },
    '/update/remote': function () { return { ok: true }; }
  };

  var POSTS = [
    '/save', '/control/set', '/control/device', '/control/plan', '/control/template',
    '/ble/act', '/network/act', '/status/dst', '/log/send', '/update', '/reboot',
    '/system/reset', '/restore', '/wifi/connect'
  ];

  function json(data) {
    return new Response(JSON.stringify(data), {
      status: 200,
      headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' }
    });
  }

  var realFetch = window.fetch.bind(window);

  window.fetch = function (input, init) {
    var url = typeof input === 'string' ? input : (input && input.url) || String(input);
    var path;
    try { path = new URL(url, location.href).pathname; } catch (e) { path = url; }
    var method = (init && init.method) || (input && input.method) || 'GET';
    CALLS++;

    if (method === 'GET') {
      if (path === '/backup') {
        return Promise.resolve(new Response(JSON.stringify({ demo: true }, null, 2), {
          status: 200,
          headers: {
            'Content-Type': 'application/json',
            'Content-Disposition': 'attachment; filename="spiderbridge-backup-demo.json"'
          }
        }));
      }
      var getter = GETTERS[path];
      if (getter) {
        // /ble/act style async jobs: simulate a short delay for realism
        return new Promise(function (res) {
          setTimeout(function () { res(json(getter())); }, path === '/ble/data' ? 150 : 60);
        });
      }
    } else if (POSTS.indexOf(path) !== -1) {
      // Simulate a successful POST. /ble/act triggers a rescan-shaped answer.
      if (path === '/ble/act' && /a=scan/.test(String(url))) {
        return new Promise(function (res) {
          setTimeout(function () { res(json({ ok: true })); }, 400);
        });
      }
      return new Promise(function (res) {
        setTimeout(function () { res(json({ ok: true })); }, 120);
      });
    }

    return realFetch(input, init);
  };

  // Small helper the extracted pages could call; harmless for the real GUI.
  window.spiderbridgeDemo = { calls: function () { return CALLS; } };
})();
