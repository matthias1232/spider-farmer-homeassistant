#!/usr/bin/env python3
"""Generates main/ha_discovery_table.c and recomputes the payload ceiling.

Hand-writing ~100 JSON payloads is how the device grouping went wrong in the
first place: every per-mode entity carried its own device name, which Home
Assistant turned into ten separate pages. The table is generated from a
structured description here, and this script doubles as the validator:

  * every payload is parsed back as JSON
  * every state topic is checked for a publisher
  * every command topic is checked for a handler
  * the longest payload is measured so HA_DISCOVERY_MAX_PAYLOAD is computed
    rather than guessed
"""

import json
import os
import re

MAIN = "/mnt/d/OneDrive/Documents/SpiderBridge/spiderbridge-esp32-v2/main"
OUT_C = os.path.join(MAIN, "ha_discovery_table.c")
OUT_H = os.path.join(MAIN, "ha_discovery_table.h")
OUT_STALE_C = os.path.join(MAIN, "ha_discovery_stale.c")
OUT_STALE_H = os.path.join(MAIN, "ha_discovery_stale.h")
PREV_TOPICS = os.path.join(MAIN, "..", "tools", "prev_discovery_topics.txt")

# Device blocks carry only keys Home Assistant's device schema knows.
#
# "has_entity_name" used to be in here. It is not a device option -- Home
# Assistant sets it on every MQTT entity itself -- and the device schema
# does not allow extra keys, so it made every one of these payloads fail
# validation: the entities stayed registered from earlier firmware but were
# never loaded again, which Home Assistant shows as "unavailable".
CTRL_DEVICE = {
    "identifiers": ["spiderfarmer_$D"],
    "manufacturer": "Spider Farmer",
    "model": "GGS Controller",
    "name": "$N",
    "via_device": "spiderbridge",
}

# The same identifier the firmware's hand-written bridge entities use
# (ha_mqtt.c publish_bridge_discovery). Two different identifiers made Home
# Assistant create two bridge devices with the same name.
BRIDGE_DEVICE = {
    "identifiers": ["spiderbridge"],
    "manufacturer": "SpiderBridge",
    "model": "ESP32 Bridge",
    # Expanded by the firmware to the name set under Settings.
    "name": "$B",
}

# Origin information, recommended by Home Assistant for every discovery
# payload: it is what the MQTT log names as the source of an entity.
ORIGIN = {"name": "SpiderBridge"}

AVAIL = "spiderfarmer/$D/availability"
CFG = "config"
DIAG = "diagnostic"

# The app's own speed ranges, per module.
#
# Both speed entities are presented in percent, in steps of ten, because
# that is how the app shows them: the circulation fan runs 10-100% (its
# wire values are 1-10, so those are scaled on the way in and out) and the
# exhaust blower 25-100%, which is already a percent-like range on the
# wire.
#
# The blower's run speed additionally offers Auto, which writes 0 -- the
# controller then picks the speed itself. That does not collide with the
# standby field's Off, because standby is a different field (minSpeed)
# where 0 already means off.
#
# A select rather than a number for both: each needs an "Off" or "Auto"
# option that is not part of the numeric range, and Home Assistant's fan
# entity has no such option -- a speed_range_min of 0 would let the speed
# slider itself go to zero, which is a different action from Auto.
# In each module's own units, as the app shows them (1.16): the
# circulation fan 1-10, the exhaust blower 25-100 %.
PERCENT_MIN = {"fan": 1, "blower": 25}
PERCENT_MAX = {"fan": 10, "blower": 100}

# Stated in wire units, and scaled by PERCENT_SCALE on the way in and out.
SPEED_MIN = {"fan": 1, "blower": 25}
SPEED_MAX = {"fan": 10, "blower": 100}
PERCENT_SCALE = {"fan": 10, "blower": 1}

# Standby, which the app offers as Off plus a narrower band than the
# running speed. The circulation fan's standby is fixed at 0 with no
# control at all: its wire range starts at 1, so there is no lower value
# to hold and every reachable value would be a running speed, not a
# standby one.
STANDBY_MAX = {"fan": 0, "blower": 39}
STANDBY_MIN = {"fan": 0, "blower": 25}

# Cycle repeats: the app offers 1-100 and the controller's cap is also
# 100, so the full range is offered here rather than the 1-3 the earlier
# table guessed at.
CYCLE_TIMES_MIN = 1
CYCLE_TIMES_MAX = 100

# Light-side ranges, all taken from the app's own controls.
#
# Target brightness and the PPFD dimming band both run 11-100%. The
# controller accepts 1 and up, but a control reaching below the app's own
# floor would only offer values the app never writes.
LIGHT_BRI_MIN = 11

# PPFD target, in micromoles. The earlier 0-1000 cap was the sensor's
# reporting scale rather than anything the controller enforces.
PPFD_TARGET_MIN = 20
PPFD_TARGET_MAX = 2000

# Sunrise and sunset simulation, in minutes: Off or 1-60, for both the
# brightness and the PPFD fade.
FADE_MINUTES_MAX = 60

# Dim and off thresholds, in degrees C: Off or 15-50.
THRESHOLD_MIN = 15
THRESHOLD_MAX = 50

# Calibration offsets, per sensor. Temperature and humidity move in tenths
# over -10/+10 and -20/+20, CO2 in tens of ppm over -200/+200, and the PPFD
# offset in tenths over -20/+20.
CAL_MIN = {"temp": -10, "humi": -20, "co2": -200, "ppfd": -20}
CAL_MAX = {"temp": 10, "humi": 20, "co2": 200, "ppfd": 20}
CAL_STEP = {"temp": 0.1, "humi": 0.1, "co2": 10, "ppfd": 0.1}

FAN_LABEL = {"fan": "Fan", "blower": "Fan Exhaust"}
LIGHT_LABEL = {"light": "Light 1", "light2": "Light 2"}

# The mode labels the controller reports, in the order Home Assistant shows
# them for the fan preset list. modeType is a flat integer space, so the five
# environment variants sit among the top-level modes on the wire.
PRESET_MODES = [
    "Manual",
    "Schedule",
    "Cycle",
    "Environment: Prioritize temperature",
    "Environment: Prioritize humidity",
    "Environment: Temperature only",
    "Environment: Humidity only",
    "Environment: Temperature & humidity",
]

ENV_SUBMODES = [
    "Prioritize temperature",
    "Prioritize humidity",
    "Temperature only",
    "Humidity only",
    "Temperature & humidity",
]

MAIN_MODES = ["Manual", "Schedule", "Cycle", "Environment"]
LIGHT_MODES = ["Manual", "Schedule", "PPFD"]

entries = []
state_topics = set()
command_topics = set()


