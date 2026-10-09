#!/usr/bin/env python3
"""Generate the Home Assistant entity list in README.md.

Three firmware sources define what Home Assistant gets:

  * firmware/ggs/main/ha_discovery_table.c  - the per-controller table (plus the
    two bridge entries "Bridge Free Memory" / "Bridge Firmware")
  * firmware/ggs/main/ha_mqtt.c             - the controller "Plan Template"
    select (options depend on the templates) and the bridge's own entities
    (clock, time zone, network, diagnostics, restart, factory reset)

This script parses all of them and rewrites the region between the
ENTITY-LIST markers in README.md, so the documented entity list can never drift
from the firmware.

Run from the repository root:

    python3 scripts/gen_entity_docs.py
"""

import re
import sys
from collections import OrderedDict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TABLE = ROOT / "firmware" / "ggs" / "main" / "ha_discovery_table.c"
MQTT = ROOT / "firmware" / "ggs" / "main" / "ha_mqtt.c"
README = ROOT / "README.md"

START_MARKER = "<!-- ENTITY-LIST:START -->"
END_MARKER = "<!-- ENTITY-LIST:END -->"

# one entry: { "homeassistant/<platform>/spiderfarmer_<slug>/config", "...json..." }
ENTRY_RE = re.compile(
    r'\{\s*"homeassistant/([a-z_]+)/spiderfarmer_([^"]+)/config",\s*\n\s*"(\{.*?\})"\s*\}',
    re.S,
)

AREA_BY_SEGMENT = [
    ("fan", "Circulation fan"),
    ("blower", "Exhaust fan"),
    ("light", "Lights"),
    ("light2", "Lights"),
    ("target", "Day/night cycle targets"),
    ("calibration", "Sensor calibration"),
    ("cal_", "Sensor calibration"),
    ("alarm", "Alarms"),
    ("plan", "Grow plan"),
    ("sensor_cleaning", "Sensor cleaning"),
    ("pairing", "Bluetooth pairing"),
    ("dst", "Daylight saving"),
    ("timezone", "Time zone"),
    ("time_sync", "Time synchronisation"),
    ("wan", "Controller internet access"),
    ("heater", "Climate & outlets"),
    ("humidifier", "Climate & outlets"),
    ("dehumidifier", "Climate & outlets"),
    ("outlet_", "Climate & outlets"),
]

SENSOR_AREA = "Climate readings"
INFO_AREA = "Controller info & diagnostics"
BRIDGE_AREA = "Bridge diagnostics"

# Bridge-level entities published by publish_bridge_discovery() in ha_mqtt.c,
# keyed by the id in "homeassistant/<platform>/spiderbridge/<id>/config".
BRIDGE_AREA_BY_ID = {
    "tz_pick": "Bridge time & clock", "timezone": "Bridge time & clock",
    "dst": "Bridge time & clock", "dst_on": "Bridge time & clock",
    "dst_auto": "Bridge time & clock", "ntp": "Bridge time & clock",
    "time": "Bridge time & clock", "time_valid": "Bridge time & clock",
    "tz_push": "Bridge time & clock", "ntp_ex": "Bridge time & clock",
    "ntp_rd": "Bridge time & clock",
    "wan": "Bridge network", "cloudfw": "Bridge network",
    "dns_ex": "Bridge network", "dns_rd": "Bridge network",
    "ssid": "Bridge uplink & hotspot", "ip": "Bridge uplink & hotspot",
    "rssi": "Bridge uplink & hotspot", "signal": "Bridge uplink & hotspot",
    "clients": "Bridge uplink & hotspot", "wifi_drops": "Bridge uplink & hotspot",
    "uplink": "Bridge uplink & hotspot",
    "heap": "Bridge diagnostics", "uptime": "Bridge diagnostics",
    "version": "Bridge diagnostics",
    "bridge_name": "Bridge device", "reboot": "Bridge device",
    "factory_reset": "Bridge device",
}


def payload_field(payload: str, name: str) -> str:
    m = re.search(r'\\?"%s\\?":\\?"([^"\\]*)' % name, payload)
    return m.group(1) if m else ""


