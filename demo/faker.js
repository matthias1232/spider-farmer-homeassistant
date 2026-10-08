/*
 * SpiderBridge demo faker (GitHub Pages demo of the bridge web interface).
 *
 * The demo pages are the REAL interface pages, extracted from the firmware
 * sources (scripts/extract_gui.py). This file intercepts window.fetch and
 * answers every JSON endpoint of the bridge with demo data that mirrors the
 * live shapes 1:1 (verified against a running bridge):
 *
 *   /control/data        streamed head + per-device blocks (fan, blower,
 *                        light, light2, climate, target, cal, clean, alarm,
 *                        alarm_spec, sys, sensors, outlets, plan)
 *   /control/zones       timezone list
 *   /control/templates  {system:[{id,tpl}],custom:[],max,free}
 *   /control/ver         {"v":<device-cache version>}
 *   /control/lplan       long plan {active,running,count,window,max,from,
 *                        last_end,stage:[...]}
 *   /status/data         wifi/wan/mqtt/syslog/devices/time/tz_push/fw/…/ble
 *   /syslog/data         {lines:[{seq,ms,text}],dropped}
 *   /network/data        clients/mac_mode/mac_list/ip_mode/ip_list
 *   /ble/data            running/scanned/busy/result/trace/ap_ssid/dev[]
 *   /ble/log             plain-text Bluetooth job log
 *   /wifi/scan           [{ssid,rssi,ch,open}]
 *   /wifi/connect        plain text, 202 Accepted
 *   /log/data            {e:[{s,m,d,t,o,p}],n:<next cursor>}
 *
 * POSTs are simulated: /wifi/connect answers 202 with the live text, BLE and
 * system actions reply with the real plain-text messages, everything else is
 * acknowledged. The Settings form, the firmware upload (XMLHttpRequest) and
 * the /backup and /ble/log links do not use fetch(), so they are handled
 * separately below.
 */