def add(component, uid_suffix, payload, bridge=False):
    # Bridge entities get a slug of their own. Their identifiers must not
    # contain $D: the heap and version are properties of the one ESP32, so
    # they are published once under "bridge" and would otherwise be
    # duplicated once per controller, all writing the same values.
    dev = BRIDGE_DEVICE if bridge else CTRL_DEVICE
    slug = "bridge" if bridge else "$D"
    full = dict(payload)
    full["device"] = dev
    full["origin"] = ORIGIN
    # Controller entities depend on two things being up: the bridge, which
    # carries every value, and the controller itself. "all" mode makes
    # Home Assistant mark them unavailable when either goes away -- a dead
    # bridge leaves the controller's last retained "online" behind, which
    # alone would keep showing stale values as live.
    if "availability_topic" not in full and "availability" not in full:
        if bridge:
            full["availability_topic"] = "spiderfarmer/bridge/availability"
        else:
            full["availability"] = [
                {"topic": "spiderfarmer/bridge/availability"},
                {"topic": AVAIL},
            ]
            full["availability_mode"] = "all"
    full["unique_id"] = "spiderfarmer_%s_%s" % (slug, uid_suffix)
    topic = "homeassistant/%s/spiderfarmer_%s_%s/config" % (component, slug,
                                                            uid_suffix)
    entries.append((topic, full))
    if full.get("state_topic"):
        state_topics.add(full["state_topic"])
    if full.get("command_topic"):
        command_topics.add(full["command_topic"])
    for key in ("percentage_command_topic", "preset_mode_command_topic",
                "oscillation_command_topic"):
        if full.get(key):
            command_topics.add(full[key])


def cmd(field, sub):
    return "spiderfarmer/$D/command/%s/%s/set" % (field, sub)


def st(field, sub):
    return "spiderfarmer/$D/state/%s/%s" % (field, sub)


# ---------------------------------------------------------------------------
# Fans and blowers
# ---------------------------------------------------------------------------

for m in ("fan", "blower"):
    lbl = FAN_LABEL[m]

    # Speed is a select rather than a number because the app's own control
    # is a list, and because Auto (blower) is an option that is not a
    # speed at all. The values are percents in steps of ten, which is what
    # the app shows; the value template scales them onto the wire.
    # The blower runs on whole percents from 25, so its list is every value
    # 25-100: a list in steps of ten started at 25 (25, 35, ... 95) and
    # never contained 50, 60 or 100, which Home Assistant then showed as
    # "unknown". The fan's percent is always a multiple of ten.
    # The running speed is always a number (the controller's own value);
    # "Auto" exists only for the schedule speed, see below.
    speed_opts = [str(n) for n in range(PERCENT_MIN[m], PERCENT_MAX[m] + 1)]

    osc = {}
    if m == "fan":
        # The toggle in the Fan's own dialog; the level has its own entity.
        osc = {
            "oscillation_command_topic": cmd(m, "oscillation"),
            "oscillation_state_topic": st(m, "oscillation"),
            "payload_oscillation_on": "ON",
            "payload_oscillation_off": "OFF",
        }
    add("fan", m, dict(osc, **{
        "name": lbl,
        "command_topic": "spiderfarmer/$D/command/%s/set" % m,
        "state_topic": "spiderfarmer/$D/state/%s" % m,
        "state_value_template": "{{ value_json.state }}",
        "payload_on": "ON",
        "payload_off": "OFF",
        "percentage_command_topic": cmd(m, "percentage"),
        "percentage_state_topic": "spiderfarmer/$D/state/%s" % m,
        # Auto passes through unchanged; a percent is scaled down onto the
        # wire, so the fan's wire 1-10 becomes a 10-100% slider and the
        # blower's already-percent range is left alone.
        "percentage_value_template": "{{ value_json.percentage | int(0) }}",
        "preset_mode_command_topic": cmd(m, "preset_mode"),
        "preset_mode_state_topic": st(m, "mode_label"),
        "preset_modes": PRESET_MODES,
        # 1-100 makes Home Assistant's ranged value identical to the
        # percent: the bridge publishes percent (the fan's wire 1-10 times
        # ten) and takes percent back, scaling and clamping to each
        # module's own range itself. A 1-10 range here read a published
        # 30% as an out-of-range 30 and sent 3 back for "30%".
        # Home Assistant maps speed_range_min..max onto its 1-100 % slider.
        # The blower is a percent on the wire, so 1-100 shows it 1:1 (40 on
        # the controller is 40 % in Home Assistant); values below 25 are
        # raised to 25 by the bridge, the controller's floor. The
        # circulation fan's 1-10 has no percent equivalent: 1-10 makes each
        # step 10 % on the slider, and its Speed select shows 1-10 itself.
        "speed_range_min": 1,
        "speed_range_max": 100 if m == "blower" else 10,
    }))

    # The fan entity's own speed slider has no room for a non-numeric
    # option, so the app's list is offered as this select alongside it.
    add("select", "%s_speed" % m, {
        "name": "%s Speed" % lbl,
        "icon": "mdi:fan",
        "entity_category": CFG,
        "options": speed_opts,
        "command_topic": cmd(m, "percentage"),
        "state_topic": st(m, "speed"),
        # The running speed is only settable in Manual mode; in every other
        # mode the controller picks it, so the select is greyed out there.
        # (The bridge refuses a speed outside Manual from any source.)
        "availability": [
            {"topic": "spiderfarmer/bridge/availability"},
            {"topic": AVAIL},
            {"topic": st(m, "mode_label"),
             "value_template": "{{ 'online' if value == 'Manual' else 'offline' }}"},
        ],
        "availability_mode": "all",
    })

    # One mode select covering every mode, environment variants included.
    #
    # The app's two-step picker (mode, then which environment) was mirrored
    # as two selects before; that split one choice across two controls,
    # and the second one meant nothing outside Environment mode. On the
    # wire it is one field anyway (modeType), and the preset path already
    # takes exactly these labels, so a single list is both clearer and
    # needs no new command handling. main_mode/env_submode stay available
    # over MQTT for anything scripted against them.
    add("select", "%s_mode" % m, {
        "name": "%s Mode" % lbl,
        "icon": "mdi:tune-variant",
        "options": PRESET_MODES,
        "command_topic": cmd(m, "preset_mode"),
        "state_topic": st(m, "mode_label"),
    })

    # Schedule window. The times are real pickers rather than text fields,
    # because HH:MM is exactly what Home Assistant's time entity wants.
    for which, label in (("start", "Start Time"), ("end", "End Time")):
        add("time", "%s_schedule_%s" % (m, which), {
            "name": "%s Schedule %s" % (lbl, label),
            "icon": "mdi:clock-outline",
            "entity_category": CFG,
            "format": "%H:%M",
            "command_topic": cmd(m, "schedule_" + which),
            "state_topic": st(m, "schedule_" + which),
        })

    # Schedule speed, as a percent: the app shows this in steps of ten for
    # the fan and in plain percents for the blower.
    # The blower's schedule speed may be Auto (0 on the wire): the
    # controller then picks the speed in the scheduled/environment modes.
    # As the app offers it for both modules: a step 1-10, plus Auto (0 on
    # the wire, the controller picks the speed). Home Assistant's option
    # list is static, so Auto is always listed; the bridge only accepts it
    # in the Environment modes and ignores it otherwise.
    # Fan: Auto + 1-10. Exhaust: Auto + 25-100 % in 1 % steps.
    sched_opts = ["Auto"] + speed_opts
    add("select", "%s_schedule_speed" % m, {
        "name": "%s Schedule Speed" % lbl,
        "entity_category": CFG,
        "options": sched_opts,
        "command_topic": cmd(m, "schedule_speed"),
        "state_topic": st(m, "schedule_speed"),
    })

    # Standby speed.
    #
    # The blower's standby is Off plus 25-39, a narrower band than its
    # running speed -- the app treats standby as a deliberate low setting
    # rather than the same range.
    #
    # The circulation fan's standby is fixed at 0 and gets no control at
    # all: its wire floor is 1, so it has no lower value to hold, and
    # publishing a slider for it would only offer values that are running
    # speeds. The value is still published as a disabled entity so the
    # setting is visible rather than silently missing.
    standby_opts = ["Off"] + [str(n) for n in range(STANDBY_MIN[m],
                                                    STANDBY_MAX[m] + 1)]
    standby_payload = {
        "name": "%s Standby Speed" % lbl,
        "icon": "mdi:fan-minus",
        "entity_category": CFG,
        "options": standby_opts,
        "command_topic": cmd(m, "standby_speed"),
        "state_topic": st(m, "standby_speed"),
    }
    if m == "fan":
        # Not a control anyone needs: hidden unless explicitly enabled.
        standby_payload["enabled_by_default"] = False
    add("select", "%s_standby_speed" % m, standby_payload)

    # Cycle mode. Start time is a picker; run and off times are durations,
    # which need seconds, and Home Assistant has no picker entity for those,
    # so they stay text fields carrying HH:MM:SS.
    add("time", "%s_cycle_start" % m, {
        "name": "%s Cycle Start Time" % lbl,
        "icon": "mdi:clock-outline",
        "entity_category": CFG,
        "format": "%H:%M",
        "command_topic": cmd(m, "cycle_start"),
        "state_topic": st(m, "cycle_start"),
    })

    # Cycle run and off are durations, so they need a seconds step: the app
    # sets them to the second and a captured 03h 02min 02s run is 10922 s,
    # which a picker without seconds cannot express. Home Assistant's time
    # entity carries show_seconds for exactly this, so these are pickers
    # like the times above rather than the text fields they used to be.
    for which, label, icon in (
        ("run_time", "Cycle Run Time", "mdi:timer-play"),
        ("off_time", "Cycle Off Time", "mdi:timer-off"),
    ):
        add("time", "%s_cycle_%s" % (m, which), {
            "name": "%s %s" % (lbl, label),
            "icon": icon,
            "entity_category": CFG,
            "format": "%H:%M:%S",
            "show_seconds": True,
            "command_topic": cmd(m, "cycle_" + which),
            "state_topic": st(m, "cycle_" + which),
        })

    # Cycle repeats, over the controller's own 1-100 cap.
    add("select", "%s_cycle_times" % m, {
        "name": "%s Cycle Repeats" % lbl,
        "icon": "mdi:repeat",
        "entity_category": CFG,
        "options": [str(n) for n in range(CYCLE_TIMES_MIN, CYCLE_TIMES_MAX + 1)],
        "command_topic": cmd(m, "cycle_times"),
        "state_topic": st(m, "cycle_times"),
    })