def area_of(segment: str, is_bridge: bool) -> str:
    if is_bridge:
        return BRIDGE_AREA
    for prefix, label in AREA_BY_SEGMENT:
        if segment == prefix or segment.startswith(prefix):
            return label
    if segment in (
        "temperature", "humidity", "co2", "vpd", "ppfd",
        "temp_soil", "humi_soil", "ec_soil",
    ):
        return SENSOR_AREA
    return INFO_AREA


def parse_entries(text: str):
    entries = []
    for m in ENTRY_RE.finditer(text):
        platform = m.group(1)
        slug = m.group(2)
        payload = m.group(3)
        is_bridge = "$D" not in slug
        topic_m = re.search(
            r'\\?"(?:state|command)_topic\\":\\?"spiderfarmer/(?:\$D|bridge)/(?:state|command)/([a-z_0-9]+)',
            payload,
        )
        segment = topic_m.group(1) if topic_m else ""
        # The nested "device" object has its own "name" ($N / $B placeholder).
        # Drop it first so only the entity's own top-level name is read.
        entity_payload = re.sub(r'\\?"device\\?":\{.*?\}', "", payload, flags=re.S)
        name = payload_field(entity_payload, "name")
        if not name or name in ("$N", "$B"):
            continue
        unit = payload_field(payload, "unit_of_measurement")
        category = payload_field(payload, "entity_category")
        is_control = "command_topic" in payload
        kind = "Control" if is_control else "Sensor"
        if category:
            kind += " (%s)" % category
        entries.append(
            {
                "platform": platform,
                "name": name,
                "unit": unit,
                "kind": kind,
                "area": area_of(segment, is_bridge),
                "bridge": is_bridge,
            }
        )
    return entries


def _kind(is_control: bool, category: str) -> str:
    kind = "Control" if is_control else "Sensor"
    return kind + (" (%s)" % category if category else "")


def parse_bridge_entries(text: str):
    """Entities that ha_mqtt.c builds by hand rather than from the table."""
    entries = []

    def add(platform, name, unit, category, is_control, area):
        entries.append({
            "platform": platform, "name": name, "unit": unit,
            "kind": _kind(is_control, category), "area": area, "bridge": True,
        })

    # 1) one snprintf per entity:  "homeassistant/<plat>/spiderbridge/<id>/config"
    one = re.compile(
        r'"homeassistant/([a-z_]+)/spiderbridge/([a-z_0-9]+)/config"\);\s*\n'
        r'\s*snprintf\(payload,\s*sizeof\(payload\),(.*?)\);\s*\n\s*esp_mqtt_client_publish',
        re.S,
    )
    for m in one.finditer(text):
        platform, ident, body = m.groups()
        nm = re.search(r'\{\\"name\\":\\"([^"\\]*)\\"', body)
        if not nm or nm.group(1) == "%s":
            continue  # generated by one of the loops below
        unit = re.search(r'unit_of_meas\\":\\"([^"\\]*)', body)
        cat = re.search(r'ent_cat\\":\\"([a-z]+)', body)
        add(platform, nm.group(1), unit.group(1) if unit else "",
            cat.group(1) if cat else "", "cmd_t" in body,
            BRIDGE_AREA_BY_ID.get(ident, BRIDGE_AREA))

    # 2) NET_SW[] - the network switches
    sw = re.search(r'NET_SW\[\]\s*=\s*\{(.*?)\n\s*\};', text, re.S)
    if sw:
        for ident, name in re.findall(r'\{\s*"([a-z_]+)",\s*"([^"]+)",\s*"mdi:[^"]+"\s*\}', sw.group(1)):
            add("switch", name, "", "config", True,
                BRIDGE_AREA_BY_ID.get(ident, BRIDGE_AREA))

    # 3) DIAG[] - the diagnostic sensors (extra = concatenated JSON literals)
    dg = re.search(r'DIAG\[\]\s*=\s*\{(.*?)\n\s*\};', text, re.S)
    if dg:
        item = re.compile(r'\{\s*"([a-z_]+)",\s*"([^"]+)",\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\}', re.S)
        for ident, name, extra in item.findall(dg.group(1)):
            unit = re.search(r'unit_of_meas\\":\\"([^"\\]*)', extra)
            add("sensor", name, unit.group(1) if unit else "", "diagnostic", False,
                BRIDGE_AREA_BY_ID.get(ident, BRIDGE_AREA))

    # 4) the controller's plan template select, built per controller
    if "plan_template_sel/config" in text:
        entries.append({
            "platform": "select", "name": "Plan Template", "unit": "",
            "kind": "Control (config)", "area": "Grow plan", "bridge": False,
        })
    return entries