(function () {
  'use strict';

  function rnd(a, b) { return a + Math.random() * (b - a); }
  function pick(a) { return a[Math.floor(Math.random() * a.length)]; }
  function ri(a, b) { return Math.floor(rnd(a, b + 1)); }
  function mac() {
    var h = '0123456789ABCDEF';
    return Array.from({ length: 6 }, function () {
      return pick(h.split('')) + pick(h.split(''));
    }).join(':');
  }

  var CALLS = 0;
  var sysSeq = 4000;
  var logN = 190;
  var lastSsid = 'MyHomeWiFi';

  var STA_IP = '192.168.1.' + ri(50, 199);
  var GW_IP = '192.168.1.1';
  var AP_IP = '192.168.4.1';
  var AP_SUBNET = '192.168.4.0/24';
  var CLIENT_IP = '192.168.4.' + ri(2, 250);
  var AP_MAC = mac();
  var DEV_MAC = mac();
  var DEV_MAC_FLAT = DEV_MAC.replace(/:/g, '');
  var DEV_ADDR = mac();

  // ------------------------------------------------------------------ syslog
  var SYSLOG_SAMPLES = [
    'config_poll:   fan: heap {h1} -> {h2}',
    'config_poll: Asked ' + DEV_MAC_FLAT + ' for fan (heap {h2}, lowest {h2})',
    'mitm_proxy: Command injected on SF/GGS/CB/API/DOWN/' + DEV_MAC_FLAT + ' (131 bytes)',
    'config_poll:   blower: heap {h1} -> {h2}',
    'mitm_proxy: Session ' + DEV_MAC_FLAT + ' alive (keepalive 3s, silence 0s)',
    'ha_mqtt: published 152 state topics for ggs_1',
    'ha_discovery: refreshed payloads for mac ' + DEV_MAC_FLAT,
    'ggs_ble: memory released to the system'
  ].map(function (s) { return s.replace(/\{h1\}/g, function () { return ri(70000, 78000); })
                             .replace(/\{h2\}/g, function () { return ri(70000, 78000); }); });

  function syslogLines() {
    var lines = [];
    var n = ri(1, 4);
    for (var i = 0; i < n; i++) {
      sysSeq++;
      lines.push({
        seq: sysSeq,
        ms: sysSeq * 9000,
        text: (Math.random() < 0.25 ? 'W (' + (sysSeq * 9000 + 789) + ') ' : 'I (' + (sysSeq * 9000 + 789) + ') ') + pick(SYSLOG_SAMPLES)
      });
    }
    return lines;
  }

  // -------------------------------------------------------------- MQTT log
  var LOG_TOPICS = [
    'SF/GGS/CB/API/DOWN/' + DEV_MAC_FLAT,
    'SF/GGS/CB/API/UP/' + DEV_MAC_FLAT,
    'spiderfarmer/ggs_1/state/fan',
    'spiderfarmer/ggs_1/state/temperature',
    'spiderfarmer/ggs_1/command/outlet_1/set',
    'homeassistant/sensor/spiderfarmer_ggs_1_temperature/config'
  ];

  function logEvents() {
    var e = [];
    var n = ri(2, 6);
    for (var i = 0; i < n; i++) {
      logN++;
      e.push({
        s: logN,
        m: ri(200, 900000),
        d: ri(0, 3),
        t: pick(['pub', 'sub', 'disc', 'pub']),
        o: pick(LOG_TOPICS),
        p: pick(['{"state":"ON","percentage":60}', '24.8', 'ON', '{...discovery...}', '131'])
      });
    }
    return e;
  }

  // ------------------------------------------------------------------ status
  function statusData() {
    var q = ri(45, 95);
    return {
      wifi: {
        sta_enabled: true,
        sta_has_ip: true,
        ssid: 'MyHomeWiFi',
        bssid: mac(),
        channel: 1,
        rssi: -ri(38, 72),
        quality: q,
        ip: STA_IP,
        netmask: '255.255.255.0',
        gateway: GW_IP,
        dns: GW_IP,
        disconnects: ri(0, 3),
        ap_ssid: 'SpiderBridge',
        ap_ip: AP_IP,
        ap_subnet: AP_SUBNET,
        ap_channel: 1,
        ap_clients: 1,
        ap_mac: AP_MAC
      },
      wan: false,
      mqtt: {
        configured: true,
        connected: true,
        broker: 'mqtt://homeassistant.local:1883',
        last_error: '',
        connects: ri(2, 9),
        disconnects: ri(0, 2),
        publishes: ri(4000, 9200),
        uptime_s: ri(300, 18488),
        hist: [
          { s: 1, ms: 900, w: 'c', d: 0, p: 0, pub: 0 },
          { s: 2, ms: 2687517, w: 'd', d: 0, p: 0, pub: ri(1000, 1600) },
          { s: 3, ms: 2697716, w: 'c', d: 0, p: 0, pub: ri(1000, 1600) },
          { s: 4, ms: 3888748, w: 'd', d: 0, p: 0, pub: ri(1600, 2000) },
          { s: 5, ms: 3898886, w: 'c', d: 0, p: 0, pub: ri(1600, 2000) }
        ]
      },
      syslog: {
        enabled: false,
        connected: false,
        proto: 0,
        host: '',
        port: 514,
        sent: 0,
        dropped: 0,
        reconnects: 0,
        last_error: ''
      },
      devices: [{
        name: 'Demo Tent',
        mac: DEV_MAC_FLAT,
        slug: 'demo_tent',
        online: true,
        silence_s: 0,
        keepalive_s: 3
      }],
      time: {
        synced: true,
        now: new Date().toISOString().slice(0, 19).replace('T', ' '),
        zone: 'CET-1CEST,M3.5.0,M10.5.0/3',
        abbrev: 'CEST',
        is_dst: true,
        utc_offset_min: 120,
        server: 'pool.ntp.org',
        name: 'Europe/Berlin',
        dst_mode: 0
      },
      tz_push: true,
      fw: {
        version: '1.20.4',
        built: 'Oct  8 2026 19:05:14',
        slot: 'ota_1',
        ota_url: '',
        ota_checked: false,
        ota_available: false,
        ota_remote: '',
        ota_last: 'never',
        ota_error: ''
      },
      uptime_s: ri(3600, 18488),
      heap_free: ri(60000, 76000),
      heap_min: ri(33000, 50000),
      heap_low_note: '',
      heap_largest: ri(30000, 40000),
      web: { served: ri(1000, 14542), shed: 0, busy: 0 },
      stack_free: { mqtt_task: ri(2000, 2600), sb_session: ri(5000, 6000), httpd: ri(3000, 3500), config_poll: ri(1800, 2000), devsave: ri(2300, 2600) },
      reset_reason: 'Software restart',
      ble: {
        released: true,
        pending: false,
        found: 1,
        last: 'Scan finished: 1 GGS controller(s) found'
      }
    };
  }

  // ----------------------------------------------------------------- control
  function fanBlock(circ) {
    return {
      on: true,
      level: circ ? ri(1, 10) : ri(25, 100),
      shake: circ ? ri(0, 10) : 0,
      shake_last: circ ? ri(0, 10) : 0,
      natural: circ ? Math.random() < 0.6 : false,
      co2: circ ? undefined : Math.random() < 0.5,
      maxSpeed: circ ? String(ri(1, 10)) : String(ri(25, 100)),
      minSpeed: 'Off',
      mode_label: pick(['Manual', 'Schedule', 'Cycle', 'Environment: Prioritize temperature', 'Environment: Temperature only']),
      sched_start: '00:00',
      sched_end: '00:00',
      cyc_start: '00:00',
      cyc_run: 0,
      cyc_off: 0,
      cyc_run_t: '00:00:00',
      cyc_off_t: '00:00:00',
      cyc_times: 1
    };
  }

  function lightBlock() {
    return {
      on: Math.random() < 0.5,
      level: Math.random() < 0.5 ? 0 : ri(11, 100),
      mode_label: pick(['Manual', 'Schedule', 'PPFD']),
      dark: 0,
      offT: 0,
      ppfd_min: 0,
      ppfd_max: 100,
      sched_start: '00:00',
      sched_end: '00:00',
      sched_bri: Math.random() < 0.5 ? 0 : ri(11, 100),
      fade: 0,
      ppfd_start: '00:00',
      ppfd_end: '00:00',
      ppfd: 0,
      ppfd_fade: 0
    };
  }

  function lightPlanStage(modeType, ppfd) {
    return {
      modeType: modeType,
      darkTemp: 0,
      offTemp: 0,
      timePeriod: [{ enabled: 1, weekmask: 127, startTime: 18000, endTime: 82800, brightness: 60, fadeTime: 0 }],
      ppfdPeriod: [{ enabled: 1, weekmask: 127, startTime: 18000, endTime: 82800, brightness: ppfd, fadeTime: 1800 }],
      mOnOff: 0,
      mLevel: 11,
      ppfdMinBrightness: 11,
      ppfdMaxBrightness: 100
    };
  }

  function planStage(stageId, label, color, temp, humi, co2, ppfd) {
    return {
      stageId: stageId,
      label: label,
      startDate: 132779020,
      endDate: 132779022,
      alarmDate: 0,
      color: color,
      light1: lightPlanStage(12, ppfd),
      light2: lightPlanStage(12, ppfd),
      target: {
        dayTime: { startTime: 18000, endTime: 82800 },
        temp: { targetDay: temp, targetNight: temp, deadband: 3 },
        humi: { targetDay: humi, targetNight: humi, deadband: 5 },
        co2: { targetDay: co2, targetNight: co2, deadband: 200 }
      }
    };
  }

  function buildDevice() {
    return {
      mac: DEV_MAC_FLAT,
      slug: 'demo_tent',
      name: 'Demo Tent',
      def_name: 'GGS ' + DEV_MAC_FLAT,
      def_slug: 'ggs_' + DEV_MAC_FLAT.toLowerCase(),
      online: true,
      fan: fanBlock(true),
      blower: fanBlock(false),
      light: lightBlock(),
      light2: lightBlock(),
      climate: [],
      target: {
        dayTime: { startTime: 28800, endTime: 72000 },
        temp: { targetDay: 24, targetNight: 22, deadband: 3 },
        humi: { targetDay: 65, targetNight: 65, deadband: 10 },
        co2: { targetDay: 800, targetNight: 800, deadband: 100 }
      },
      has_plan: true,
      lplan_active: false,
      today: 132778505,
      cal: { temp: 0, humi: 0, co2: 0, ppfd: 0 },
      clean: { on: false, known: false, phase: 0, remain: 0 },
      alarm: {
        temp: { enabled: 0, vmin: 16, vmax: 32 },
        humi: { enabled: 0, vmin: 40, vmax: 90 },
        co2: { enabled: 0, vmin: 350, vmax: 1450 },
        tempSoil: { vmin: 10, vmax: 30 },
        humiSoil: { vmin: 15, vmax: 90 },
        ECSoil: { vmin: 1, vmax: 4.5 },
        vpd: { enabled: 0, vmin: 0.4, vmax: 1.7 },
        ppfd: { enabled: 0, vmax: 4000 },
        devOffline: 0,
        dehumiWaterFull: 0,
        lightTemp: 0,
        humiWaterLess: 0,
        waterLess: 0,
        waterLeak: 0
      },
      alarm_spec: {
        r: [
          { k: 'temp', l: 'Air temperature', u: '°C', mn: true, nlo: 0, nhi: 30, xlo: 18, xhi: 50, st: 1 },
          { k: 'humi', l: 'Air humidity', u: '%', mn: true, nlo: 0, nhi: 80, xlo: 50, xhi: 100, st: 1 },
          { k: 'vpd', l: 'VPD', u: 'kPa', mn: true, nlo: 0, nhi: 1.6, xlo: 0.5, xhi: 4, st: 0.1 },
          { k: 'co2', l: 'CO₂', u: 'ppm', mn: true, nlo: 200, nhi: 1500, xlo: 450, xhi: 5000, st: 10 },
          { k: 'ppfd', l: 'PPFD', u: 'µmol/m²/s', mn: false, nlo: 0, nhi: 3900, xlo: 100, xhi: 4000, st: 10 },
          { k: 'tempSoil', l: 'Substrate temperature', u: '°C', mn: true, nlo: 0, nhi: 26, xlo: 13, xhi: 50, st: 1 },
          { k: 'humiSoil', l: 'Substrate moisture', u: '%', mn: true, nlo: 0, nhi: 80, xlo: 25, xhi: 100, st: 1 },
          { k: 'ECSoil', l: 'Substrate EC', u: 'mS/cm', mn: true, nlo: 0, nhi: 4.3, xlo: 1.2, xhi: 20, st: 0.1 }
        ],
        s: [
          { k: 'devOffline', l: 'Sensor offline alarm' },
          { k: 'lightTemp', l: 'Light over-temperature alarm' },
          { k: 'dehumiWaterFull', l: 'Dehumidifier water tank full' },
          { k: 'humiWaterLess', l: 'Humidifier water low' },
          { k: 'waterLeak', l: 'Water leak' },
          { k: 'waterLess', l: 'Water shortage' }
        ]
      },
      tz_name: 'Europe/Berlin',
      tz_posix: 'CET-1CEST,M3.5.0,M10.5.0/3',
      sys: {
        ver: '3.20',
        hwcode: 1,
        hwver: '2.7',
        buildTime: 'Jun  9 2026 14:06:22',
        timezone: 'Europe/Berlin',
        TZ: 'CET-1CEST,M3.5.0,M10.5.0/3',
        gmtoff: 0,
        upCount: 35,
        mem: 1130,
        tzoff: 3600,
        verUpdateNum: 43,
        verUpdateTime: 1791048156,
        verUpdateWho: 'local',
        eth: { isConnect: 0 },
        bluetooth: { isConnect: 0 },
        wifi: { isConnect: 1, rssi: -ri(45, 70) },
        localtime: new Date().toISOString().slice(0, 19).replace('T', ' ') + ' Friday CEST',
        upTime: ri(100000, 184488),
        UTC: Math.floor(Date.now() / 1000),
        mqtt: { isConnect: 1, connectTime: ri(1000, 14357) }
      },
      sensors: {
        Temperature: (+rnd(21, 28).toFixed(1)).toString(),
        Humidity: (+rnd(50, 75).toFixed(1)).toString(),
        VPD: (+rnd(0.4, 1.7).toFixed(2)).toString()
      },
      outlets: [
        { key: 'outlet_1', label: 'Outlet 1', on: Math.random() < 0.5, idx: 0 },
        { key: 'outlet_2', label: 'Outlet 2', on: Math.random() < 0.5, idx: 1 },
        { key: 'outlet_3', label: 'Outlet 3', on: Math.random() < 0.5, idx: 2 }
      ],
      plan: {
        stage: [
          planStage(1791450785, 'Sämlingszucht', 1, 23, 70, 600, 300),
          planStage(1791450807, 'Klonen und Sämlingszucht', 2, 26, 85, 600, 200),
          planStage(1791450831, 'Vegetatives Wachstum', 3, 26, 65, 600, 600),
          planStage(1791450851, 'Blütezeit', 4, 25, 55, 1000, 900),
          planStage(1791457102, 'Drying', 5, 18, 50, 400, 20)
        ],
        enabled: 0
      }
    };
  }

  function controlData() {
    var suffix = Math.floor(rnd(0x10000, 0xfffff)).toString(16).toUpperCase();
    return {
      ver: 2250,
      plan_max: 5,
      session: true,
      tz_push: true,
      bridge_tz: 'Europe/Berlin',
      bridge_name: 'SpiderBridge ' + suffix,
      bridge_def: 'SpiderBridge ' + suffix,
      bridge_is_def: true,
      devices: [buildDevice()]
    };
  }

  // ----------------------------------------------------------------- network
  function networkData() {
    return {
      clients: [{
        mac: DEV_MAC,
        rssi: -ri(17, 60),
        listed: false,
        ip: CLIENT_IP,
        name: ''
      }],
      mac_mode: 0,
      mac_list: [],
      ip_mode: 0,
      ip_list: []
    };
  }

  // --------------------------------------------------------------------- ble
  function bleData() {
    return {
      running: false,
      scanned: true,
      busy: '',
      result: 'Scan finished: 1 GGS controller(s) found',
      trace: 'Bluetooth boot: scan ',
      ap_ssid: 'SpiderBridge',
      dev: [{
        addr: DEV_ADDR,
        name: 'SF-GGS-CB',
        rssi: -ri(32, 60),
        known_as: 'Demo Tent',
        session: true,
        wifi_mac: DEV_MAC_FLAT,
        pcode: 1004,
        adv_active: true,
        adv_wifi: true,
        adv_cloud: true,
        connected: true,
        hs: 'ip',
        ip: CLIENT_IP
      }]
    };
  }

  // ------------------------------------------------------------------- zones
  var ZONES = ["Africa/Abidjan","Africa/Algiers","Africa/Blantyre","Africa/Cairo","Africa/Casablanca","Africa/Johannesburg","Africa/Lagos","Africa/Nairobi","America/Adak","America/Anchorage","America/Anguilla","America/Asuncion","America/Atikokan","America/Belize","America/Boa_Vista","America/Bogota","America/Chicago","America/Chihuahua","America/Denver","America/Godthab","America/Halifax","America/Havana","America/Lima","America/Los_Angeles","America/Mexico_City","America/Miquelon","America/New_York","America/Noronha","America/Phoenix","America/Santiago","America/Sao_Paulo","America/Scoresbysund","America/St_Johns","America/Toronto","America/Vancouver","Antarctica/Casey","Antarctica/DumontDUrville","Antarctica/Mawson","Antarctica/Troll","Antarctica/Vostok","Asia/Amman","Asia/Anadyr","Asia/Bangkok","Asia/Beirut","Asia/Chita","Asia/Colombo","Asia/Damascus","Asia/Dubai","Asia/Gaza","Asia/Hong_Kong","Asia/Jakarta","Asia/Jayapura","Asia/Jerusalem","Asia/Kabul","Asia/Karachi","Asia/Kathmandu","Asia/Kolkata","Asia/Makassar","Asia/Manila","Asia/Seoul","Asia/Shanghai","Asia/Singapore","Asia/Taipei","Asia/Tehran","Asia/Tokyo","Asia/Yangon","Atlantic/Cape_Verde","Australia/Adelaide","Australia/Brisbane","Australia/Darwin","Australia/Eucla","Australia/Lord_Howe","Australia/Melbourne","Australia/Perth","Australia/Sydney","Etc/GMT+10","Etc/GMT+11","Etc/GMT+12","Etc/GMT+6","Etc/GMT+7","Etc/GMT+8","Etc/GMT+9","Etc/GMT-13","Etc/GMT-14","Etc/GMT-2","Europe/Amsterdam","Europe/Athens","Europe/Berlin","Europe/Brussels","Europe/Bucharest","Europe/Budapest","Europe/Chisinau","Europe/Copenhagen","Europe/Dublin","Europe/Helsinki","Europe/Istanbul","Europe/Kiev","Europe/Lisbon","Europe/London","Europe/Madrid","Europe/Moscow","Europe/Oslo","Europe/Paris","Europe/Prague","Europe/Rome","Europe/Sofia","Europe/Stockholm","Europe/Vienna","Europe/Warsaw","Europe/Zurich","Pacific/Auckland","Pacific/Chatham","Pacific/Easter","Pacific/Fiji","Pacific/Guam","Pacific/Honolulu","Pacific/Marquesas","Pacific/Midway","Pacific/Norfolk","UTC"];

  // --------------------------------------------------------------- templates
  function tplLight(ppfd) {
    return {
      modeType: 12,
      darkTemp: 0,
      offTemp: 0,
      timePeriod: [{ enabled: 1, weekmask: 127 }],
      ppfdPeriod: [{ enabled: 1, weekmask: 127, startTime: 18000, endTime: 82800, brightness: ppfd, fadeTime: 1800 }],
      mOnOff: 0,
      mLevel: 11,
      ppfdMinBrightness: 11,
      ppfdMaxBrightness: 100
    };
  }

  function template(id, name, days, color, temp, humi, co2, ppfd) {
    return {
      id: id,
      tpl: {
        name: name,
        days: days,
        color: color,
        light1: tplLight(ppfd),
        light2: tplLight(ppfd),
        target: {
          dayTime: { startTime: 18000, endTime: 82800 },
          temp: { targetDay: temp, targetNight: temp, deadband: 3 },
          humi: { targetDay: humi, targetNight: humi, deadband: 5 },
          co2: { targetDay: co2, targetNight: co2, deadband: 200 }
        }
      }
    };
  }

  function templates() {
    return {
      system: [
        template('sys:seedling', 'Seedling', 14, 1, 23, 70, 600, 300),
        template('sys:clone', 'Cloning & Seedling', 14, 2, 26, 85, 600, 200),
        template('sys:veg', 'Vegetative Growth', 28, 3, 26, 65, 600, 600),
        template('sys:flower', 'Flowering', 56, 4, 25, 55, 1000, 900),
        template('sys:dry', 'Drying', 10, 5, 18, 50, 400, 20)
      ],
      custom: [],
      max: 40,
      free: 'cust:0'
    };
  }

  function lplanData() {
    var d = buildDevice();
    return {
      active: false,
      running: false,
      count: d.plan.stage.length,
      window: 0,
      max: 27,
      from: 0,
      last_end: 132841730,
      stage: d.plan.stage
    };
  }

  // The detailed log of the last Bluetooth job is plain text, not JSON.
  function bleLogText() {
    return [
      'Bluetooth boot: scan start',
      'ggs_ble: scanning for GGS controllers (5 s) ...',
      'ggs_ble: found ' + DEV_ADDR + '  name=SF-GGS-CB  rssi=-' + ri(32, 60) + '  pcode=1004 flags=0x7',
      'ggs_ble: connecting to ' + DEV_ADDR + ' ...',
      'ggs_ble: MTU 247, bonded=yes',
      'ggs_ble: session key established',
      'ggs_ble: > {"cmd":1,"ssid":"SpiderBridge","pwd":"*****","pcode":1004}',
      'ggs_ble: frame 131 bytes written',
      'ggs_ble: < {"result":0,"msg":"ok"}',
      'ggs_ble: reply decrypted, command accepted',
      'ggs_ble: disconnecting, result stored',
      'Bluetooth job done: 1 controller(s) provisioned'
    ].join('\n') + '\n';
  }

  var GETTERS = {
    '/status/data': function () { return statusData(); },
    '/syslog/data': function () { return { lines: syslogLines(), dropped: 0 }; },
    '/control/data': function () { return controlData(); },
    '/control/zones': function () { return ZONES; },
    '/control/templates': function () { return templates(); },
    '/control/ver': function () { return { v: 2250 }; },
    '/control/lplan': function () { return lplanData(); },
    '/control/zones/list': function () { return ZONES; },
    '/network/data': function () { return networkData(); },
    '/ble/data': function () { return bleData(); },
    '/log/data': function () { return { e: logEvents(), n: logN + 1 }; },
    '/wifi/scan': function () {
      return Array.from({ length: 6 }, function () {
        var ssid = pick(['MyHomeWiFi', 'FRITZ!Box 7590', 'RepeaterEG', 'IoT', 'GGS-Net']);
        return { ssid: ssid, rssi: -ri(35, 89), ch: ri(1, 11), open: ssid === 'IoT' };
      });
    },
    '/wifi/status': function () {
      return { running: false, ok: true, msg: 'Connected to "' + lastSsid + '" -- saved' };
    }
  };

  // JSON endpoints that only acknowledge a POST.
  var JSON_POSTS = [
    '/save', '/control/set', '/control/device', '/control/plan', '/control/template',
    '/network/act', '/status/dst', '/log/send'
  ];

  // Endpoints that answer with plain text on the real bridge.
  var TEXT_POSTS = {
    '/ble/act': function () { return 'Restarting into Bluetooth to scan -- back in about 30 seconds'; },
    '/reboot': function () { return 'Restarting -- back in about 15 seconds'; },
    '/system/reset': function () { return 'Factory reset started -- the bridge comes back on its own hotspot'; },
    '/restore': function () { return 'Backup applied -- restarting'; },
    '/update/check': function () { return 'Checking for a newer version...'; },
    '/update/remote': function () { return 'Downloading and installing...'; }
  };

  function json(data) {
    return new Response(JSON.stringify(data), {
      status: 200,
      headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' }
    });
  }

  function text(body, status) {
    return new Response(body, {
      status: status || 200,
      headers: { 'Content-Type': 'text/plain; charset=utf-8', 'Cache-Control': 'no-store' }
    });
  }

  function delayed(data, ms) {
    return new Promise(function (res) {
      setTimeout(function () { res(json(data)); }, ms);
    });
  }

  function delayedText(body, ms, status) {
    return new Promise(function (res) {
      setTimeout(function () { res(text(body, status)); }, ms);
    });
  }

  function toast(msg) {
    var el = document.createElement('div');
    el.textContent = msg;
    el.style.cssText = 'position:fixed;left:50%;bottom:1.2rem;transform:translateX(-50%);' +
      'background:#8ab4f8;color:#13151a;font:600 .82rem system-ui;padding:.55rem .9rem;' +
      'border-radius:.4rem;box-shadow:0 4px 18px rgba(0,0,0,.4);z-index:9999;max-width:90vw';
    document.body.appendChild(el);
    setTimeout(function () { el.remove(); }, 3200);
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
      if (path === '/ble/log') {
        return delayedText(bleLogText(), 80);
      }
      var getter = GETTERS[path];
      if (getter) {
        return delayed(getter(), path === '/ble/data' ? 150 : 60);
      }
    } else {
      if (path === '/wifi/connect') {
        var ssid = lastSsid;
        try {
          var q = new URLSearchParams((init && init.body) || '');
          if (q.get('ssid')) ssid = q.get('ssid');
        } catch (e) {}
        lastSsid = ssid;
        return delayedText('Connecting to "' + ssid + '" ...', 300, 202);
      }
      var tp = TEXT_POSTS[path];
      if (tp) {
        return delayedText(tp(), path === '/ble/act' ? 400 : 150, 200);
      }
      if (JSON_POSTS.indexOf(path) !== -1) {
        return delayed({ ok: true }, 120);
      }
    }

    return realFetch(input, init);
  };

  // -------------------------------------------------------------------------
  // Behaviour the real page gets from plain HTML: the Settings form is a
  // native POST, the Firmware upload uses XMLHttpRequest, and the backup and
  // Bluetooth-log links are plain navigations. None of those go through
  // fetch(), so they are handled here to keep the demo working.

  document.addEventListener('submit', function (e) {
    var f = e.target;
    if (!f || !f.getAttribute) return;
    var path;
    try { path = new URL(f.getAttribute('action') || '', location.href).pathname; } catch (err) { return; }
    if (path === '/save') {
      e.preventDefault();
      toast('Demo: settings are simulated \u2014 nothing is stored.');
    }
  }, true);

  document.addEventListener('click', function (e) {
    var a = e.target && e.target.closest ? e.target.closest('a') : null;
    if (!a) return;
    var path;
    try { path = new URL(a.href, location.href).pathname; } catch (err) { return; }
    if (path === '/backup') {
      e.preventDefault();
      fetch('/backup').then(function (r) { return r.blob(); }).then(function (b) {
        var u = URL.createObjectURL(b), x = document.createElement('a');
        x.href = u; x.download = 'spiderbridge-backup-demo.json';
        document.body.appendChild(x); x.click(); x.remove(); URL.revokeObjectURL(u);
      });
    } else if (path === '/ble/log') {
      e.preventDefault();
      fetch('/ble/log').then(function (r) { return r.text(); }).then(function (t) {
        var esc = t.replace(/[&<>]/g, function (c) { return { '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]; });
        var w = window.open('', '_blank');
        if (w) {
          w.document.write('<title>Bluetooth job log</title>' +
            '<pre style="font:12px monospace;white-space:pre-wrap;padding:1rem">' + esc + '</pre>');
        }
      });
    }
  }, true);

  // The Firmware page uploads with XMLHttpRequest; answer that one too.
  var RealXHR = window.XMLHttpRequest;
  window.XMLHttpRequest = function () {
    var xhr = new RealXHR();
    var realOpen = xhr.open, realSend = xhr.send, fake = false;
    xhr.open = function (method, url) {
      try {
        fake = String(method).toUpperCase() === 'POST' &&
               new URL(url, location.href).pathname === '/update';
      } catch (e) { fake = false; }
      if (!fake) realOpen.apply(xhr, arguments);
    };
    xhr.send = function (body) {
      if (!fake) { realSend.apply(xhr, arguments); return; }
      setTimeout(function () {
        try {
          if (xhr.upload && xhr.upload.onprogress) {
            xhr.upload.onprogress({ lengthComputable: true, loaded: 100, total: 100 });
          }
        } catch (e) {}
        try { Object.defineProperty(xhr, 'status', { value: 200 }); } catch (e) {}
        try { Object.defineProperty(xhr, 'responseText', { value: 'demo' }); } catch (e) {}
        if (xhr.onload) xhr.onload();
      }, 500);
    };
    return xhr;
  };

  // Randomize the Settings form on every load, so the demo never shows the
  // same home network twice. The committed page carries neutral values
  // already; this is the "randomized" part.
  function randomizeSettings() {
    var f = document.querySelector('form[action="/save"]');
    if (!f) return;
    function set(n, v) {
      var el = f.querySelector('[name="' + n + '"]');
      if (el && el.type !== 'hidden' && el.tagName !== 'SELECT') el.value = v;
    }
    set('sta_ssid', pick(['MyHomeWiFi', 'HomeNet-2.4G', 'FRITZ!Box 7590', 'GrowRoom', 'IoT-VLAN']));
    set('static_ip', '192.168.1.' + ri(20, 200));
    set('static_gw', '192.168.1.1');
    set('ha_uri', 'mqtt://192.168.1.' + ri(2, 20) + ':1883');
    set('ha_user', pick(['homeassistant', 'mqtt-user', 'openhab']));
  }
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', randomizeSettings);
  } else {
    randomizeSettings();
  }

  window.spiderbridgeDemo = { calls: function () { return CALLS; } };
})();