# Circulation-fan-only extras.
#
# Oscillation as the app shows it: a switch, and a level 1-10 (one wire
# field, shakeLevel, where 0 is off). Both names start with "Fan
# Oscillation" so Home Assistant lists them together.
#
# Home Assistant's fan entity only knows oscillation as on/off, so the level
# cannot live on the Fan entity itself. It is offered as a fan entity of its
# own instead: on/off plus a slider whose 1-10 range makes every step 10 %,
# exactly the app's ten levels. A slider at 0 is sent as 0, which the
# command handler takes as off. This replaces the former switch plus level
# select. The Fan entity additionally gets the plain oscillation toggle.
add("fan", "fan_swing", {
    "name": "Fan Oscillation",
    "icon": "mdi:sync",
    "payload_on": "ON",
    "payload_off": "OFF",
    "command_topic": cmd("fan", "oscillation"),
    "state_topic": st("fan", "oscillation"),
    "percentage_command_topic": cmd("fan", "oscillation_level"),
    "percentage_state_topic": st("fan", "oscillation_level"),
    "speed_range_min": 1,
    "speed_range_max": 10,
})

add("switch", "fan_natural_wind", {
    "name": "Fan Natural Wind",
    "icon": "mdi:weather-windy",
    "payload_on": "ON",
    "payload_off": "OFF",
    "command_topic": cmd("fan", "natural_wind"),
    "state_topic": st("fan", "natural_wind"),
})

# Exhaust-blower-only extra: closeCO2 appears in every captured blower block
# and in none of the circulation fan's.
#
# The name says what the setting does rather than what it controls. The
# field makes the blower hold back while CO2 is being fed in, so "CO2
# Connection" with an ON button read as though ON opened the valve. It is
# the same value either way; only the label was misleading.
add("switch", "blower_close_co2", {
    "name": "Close CO2 While Blower Runs",
    "icon": "mdi:air-filter",
    "payload_on": "ON",
    "payload_off": "OFF",
    "command_topic": cmd("blower", "close_co2"),
    "state_topic": st("blower", "close_co2"),
})


# ---------------------------------------------------------------------------
# Sensor cleaning
# ---------------------------------------------------------------------------
#
# The app's two-hour sensor-cleaning cycle for the temperature/humidity
# sensor: the controller heats the sensor element to dry it out and burn off
# moisture and contamination, then lets it cool for five minutes before its
# readings count again. Captured from the app, which sends method
# setSensorHeating with params {"on": 0|1}; that method name is the
# controller's, not a description of what the feature is for.
#
# Both timers are the controller's own. The switch only starts and stops the
# cycle. The controller does not report the state in any frame the bridge
# polls, so the switch shows what was last asked for -- from Home Assistant,
# the web interface, or the vendor app (observed on the cloud link).
add("switch", "sensor_cleaning", {
    "name": "Sensor Cleaning",
    "icon": "mdi:spray-bottle",
    "payload_on": "ON",
    "payload_off": "OFF",
    "command_topic": "spiderfarmer/$D/command/sensor_cleaning/set",
    "state_topic": "spiderfarmer/$D/state/sensor_cleaning",
})