def render(entries) -> str:
    areas = OrderedDict()
    for e in entries:
        areas.setdefault(e["area"], []).append(e)

    area_order = [
        "Circulation fan", "Exhaust fan", "Lights", "Day/night cycle targets",
        "Climate & outlets", "Sensor calibration", "Sensor cleaning", "Alarms",
        "Grow plan", "Bluetooth pairing", "Time zone", "Time synchronisation",
        "Daylight saving", "Controller internet access",
        "Climate readings", "Controller info & diagnostics",
        "Bridge time & clock", "Bridge network", "Bridge uplink & hotspot",
        "Bridge diagnostics", "Bridge device",
    ]
    ordered = sorted(
        areas.items(),
        key=lambda kv: area_order.index(kv[0]) if kv[0] in area_order else len(area_order),
    )

    lines = []
    lines.append("Grouped by function. *Kind*: Control = writable entity, Sensor = read-only.")
    lines.append("")
    lines.append("| Entity | Platform | Kind | Unit |")
    lines.append("|---|---|---|---|")
    for area, items in ordered:
        for e in sorted(items, key=lambda x: x["name"].lower()):
            lines.append(
                "| %s | %s | %s | %s |"
                % (e["name"], e["platform"], e["kind"], e["unit"] or "—")
            )

    counts = {}
    for e in entries:
        counts[e["platform"]] = counts.get(e["platform"], 0) + 1
    n_ctrl = sum(1 for e in entries if not e["bridge"])
    n_bridge = sum(1 for e in entries if e["bridge"])
    summary = ", ".join(
        "%s × %d" % (p.replace("_", "\\_"), counts[p]) for p in sorted(counts)
    )
    lines.append("")
    lines.append(
        "**%d entities in total** — %d per controller + %d on the bridge itself "
        "(%s). Generated from the firmware (`ha_discovery_table.c` and `ha_mqtt.c`) by "
        "`scripts/gen_entity_docs.py`; do not edit by hand." % (len(entries), n_ctrl, n_bridge, summary)
    )
    return "\n".join(lines) + "\n"


def main() -> int:
    text = TABLE.read_text(encoding="utf-8")
    entries = parse_entries(text)
    if not entries:
        print("ERROR: no entries parsed", file=sys.stderr)
        return 1

    entries += parse_bridge_entries(MQTT.read_text(encoding="utf-8"))

    # Guard against a parser regression: these are the numbers Home Assistant
    # shows on the two device pages (controller 153, bridge 30).
    n_ctrl = sum(1 for e in entries if not e["bridge"])
    n_bridge = sum(1 for e in entries if e["bridge"])
    if (n_ctrl, n_bridge) != (153, 30):
        print("ERROR: parsed %d controller + %d bridge entities, expected 153 + 30"
              % (n_ctrl, n_bridge), file=sys.stderr)
        return 1

    readme = README.read_text(encoding="utf-8")
    if START_MARKER not in readme or END_MARKER not in readme:
        print("ERROR: entity list markers not found in README", file=sys.stderr)
        return 1
    block = render(entries)
    new = re.sub(
        re.escape(START_MARKER) + ".*?" + re.escape(END_MARKER),
        START_MARKER + "\n" + block.rstrip("\n") + "\n" + END_MARKER,
        readme,
        flags=re.S,
    )
    README.write_text(new, encoding="utf-8")
    print("OK: %d entities written to README.md" % len(entries))
    return 0


if __name__ == "__main__":
    sys.exit(main())
