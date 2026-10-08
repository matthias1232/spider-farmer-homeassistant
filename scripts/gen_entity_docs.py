#!/usr/bin/env python3
"""Generate the Home Assistant entity list in README.md.

The discovery table in firmware/ggs/main/ha_discovery_table.c is the single
source of truth for what the bridge exposes to Home Assistant. This script
parses that table and rewrites the region between the ENTITY-LIST markers in
README.md, so the documented entity list can never drift from the firmware.

Run from the repository root:

    python3 scripts/gen_entity_docs.py
"""

import re
import sys
from collections import OrderedDict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TABLE = ROOT / "firmware" / "ggs" / "main" / "ha_discovery_table.c"
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
        name = payload_field(payload, "name")
        if not name:
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


def render(entries) -> str:
    areas = OrderedDict()
    for e in entries:
        areas.setdefault(e["area"], []).append(e)

    area_order = [
        "Circulation fan", "Exhaust fan", "Lights", "Day/night cycle targets",
        "Climate & outlets", "Sensor calibration", "Sensor cleaning", "Alarms",
        "Grow plan", "Bluetooth pairing", "Time zone", "Time synchronisation",
        "Daylight saving", "Controller internet access",
        "Climate readings", "Controller info & diagnostics", "Bridge diagnostics",
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
        "(%s). Generated from the firmware discovery table by "
        "`scripts/gen_entity_docs.py`; do not edit by hand." % (len(entries), n_ctrl, n_bridge, summary)
    )
    return "\n".join(lines) + "\n"


def main() -> int:
    text = TABLE.read_text(encoding="utf-8")
    entries = parse_entries(text)
    if not entries:
        print("ERROR: no entries parsed", file=sys.stderr)
        return 1

    expected = {
        "fan": 3, "select": 18, "time": 20, "switch": 33, "light": 2,
        "number": 36, "sensor": 36, "binary_sensor": 1, "text": 1, "button": 4,
    }
    counts = {}
    for e in entries:
        counts[e["platform"]] = counts.get(e["platform"], 0) + 1
    if counts != expected:
        print("ERROR: platform counts %s != expected %s" % (counts, expected), file=sys.stderr)
        return 1
    if len(entries) != 154:
        print("ERROR: %d entries != 154" % len(entries), file=sys.stderr)
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