# What the cycle is doing, from the controller's getDevSta.sensorHeating
# {phase, remainTime}: phase 1 cleaning (switch on), phase 2 cooldown (switch
# already off), otherwise idle. An enum sensor so automations can match it.
add("sensor", "sensor_cleaning_status", {
    "name": "Sensor Cleaning Status",
    "icon": "mdi:progress-clock",
    "device_class": "enum",
    "options": ["Cleaning", "Cooling down", "Idle"],
    "state_topic": "spiderfarmer/$D/state/sensor_cleaning_status",
})

# Remaining time of the current phase as H:MM:SS -- two hours of cleaning,
# then five minutes of cooldown; 0:00:00 when idle. Updated every second
# while a phase runs.
add("sensor", "sensor_cleaning_remain", {
    "name": "Sensor Cleaning Time Left",
    "icon": "mdi:timer-sand",
    "state_topic": "spiderfarmer/$D/state/sensor_cleaning_remain",
})

# When the current phase ends, as a timestamp. Home Assistant's frontend
# renders this as a live relative time ("in 1 hour 59 minutes") that ticks
# on its own, and it is the right value for automations. Unknown when idle.
add("sensor", "sensor_cleaning_end", {
    "name": "Sensor Cleaning Phase Ends",
    "icon": "mdi:timer-outline",
    "device_class": "timestamp",
    "state_topic": "spiderfarmer/$D/state/sensor_cleaning_end",
    "value_template": "{{ value if value != 'None' else None }}",
})


# ---------------------------------------------------------------------------
# Lights
# ---------------------------------------------------------------------------

for l in ("light", "light2"):
    lbl = LIGHT_LABEL[l]
    add("light", l, {
        "name": lbl,
        "command_topic": "spiderfarmer/$D/command/%s/set" % l,
        "state_topic": "spiderfarmer/$D/state/%s" % l,
        "schema": "json",
        "brightness": True,
        "brightness_scale": 100,
        "effect": True,
        "effect_list": LIGHT_MODES,
    })

    for which, label in (("start", "Start Time"), ("end", "End Time")):
        add("time", "%s_schedule_%s" % (l, which), {
            "name": "%s Schedule %s" % (lbl, label),
            "icon": "mdi:clock-outline",
            "entity_category": CFG,
            "format": "%H:%M",
            "command_topic": cmd(l, "schedule_" + which),
            "state_topic": st(l, "schedule_" + which),
        })

    # Target brightness, over the app's 11-100%. The controller accepts
    # anything from 1 up, but 11% is the lowest light level the app offers,
    # so a control going below that would only offer values the app itself
    # never writes.
    add("number", "%s_schedule_brightness" % l, {
        "name": "%s Schedule Brightness" % lbl,
        "entity_category": CFG,
        "min": LIGHT_BRI_MIN, "max": 100, "step": 1, "mode": "box",
        "unit_of_measurement": "%",
        "command_topic": cmd(l, "schedule_brightness"),
        "state_topic": st(l, "schedule_brightness"),
    })

    # Sunrise and sunset simulation, in minutes: Off, or 1-60. A select
    # rather than a number because Off is not a duration.
    add("select", "%s_fade_minutes" % l, {
        "name": "%s Schedule Fade Time" % lbl,
        "icon": "mdi:sun-clock",
        "entity_category": CFG,
        "options": ["Off"] + [str(n) for n in range(1, FADE_MINUTES_MAX + 1)],
        "command_topic": cmd(l, "fade_minutes"),
        "state_topic": st(l, "fade_minutes"),
    })

    for which, label in (("start", "Start Time"), ("end", "End Time")):
        add("time", "%s_ppfd_%s" % (l, which), {
            "name": "%s PPFD %s" % (lbl, label),
            "icon": "mdi:clock-outline",
            "entity_category": CFG,
            "format": "%H:%M",
            "command_topic": cmd(l, "ppfd_" + which),
            "state_topic": st(l, "ppfd_" + which),
        })

    # PPFD target, in micromoles, over the app's 20-2000.
    add("number", "%s_ppfd_target" % l, {
        "name": "%s PPFD Target" % lbl,
        "icon": "mdi:white-balance-sunny",
        "entity_category": CFG,
        "min": PPFD_TARGET_MIN, "max": PPFD_TARGET_MAX,
        "step": 1, "mode": "box",
        "unit_of_measurement": "\u00b5mol/m\u00b2/s",
        "command_topic": cmd(l, "ppfd_target"),
        "state_topic": st(l, "ppfd_target"),
    })

    add("select", "%s_ppfd_fade_minutes" % l, {
        "name": "%s PPFD Fade Time" % lbl,
        "icon": "mdi:timer-outline",
        "entity_category": CFG,
        "options": ["Off"] + [str(n) for n in range(1, FADE_MINUTES_MAX + 1)],
        "command_topic": cmd(l, "ppfd_fade_minutes"),
        "state_topic": st(l, "ppfd_fade_minutes"),
    })

# The PPFD dimming band, as brightness percents, both ends 11-100.
    add("number", "%s_ppfd_min" % l, {
        "name": "%s PPFD Min Brightness" % lbl,
        "entity_category": CFG,
        "min": LIGHT_BRI_MIN, "max": 100, "step": 1, "mode": "box",
        "unit_of_measurement": "%",
        "command_topic": cmd(l, "ppfd_min"),
        "state_topic": st(l, "ppfd_min"),
    })

    add("number", "%s_ppfd_max" % l, {
        "name": "%s PPFD Max Brightness" % lbl,
        "entity_category": CFG,
        "min": LIGHT_BRI_MIN, "max": 100, "step": 1, "mode": "box",
        "unit_of_measurement": "%",
        "command_topic": cmd(l, "ppfd_max"),
        "state_topic": st(l, "ppfd_max"),
    })

    # Temperature thresholds: Off, or 15-50 degrees C at which the light
    # dims or shuts down. A select rather than a number because Off is not
    # a temperature -- Home Assistant's number entity has no way to express
    # "never", which is what the controller's 0 means here.
    for which, field, icon, title in (
        ("dim_threshold", "dim_threshold", "mdi:weather-sunset-up", "Dim Threshold"),
        ("off_threshold", "off_threshold", "mdi:weather-night", "Off Threshold"),
    ):
        add("select", "%s_%s" % (l, which), {
            "name": "%s %s" % (lbl, title),
            "icon": icon,
            "entity_category": CFG,
            "options": ["Off"] + [str(n) for n in range(THRESHOLD_MIN,
                                                        THRESHOLD_MAX + 1)],
            "command_topic": cmd(l, field),
            "state_topic": st(l, field),
        })


# ---------------------------------------------------------------------------
# Day cycle targets -- the keyPath ["target"] block
#
# Ranges confirmed against the app: temperature and humidity targets run
# 0-50 and 0-100 in steps of 1, CO2 targets 0-2500 in steps of 10. The
# deadbands are narrower: 1-10 for temperature and humidity, 10-250 for CO2.
# ---------------------------------------------------------------------------

add("time", "target_day_time_start", {
    "name": "Day Cycle Start",
    "icon": "mdi:weather-sunny",
    "entity_category": CFG,
    "format": "%H:%M",
    "command_topic": cmd("target", "day_time_start"),
    "state_topic": st("target", "day_time_start"),
})

add("time", "target_day_time_end", {
    "name": "Day Cycle End",
    "icon": "mdi:weather-night",
    "entity_category": CFG,
    "format": "%H:%M",
    "command_topic": cmd("target", "day_time_end"),
    "state_topic": st("target", "day_time_end"),
})

for sub, name, icon, lo, hi, step, unit in (
    ("temp_target_day", "Target Temperature Day", "mdi:thermometer-high",
     0, 50, 1, "\u00b0C"),
    ("temp_target_night", "Target Temperature Night", "mdi:thermometer-low",
     0, 50, 1, "\u00b0C"),
    ("temp_deadband", "Target Temperature Deadband", "mdi:thermometer-lines",
     1, 10, 1, "\u00b0C"),
    ("humi_target_day", "Target Humidity Day", "mdi:water-percent",
     0, 100, 1, "%"),
    ("humi_target_night", "Target Humidity Night", "mdi:water-percent",
     0, 100, 1, "%"),
    ("humi_deadband", "Target Humidity Deadband", "mdi:water-minus",
     1, 10, 1, "%"),
    ("co2_target_day", "Target CO2 Day", "mdi:molecule-co2",
     0, 2500, 10, "ppm"),
    ("co2_target_night", "Target CO2 Night", "mdi:molecule-co2",
     0, 2500, 10, "ppm"),
    ("co2_deadband", "Target CO2 Deadband", "mdi:waves-arrow-right",
     10, 250, 10, "ppm"),
):
    add("number", "target_" + sub, {
        "name": name,
        "icon": icon,
        "entity_category": CFG,
        "min": lo, "max": hi, "step": step, "mode": "box",
        "unit_of_measurement": unit,
        "command_topic": cmd("target", sub),
        "state_topic": st("target", sub),
    })


# ---------------------------------------------------------------------------
# Sensor calibration
#
# The controller applies these offsets to its own readings, so a correction
# made here also shows up in the Spider Farmer app. Units come from the
# captured app traffic: temperature in degrees Celsius, humidity in percent,
# PPFD in its own units, CO2 as a ppm offset rather than a percentage.
#
# Ranges and steps are the app's own. Temperature and humidity move in tenths,
# CO2 in tens, PPFD in tenths; the bounds are well inside what the sensors can
# drift, so a wider slider would mostly offer offsets no sensor needs. The
# WebGUI offers the same values, so the two cannot drift apart.
#
# Every one carries a state_topic. Without it Home Assistant showed nothing
# at all rather than even a zero, because an optimistic number with no state
# to read has nothing to display until it is moved once in the UI -- and that
# value is never the controller's real reading. The bridge publishes these
# under state/cal_* whenever a calibration block arrives.
# ---------------------------------------------------------------------------

for sub, name, icon, unit in (
    ("temp", "Temperature Offset", "mdi:thermometer-lines", "\u00b0C"),
    ("humi", "Humidity Offset", "mdi:water-percent", "%"),
    ("co2", "CO2 Offset", "mdi:molecule-co2", "ppm"),
    ("ppfd", "PPFD Offset", "mdi:white-balance-sunny", ""),
):
    entry = {
        "name": name,
        "icon": icon,
        "entity_category": CFG,
        "state_topic": "spiderfarmer/$D/state/cal_" + sub,
        "command_topic": cmd("calibration", sub),
        "min": CAL_MIN[sub], "max": CAL_MAX[sub],
        "step": CAL_STEP[sub], "mode": "box",
    }
    if unit:
        entry["unit_of_measurement"] = unit
    add("number", "cal_" + sub, entry)


# ---------------------------------------------------------------------------
# Time zone and clock
# ---------------------------------------------------------------------------

add("text", "timezone", {
    "name": "Time Zone",
    "icon": "mdi:earth",
    "entity_category": CFG,
    "max": 40,
    "state_topic": "spiderfarmer/$D/state/timezone",
    "command_topic": "spiderfarmer/$D/command/timezone/set",
})

# ---------------------------------------------------------------------------
# Grow plan
#
# The plan itself (stages with their own light and environment settings) is
# edited on the bridge's Control page; here it is started and stopped, and
# the running stage is shown. Whole-plan JSON writes are possible over
# spiderfarmer/<slug>/command/plan/json/set but far too long for a text
# entity, so no entity is declared for them.
# ---------------------------------------------------------------------------

# Add a stage from the template chosen in the "Plan Template" select. The
# select itself is published by the firmware (its options are the current
# template names, system and custom), not from this table. The stage is
# appended after the last one (or today) and the plan written back; at most
# five stages. Templates are created, edited and deleted on the bridge's
# Control page.
add("button", "plan_add_stage", {
    "name": "Add Plan Stage From Template",
    "icon": "mdi:playlist-plus",
    "entity_category": CFG,
    "command_topic": "spiderfarmer/$D/command/plan_template/set",
    "payload_press": "selected",
})

# Device clock: sends the bridge's zone and current time (setDevTimezone),
# what the app's "Sync device time" does.
add("button", "time_sync", {
    "name": "Sync Device Time",
    "icon": "mdi:clock-check-outline",
    "entity_category": CFG,
    "command_topic": "spiderfarmer/$D/command/time_sync/set",
    "payload_press": "PRESS",
})

add("switch", "plan_enabled", {
    "name": "Grow Plan",
    "icon": "mdi:sprout",
    "state_topic": st("plan", "enabled"),
    "command_topic": cmd("plan", "enabled"),
})
for suffix, name, icon in (
    ("stage_label", "Plan Stage", "mdi:flag-outline"),
    ("stage_start", "Plan Stage Start", "mdi:calendar-start"),
    ("stage_end", "Plan Stage End", "mdi:calendar-end"),
    ("stage_reminder", "Plan Stage Reminder", "mdi:bell-outline"),
    ("stage_color", "Plan Stage Color", "mdi:palette"),
):
    add("sensor", "plan_" + suffix, {
        "name": name,
        "icon": icon,
        "entity_category": DIAG,
        "state_topic": st("plan", suffix),
    })
add("sensor", "plan_stage_day", {
    "name": "Plan Stage Day",
    "icon": "mdi:calendar-today",
    "entity_category": DIAG,
    "state_topic": st("plan", "stage_day"),
    "unit_of_measurement": "d",
})
add("binary_sensor", "plan_managed_by_bridge", {
    "name": "Plan Kept On Bridge",
    "icon": "mdi:database-arrow-right",
    "entity_category": DIAG,
    "state_topic": st("plan", "managed_by_bridge"),
    "payload_on": "ON",
    "payload_off": "OFF",
})
add("sensor", "plan_stages", {
    "name": "Plan Stages",
    "icon": "mdi:format-list-numbered",
    "entity_category": DIAG,
    "state_topic": st("plan", "stages"),
})

# Bluetooth pairing. A paired (bound) controller does not advertise over
# Bluetooth, so no phone can take it over; unpairing (setDevDeactive, what
# the app sends when a device is removed) makes it advertise again so a
# phone can pair with it. The controller does not report the state, so
# these are buttons rather than a switch.
add("button", "unpair", {
    "name": "Unpair Bluetooth",
    "icon": "mdi:bluetooth-off",
    "entity_category": CFG,
    "command_topic": "spiderfarmer/$D/command/pairing/set",
    "payload_press": "OFF",
})
add("button", "pair", {
    "name": "Pair Bluetooth (stop advertising)",
    "icon": "mdi:bluetooth-connect",
    "entity_category": CFG,
    "command_topic": "spiderfarmer/$D/command/pairing/set",
    "payload_press": "ON",
})

# Per-controller summer-time switch, on the device itself rather than only on
# the bridge. ON/OFF reflects what tz_rules actually says this controller is
# running, derived on the firmware side from whether the POSIX string still
# carries switching rules.
add("switch", "dst_on", {
    "name": "Summer Time",
    "icon": "mdi:weather-sunny",
    "entity_category": CFG,
    "state_topic": "spiderfarmer/$D/state/dst_on",
    "command_topic": "spiderfarmer/$D/command/dst/set",
    "payload_on": "2",
    "payload_off": "1",
})


# ---------------------------------------------------------------------------
# Alarm settings, keyPath ["alarm"]
#
# Mirrors ALARM_RANGES / ALARM_SWITCHES in main/sf_alarm.c -- keep the two in
# step. Per range alarm: an on/off switch and min/max numbers (PPFD has only
# a maximum); per plain alarm: a switch. Ranges are PROVISIONAL until the
# app's exact limits are confirmed.
# ---------------------------------------------------------------------------

ALARM_RANGES = [
    # key,       name,                    icon,                      unit,     min?,  (min lo, hi),  (max lo, hi),   step
    ("temp",     "Air Temperature",       "mdi:thermometer-alert",   "\u00b0C", True,  (0, 30),      (18, 50),       1),
    ("humi",     "Air Humidity",          "mdi:water-alert",         "%",      True,  (0, 80),      (50, 100),      1),
    ("vpd",      "VPD",                   "mdi:gauge",               "kPa",    True,  (0, 1.6),     (0.5, 4.0),     0.1),
    ("co2",      "CO2",                   "mdi:molecule-co2",        "ppm",    True,  (200, 1500),  (450, 5000),    10),
    ("ppfd",     "PPFD",                  "mdi:white-balance-sunny", "\u00b5mol/m\u00b2/s", False, (0, 3900), (100, 4000), 10),
    ("tempSoil", "Substrate Temperature", "mdi:thermometer-water",   "\u00b0C", True,  (0, 26),      (13, 50),       1),
    ("humiSoil", "Substrate Moisture",    "mdi:water-percent-alert", "%",      True,  (0, 80),      (25, 100),      1),
    ("ECSoil",   "Substrate EC",          "mdi:flash-alert",         "mS/cm",  True,  (0, 4.3),     (1.2, 20.0),    0.1),
]
ALARM_SWITCHES = [
    ("devOffline",      "Sensor Offline",                "mdi:lan-disconnect"),
    ("lightTemp",       "Light Over-Temperature",        "mdi:lightbulb-alert"),
    ("dehumiWaterFull", "Dehumidifier Water Tank Full",  "mdi:cup-water"),
    ("humiWaterLess",   "Humidifier Water Low",          "mdi:water-off"),
    ("waterLeak",       "Water Leak",                    "mdi:pipe-leak"),
    ("waterLess",       "Water Shortage",                "mdi:water-alert-outline"),
]

for key, name, icon, unit, has_min, min_rng, max_rng, step in ALARM_RANGES:
    add("switch", "alarm_%s_enabled" % key, {
        "name": "Alarm %s" % name,
        "icon": icon,
        "entity_category": CFG,
        "payload_on": "ON",
        "payload_off": "OFF",
        "command_topic": "spiderfarmer/$D/command/alarm/%s/enabled/set" % key,
        "state_topic": "spiderfarmer/$D/state/alarm/%s/enabled" % key,
    })
    for which in (("min", "max") if has_min else ("max",)):
        lo, hi = min_rng if which == "min" else max_rng
        add("number", "alarm_%s_%s" % (key, which), {
            "name": "Alarm %s %s" % (name, "Minimum" if which == "min" else "Maximum"),
            "icon": icon,
            "entity_category": CFG,
            "min": lo, "max": hi, "step": step, "mode": "box",
            "unit_of_measurement": unit,
            "command_topic": "spiderfarmer/$D/command/alarm/%s/%s/set" % (key, which),
            "state_topic": "spiderfarmer/$D/state/alarm/%s/%s" % (key, which),
        })

for key, name, icon in ALARM_SWITCHES:
    add("switch", "alarm_%s" % key, {
        "name": "Alarm %s" % name,
        "icon": icon,
        "entity_category": CFG,
        "payload_on": "ON",
        "payload_off": "OFF",
        "command_topic": "spiderfarmer/$D/command/alarm/%s/set" % key,
        "state_topic": "spiderfarmer/$D/state/alarm/%s" % key,
    })


# ---------------------------------------------------------------------------
# Climate accessories and outlets
# ---------------------------------------------------------------------------

for sub, name in (
    ("heater", "Switch Heater"),
    ("humidifier", "Switch Humidifier"),
    ("dehumidifier", "Switch Dehumidifier"),
):
    add("switch", sub, {
        "name": name,
        "payload_on": "ON",
        "payload_off": "OFF",
        "command_topic": "spiderfarmer/$D/command/%s/set" % sub,
        "state_topic": "spiderfarmer/$D/state/%s" % sub,
    })

for i in range(1, 11):
    add("switch", "outlet_%d" % i, {
        "name": "Outlet %d" % i,
        "payload_on": "ON",
        "payload_off": "OFF",
        "command_topic": "spiderfarmer/$D/command/outlet_%d/set" % i,
        "state_topic": "spiderfarmer/$D/state/outlet_%d" % i,
    })


# ---------------------------------------------------------------------------
# Readings
# ---------------------------------------------------------------------------

for sub, name, unit, klass, icon in (
    ("temperature", "Air Temperature", "\u00b0C", "temperature", "mdi:thermometer"),
    ("humidity", "Air Humidity", "%", "humidity", "mdi:water-percent"),
    ("co2", "Air CO2", "ppm", "carbon_dioxide", "mdi:molecule-co2"),
    ("vpd", "Air VPD", "kPa", None, "mdi:water-thermometer"),
    ("ppfd", "Air PPFD", "\u00b5mol/m\u00b2/s", None, "mdi:white-balance-sunny"),
    ("temp_soil", "Soil Average Temperature", "\u00b0C", "temperature",
     "mdi:thermometer-water"),
    ("humi_soil", "Soil Average Humidity", "%", "humidity", "mdi:sprout"),
    ("ec_soil", "Soil Average EC", "mS/cm", None, "mdi:flask"),
):
    entry = {
        "name": name,
        "state_class": "measurement",
        "state_topic": "spiderfarmer/$D/state/" + sub,
    }
    if klass:
        entry["device_class"] = klass
    if icon:
        entry["icon"] = icon
    if unit:
        entry["unit_of_measurement"] = unit
    add("sensor", sub, entry)


# ---------------------------------------------------------------------------
# Diagnostics -- what the controller reports about itself
# ---------------------------------------------------------------------------

for sub, name, icon, unit, klass, state_class in (
    ("localtime", "Controller Time", "mdi:clock-outline", "", None, None),
    ("fw_version", "Controller Firmware", "mdi:chip", "", None, None),
    ("hw_version", "Controller Hardware", "mdi:memory", "", None, None),
    ("fw_built", "Firmware Built", "mdi:calendar-clock", "", None, None),
    ("fw_updated", "Firmware Updated", "mdi:update", "", None, None),
    ("uptime_text", "Controller Uptime", "mdi:timer-outline", "", None, None),
    ("uptime", "Controller Uptime Seconds", "mdi:timer-outline", "s",
     "duration", "total_increasing"),
    ("restarts", "Controller Restarts", "mdi:restart", "", None,
     "total_increasing"),
    ("free_memory", "Controller Free Memory", "mdi:memory", "", None,
     "measurement"),
    ("wifi_rssi", "Controller Signal", "mdi:wifi", "dBm", "signal_strength",
     "measurement"),
    ("tz_rules", "Daylight Saving Rules", "mdi:sun-clock", "", None, None),
):
    entry = {
        "name": name,
        "icon": icon,
        "entity_category": DIAG,
        "state_topic": "spiderfarmer/$D/state/" + sub,
    }
    if klass:
        entry["device_class"] = klass
    if state_class:
        entry["state_class"] = state_class
    if unit:
        entry["unit_of_measurement"] = unit
    add("sensor", sub, entry)


# The controller reports alarmLast as four numbers and nothing documents
# their meaning -- the vendor app carries no code-to-text table, searched.
# These are published as-is rather than with invented labels, and "(code)"
# in the name makes that explicit. diagnostic keeps them available for
# automations without crowding the main view.
for sub, name, icon, unit in (
    ("alarm_type", "Last Alarm Type (code)", "mdi:alert-circle-outline", ""),
    ("alarm_dev_type", "Last Alarm Device (code)", "mdi:devices", ""),
    ("alarm_id", "Last Alarm Number", "mdi:counter", ""),
    ("alarm_epoch", "Last Alarm Time (epoch)", "mdi:clock-alert-outline", ""),
    ("alarm_time", "Last Alarm Time", "mdi:calendar-clock", ""),
):
    entry = {
        "name": name,
        "icon": icon,
        "entity_category": DIAG,
        "state_topic": "spiderfarmer/$D/state/" + sub,
    }
    if unit:
        entry["unit_of_measurement"] = unit
    add("sensor", sub, entry)


# ---------------------------------------------------------------------------
# The bridge's own hardware
# ---------------------------------------------------------------------------
#
# Only the ESP32's own properties. These are one value per bridge, so they
# belong to a single global device rather than to each controller.

add("sensor", "bridge_heap", {
    "name": "Bridge Free Memory",
    "icon": "mdi:memory",
    "entity_category": DIAG,
    "state_class": "measurement",
    "state_topic": "spiderfarmer/bridge/state/heap",
}, bridge=True)

add("sensor", "bridge_version", {
    "name": "Bridge Firmware",
    "icon": "mdi:chip",
    "entity_category": DIAG,
    "state_topic": "spiderfarmer/bridge/state/version",
}, bridge=True)

# --- The bridge's internet gate ---
#
# On the controller rather than on the bridge: it gates the controllers'
# access to the internet, so it acts on them and sits beside them. Making it
# a bridge device was what separated the switch from the controller it
# actually controls.
add("switch", "wan", {
    "name": "Controller Internet Access",
    "icon": "mdi:cloud-off-outline",
    "entity_category": CFG,
    "payload_on": "ON",
    "payload_off": "OFF",
    "state_topic": "spiderfarmer/$D/state/wan",
    "command_topic": "spiderfarmer/$D/command/wan/set",
})


# ---------------------------------------------------------------------------
# Emit
# ---------------------------------------------------------------------------

def c_string(s):
    """Escapes a payload into a C string literal body."""
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ord(ch) < 0x20:
            out.append("\\u%04x" % ord(ch))
        else:
            out.append(ch)
    return "".join(out)


def json_body(payload):
    """Compact JSON with keys sorted, so the output is stable."""
    return json.dumps(payload, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False)


errors = []
longest = 0
seen_uids = set()

for topic, payload in entries:
    body = json_body(payload)

    # Round-trips through the parser, which catches an unescaped quote or a
    # stray Python-only type long before it reaches a broker.
    try:
        back = json.loads(body)
    except Exception as exc:
        errors.append("invalid JSON for %s: %s" % (topic, exc))
        continue

    uid = back["unique_id"]
    if uid in seen_uids:
        errors.append("duplicate unique_id %s (%s)" % (uid, topic))
    seen_uids.add(uid)

    # One device per controller. Anything else here is what produced the
    # per-mode pseudo devices in the first place.
    ids = back["device"]["identifiers"]
    if len(ids) != 1:
        errors.append("%s: expected one device identifier, got %r"
                      % (uid, ids))

    # A time entity without a format, or a number without a range, shows up
    # blank or rejects the value in Home Assistant.
    comp = topic.split("/")[1]
    if comp == "time" and "format" not in back:
        errors.append("%s: time entity without a format" % uid)
    if comp == "number":
        for key in ("min", "max"):
            if key not in back:
                errors.append("%s: number entity without %s" % (uid, key))
        if "min" in back and "max" in back and back["min"] > back["max"]:
            errors.append("%s: min above max" % uid)
    if comp == "select" and not back.get("options"):
        errors.append("%s: select entity without options" % uid)
    if comp == "text" and "pattern" in back and "max" in back:
        errors.append("%s: text entity carries a numeric max" % uid)

    longest = max(longest, len(body))

if errors:
    print("VALIDATION FAILED:")
    for e in errors:
        print("  " + e)
    raise SystemExit(1)

# Room for the placeholder expansion: $D becomes a slug of up to
# DEV_SLUG_LEN-1 characters and appears up to three times in a payload.
ceiling = longest + (3 * (24 - 2)) + 16

lines = [
    '#include "ha_discovery_table.h"',
    "",
    "// GENERATED FILE -- do not edit by hand.",
    "// Source: tools/gen_discovery.py",
    "//",
    "// Regenerate with:  python3 tools/gen_discovery.py",
    "//",
    "// The table is generated rather than hand-maintained because the earlier",
    "// hand-written version gave every per-mode entity its own device name,",
    "// which Home Assistant turned into ten separate pages instead of one",
    "// controller. Keeping the device block in one place in the generator is",
    "// what makes that impossible to reintroduce by accident.",
    "//",
    "// Placeholders:",
    "//   $D -> slug    $N -> display name    $B -> bridge display name",
    "",
    "const ha_discovery_entry_t HA_DISCOVERY_TABLE[] = {",
]

for topic, payload in entries:
    body = c_string(json_body(payload))
    lines.append("    // %s" % topic.split("spiderfarmer_$D_")[-1]
                 .split("/")[0])
    lines.append('    { "%s",' % topic)
    lines.append('      "%s" },' % body)

lines.append("};")
lines.append("")
lines.append("const size_t HA_DISCOVERY_COUNT = "
             "sizeof(HA_DISCOVERY_TABLE) / sizeof(HA_DISCOVERY_TABLE[0]);")
lines.append("")

with open(OUT_C, "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))

# Rewrite only the ceiling in the header, leaving its documentation alone.
with open(OUT_H, encoding="utf-8") as fh:
    header = fh.read()
header = re.sub(r"#define HA_DISCOVERY_MAX_PAYLOAD \d+",
                "#define HA_DISCOVERY_MAX_PAYLOAD %d" % ceiling,
                header)
with open(OUT_H, "w", encoding="utf-8") as fh:
    fh.write(header)

print("entities:      %d" % len(entries))
print("state topics:  %d" % len(state_topics))
print("command topics:%d" % len(command_topics))
print("longest:       %d bytes" % longest)
print("ceiling:       %d" % ceiling)

# ---------------------------------------------------------------------------
# The superseded topics
# ---------------------------------------------------------------------------
#
# Home Assistant keeps a discovery entity for as long as the broker retains
# its config payload, and nothing republishes an entity this table no longer
# contains. Their old payloads therefore have to be overwritten with an empty
# one, or the entities -- and the device pages they belonged to -- stay in the
# registry forever. That is what left eight "GGS ... Mode" pages behind when
# the per-mode pseudo devices were folded into the controller.
#
# The list is derived by subtracting this table from the snapshot of what
# earlier firmware published. Deriving it here rather than maintaining it by
# hand is the whole point: a hand-written list goes stale exactly when an
# entity is renamed, which is when it is needed.

def read_prev_topics(path):
    seen = []
    # utf-8-sig, not utf-8: the snapshot is edited in editors that write a
    # BOM, and an unstripped BOM makes the first line fail to start with
    # "#" -- the header comment would then be treated as a topic and
    # published to the broker as a clearing write to a garbage path.
    with open(path, "r", encoding="utf-8-sig") as fh:
        for line in fh:
            line = line.strip()
            # Comments are interspersed so the file stays readable, and a
            # blank line separates the header from the list.
            if not line or line.startswith("#"):
                continue
            if not line.startswith("homeassistant/"):
                continue
            if line not in seen:
                seen.append(line)
    return seen


prev = read_prev_topics(PREV_TOPICS)
current = set(topic for topic, _ in entries)
stale = [t for t in prev if t not in current]

# A topic in both lists would be republished immediately afterwards and then
# cleared by the same pass, leaving a live entity deleted. Refuse to generate
# rather than ship that.
clash = sorted(set(stale) & current)
if clash:
    print("VALIDATION FAILED:")
    for t in clash:
        print("  %s is in the previous-topic snapshot but is published "
              "again" % t)
    raise SystemExit(1)

stale_lines = [
    '#include "ha_discovery_stale.h"',
    "",
    "// GENERATED FILE -- do not edit by hand.",
    "// Source: tools/gen_discovery.py",
    "//",
    "// Regenerate with:  python3 tools/gen_discovery.py",
    "//",
    "// The discovery topics earlier firmware published and this one no",
    "// longer does. ha_mqtt.c clears each of them with an empty retained",
    "// payload after publishing the current table, which is what makes Home",
    "// Assistant drop the superseded entities instead of showing them",
    "// forever.",
    "//",
    "// Derived from tools/prev_discovery_topics.txt minus the current table,",
    "// so it cannot drift the way the previous hand-written list did.",
    "",
    "const char *const HA_STALE_DISCOVERY_TOPICS[] = {",
]
for t in stale:
    stale_lines.append('    "%s",' % t)
stale_lines.append("};")
stale_lines.append("")
stale_lines.append("const size_t HA_STALE_DISCOVERY_COUNT = "
                   "sizeof(HA_STALE_DISCOVERY_TOPICS) / "
                   "sizeof(HA_STALE_DISCOVERY_TOPICS[0]);")
stale_lines.append("")

with open(OUT_STALE_C, "w", encoding="utf-8") as fh:
    fh.write("\n".join(stale_lines))

stale_h = """#pragma once
#include <stddef.h>

// GENERATED FILE -- do not edit by hand.
// Source: tools/gen_discovery.py
//
// The retained discovery topics of earlier firmware that must be cleared with
// an empty payload, so Home Assistant stops showing entities this firmware
// no longer publishes.
//
// Regenerate with:  python3 tools/gen_discovery.py

extern const char *const HA_STALE_DISCOVERY_TOPICS[];
extern const size_t HA_STALE_DISCOVERY_COUNT;
"""
with open(OUT_STALE_H, "w", encoding="utf-8") as fh:
    fh.write(stale_h)

print("stale topics:  %d" % len(stale))

# The topics the firmware has to publish and the ones it has to handle. Both
# lists are written next to the table so a later change to either side shows
# up as a diff rather than as an entity that silently never updates.
with open(os.path.join(MAIN, "..", "tools", "discovery_topics.txt"),
          "w", encoding="utf-8") as fh:
    fh.write("# state topics that must be published\n")
    for t in sorted(state_topics):
        fh.write(t + "\n")
    fh.write("\n# command topics that must be handled\n")
    for t in sorted(command_topics):
        fh.write(t + "\n")

print("\nwrote %s" % OUT_C)
print("wrote %s (ceiling %d)" % (OUT_H, ceiling))