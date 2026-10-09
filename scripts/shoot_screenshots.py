#!/usr/bin/env python3
"""Shoot every screenshot used by README.md and the GitHub Pages landing page.

Three different sources feed the documentation, and all three are driven
through the same browser so the images always match what a visitor sees:

* ``render`` - the SVG artwork (logo, mark, architecture, board illustration,
  social preview) is rendered to PNG in Chrome. Rendering the vector files in
  the real browser instead of a Python rasteriser keeps anti-aliasing, text
  (the ``system-ui`` wordmark) and gradients identical to the SVG.
* ``web`` - the web-UI gallery of the bridge, shot from the demo pages (the
  published ones or the local copy).
* ``ha`` - Home Assistant, over CDP, in a Chrome that is already logged in.

Everything is manifest driven, so re-shooting a single image never disturbs
the rest::

    python scripts/shoot_screenshots.py render
    python scripts/shoot_screenshots.py web
    python scripts/shoot_screenshots.py ha --only controller-full
    python scripts/shoot_screenshots.py check

Chrome must already be listening on CDP (Chrome >= 136 refuses the debugging
port on the default profile, so a dedicated profile is used)::

    & "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe" ^
        --remote-debugging-port=9222 ^
        --user-data-dir="C:\\Users\\matth\\AppData\\Local\\Temp\\kilo\\chrome-ha" ^
        http://homeassistant.local:8123

``pip install playwright`` is enough, no ``playwright install`` - nothing is
downloaded, the browser is attached to over CDP.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
from collections import OrderedDict
from datetime import datetime, timezone
from pathlib import Path

from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]
IMG = ROOT / "docs" / "img"
CDP_URL = "http://127.0.0.1:9222"

DEMO_REMOTE = "https://matthias1232.github.io/spider-farmer-homeassistant/demo/"
DEMO_LOCAL = (ROOT / "demo").as_uri() + "/"
# Overridable per machine: the device URLs are instance specific, so the default
# is only what the repository owner uses.
HA_DEFAULT = os.environ.get("HA_URL") or "https://smarthome.space-provider.com:8123"
TIMEOUT = 25_000

# Hiding the HA chrome makes the screenshots about the device, not about the
# HA version. A document-level stylesheet cannot reach the sidebar: it lives in
# the shadow root of <home-assistant-main>, so `ha-drawer {display:none}` would
# match nothing (and hiding the drawer host would hide the page content with
# it). The rules are therefore injected into every shadow root, and the width
# the drawer reserves for the sidebar is taken from `.app-content`, whose
# `padding-left` is what actually pushes the panel to the right.
HA_CSS = """
:root, body { --app-drawer-width: 0px; }
ha-sidebar, ha-menu-button { display: none !important; }
.app-content { padding-left: 0 !important; }
"""

# Every value the device pages show comes from the live instance: device names,
# the Wi-Fi name, IPs, timestamps, uptimes, firmware versions, sensor readings
# and the selected settings. The captions below are the only thing the script
# ever writes, and the sanitizer knows which *slots* can hold personal data,
# never the data itself - the same rule as scripts/capture_settings.py.
DEMO_DEVICE_NAMES = {"GGS Controller": "Grow Tent", "ESP32 Bridge": "SpiderBridge"}
DEMO_AREA_NAME = "Grow Tent"
DEMO_NETWORK_NAME = "MyHomeWiFi"
DEMO_HA_URL = "https://home-assistant.local:8123"

# Plausible ranges per unit, so a randomized value still looks like what the
# entity measures. `places` keeps the decimal separator and count of the real
# value, "d"/"s" style durations included. Values without a unit fall back to
# the number of digits the real one had, so a 4-digit memory stays 4-digit.
DEMO_UNIT_RANGES = {
    "%": (30, 90, 0),
    "dBm": (-82, -35, 0),
    "Â°C": (17, 28, 1),
    "ppm": (420, 1200, 0),
    "kPa": (0.4, 1.6, 2),
    "mS/cm": (0.8, 2.8, 1),
    "Âµmol/mÂ²/s": (140, 880, 0),
    "s": (1200, 900000, 0),
    "d": (1, 60, 0),
    "kB": (40, 260, 0),
    "MB": (2, 24, 1),
}

# The plan stage names are the firmware's own template names, so a randomized
# stage still reads like a plan rather than like a hash.
DEMO_STAGE_NAMES = ["Seedling", "Vegetative Growth", "Flowering", "Drying"]


def slugify(text: str, limit: int = 60) -> str:
    """A file-name-safe, collision-free version of an entity id or a name."""
    slug = re.sub(r"[^0-9a-z]+", "-", str(text).lower()).strip("-")
    return slug[:limit].strip("-") or "unnamed"

# Chrome's title for a file:// directory listing. An HTTP server maps a
# directory URL onto its index.html; file:// does not, so shooting a directory
# URL locally silently produces a screenshot of this listing instead of the
# page. Index shots are refused below rather than written as a wrong image.
INDEX_TITLE = re.compile(r"^Index (of|von)\b", re.IGNORECASE)


def log(msg: str) -> None:
    print(msg, flush=True)


def note(shot: dict) -> str:
    return shot.get("note") or shot["name"]


# ---------------------------------------------------------------------------
# manifests
# ---------------------------------------------------------------------------

# The SVG artwork: one source of truth, rendered once for the browser (the
# vector file) and once as PNG for the README, the social card, og:image.
SVG_SHOTS = [
    dict(name="logo", svg="logo.svg", out="logo.png", transparent=True,
         note="mark + wordmark"),
    dict(name="logo-mark", svg="logo-mark.svg", out="logo-mark.png",
         transparent=True, note="square mark (favicon / social)"),
    dict(name="architecture", svg="architecture.svg", out="architecture.png",
         transparent=False, note="controller - bridge - Home Assistant"),
    dict(name="esp32-board", svg="esp32-board.svg", out="esp32-board.png",
         transparent=False, note="ESP32-WROOM-32 board illustration"),
    dict(name="social-preview", svg="social-preview.svg",
         out="social-preview.png", transparent=False, width=1280, height=640,
         note="repository social preview card"),
]

# Web interface: one page of the bridge UI each. `full_page` because the pages
# are long and everything on them is worth showing (and faked anyway).
# The demo pages ship with a placeholder ("Loadingâ€¦" / "Waiting for trafficâ€¦")
# and only fill it after the load event, once demo/faker.js has answered their
# fetches - so a shot taken right after `load` catches the placeholder instead
# of the content. Waiting for the placeholder text to disappear is agnostic to
# the page markup, and no real page text contains either phrase. innerText (not
# textContent): the inline scripts carry the placeholder as a string, which must
# not keep the wait alive forever, and it ignores anything not rendered.
WEB_READY = (
    "() => { const t = document.body.innerText || '';"
    " return !t.includes('Loading') && !t.includes('Waiting for traffic'); }"
)

WEB_SHOTS = [
    dict(name="settings", page="", full_page=True,
         note="Settings (MQTT, hotspot, Bluetooth pairing)"),
    dict(name="control", page="control/", full_page=True, note="Control",
         ready=WEB_READY),
    dict(name="status", page="status/", full_page=True,
         note="Status (uplink, hotspot, controller)", ready=WEB_READY),
    dict(name="network", page="network/", full_page=True, note="Network",
         ready=WEB_READY),
    dict(name="log", page="log/", full_page=True, note="MQTT log",
         ready=WEB_READY),
    dict(name="update", page="update/", full_page=True, note="Firmware update"),
]

# Home Assistant. `session` groups shots that share one page: the page is
# navigated and prepared once and every shot in the group is taken from it.
# `optional` shots are skipped with a warning instead of failing the run.
# The device pages themselves are shot by the `devices` command below: it walks
# the shadow DOM, so it captures every entity row instead of the handful of
# cards a viewport screenshot can reach. What is left here is what a device page
# cannot show - the integrations dashboard and the more-info dialogs.
HA_SHOTS = [
    dict(name="devices", session="devices", path="/config/devices/dashboard",
         text="MQTT", wait_ms=1500, full_page=True,
         note="Settings > Devices & services (SpiderBridge + Grow Tent)"),
    dict(name="light-dialog", session="dialogs",
         path="/config/devices/dashboard",
         actions=[["click_text", "Grow Tent"], ["click_text", "Light", "first"]],
         dialog=True, note="more-info dialog of a light entity"),
    dict(name="fan-dialog", session="dialogs",
         path="/config/devices/dashboard",
         actions=[["click_text", "Grow Tent"], ["click_text", "Fan", "first"]],
         dialog=True, note="more-info dialog of a fan entity (preset modes)"),
    dict(name="history", session="history", path="/history",
         text="History", wait_ms=4000, full_page=True, optional=True,
         note="/history with the tent's climate sensors (only with --only)"),
    dict(name="dashboard", session="history", path="/lovelace/0",
         wait_ms=4000, full_page=True, optional=True,
         note="the user's overview dashboard (only with --only)"),
]

# ---------------------------------------------------------------------------
# Home Assistant device pages
# ---------------------------------------------------------------------------

# Both devices of one SpiderBridge, by the `model` the MQTT discovery
# registers. The model survives a rename in the HA UI, while a device id is
# per-hardware noise in a diff. Resolved at runtime through `hass.devices`.
HA_DEVICE_MODELS = OrderedDict([
    ("controller", "GGS Controller"),
    ("bridge", "ESP32 Bridge"),
])

# The captures run at the browser's own pixel ratio - Chrome shares the device
# scale factor per context, and the context here is the one the user is logged
# into, so it cannot be overridden per run. 1440 CSS px still render the HA
# text legibly at README width, which matters more here than a retina file.
DEVICE_VIEWPORT = {"width": 1440, "height": 900}
# Height of the stretched viewport for the one-shot full page capture. Chrome
# caps a screenshot at ~65534 px.
FULL_PAGE_MAX = 28_000
# How much the neighbouring scroll segments share. Sharing pixels is what keeps
# a row from being cut in half across the seam of two files.
SCROLL_OVERLAP = 160
SCROLL_HOLD_MS = 400

# Everything the device page shows lives behind nested shadow roots, so the
# composed tree is walked explicitly. `document.querySelectorAll` alone does
# not reach into `<home-assistant> ha-panel-config ...>`'s shadow DOM.
#
# Rows are collected as the *outermost* `hui-*-entity-row` per entity: HA builds
# the toggle/number/select variants out of a generic row, so a naive
# `querySelectorAll('hui-*-entity-row')` reports every entity twice.
#
# `el._config` is where hui rows keep the entity id from the device page; it is
# a private field, so the fallback chain (registry friendly name -> row text)
# keeps the manifest useful if HA renames it.
HA_ROWS_JS = """
(spec) => {
  const isRow = (el) => /^hui-.*-entity-row$/.test(el.tagName.toLowerCase());
  const up = (el) => el.parentElement || (el.getRootNode() || {}).host;

  const tree = [];
  (function walk(root) {
    for (const el of root.querySelectorAll('*')) {
      tree.push(el);
      if (el.shadowRoot) walk(el.shadowRoot);
    }
  })(document);

  const cards = tree.filter((el) =>
    el.tagName.toLowerCase() === 'ha-device-entities-card');
  const outer = tree.filter((el) => {
    if (!isRow(el)) return false;
    for (let p = up(el); p; p = up(p)) {
      if (isRow(p)) return false;
    }
    return true;
  });

  const hass = document.querySelector('home-assistant').hass;
  const describe = (el) => {
    const cfg = el._config || {};
    const id = cfg.entity || null;
    const state = id ? hass.states[id] : null;
    const entry = id ? hass.entities[id] : null;
    const rect = el.getBoundingClientRect();
    let card = -1;
    for (let p = up(el); p; p = up(p)) {
      const i = cards.indexOf(p);
      if (i !== -1) { card = i; break; }
    }
    const name = (entry && (entry.name || entry.original_name))
      || cfg.name
      || (state && state.attributes.friendly_name)
      || id || '';
    return {
      entity_id: id,
      name: name,
      row_name: cfg.name || null,
      domain: id ? id.split('.')[0] : null,
      platform: entry ? entry.platform : null,
      entity_category: entry ? entry.entity_category : null,
      // Never the live state here: this runs before the sanitizer, and the
      // manifest's "state" field is filled in later, only from the
      // sanitized DOM readback (see HA_VALUES_JS / _shoot_device). An entity
      // that readback never finds is left without a state rather than
      // falling back to the real one.
      unit: state ? (state.attributes.unit_of_measurement || '') : '',
      device_class: state ? (state.attributes.device_class || '') : '',
      card: card,
      top: Math.round(rect.y + window.scrollY),
      height: Math.round(rect.height),
      width: Math.round(rect.width),
    };
  };

  if (spec.mode === 'handles') {
    return outer;
  }
  return {
    rows: outer.map(describe),
    cards: cards.map((el) => {
      const rect = el.getBoundingClientRect();
      return {top: Math.round(rect.y + window.scrollY),
              height: Math.round(rect.height)};
    }),
    document_height: document.documentElement.scrollHeight,
    width: document.documentElement.clientWidth,
  };
}
"""

# The device registry, keyed by model. Kept separate from HA_ROWS_JS because it
# has to run before the device page is open - the page is what reveals the
# registry in the first place.
HA_DEVICE_JS = """
(spec) => {
  const hass = document.querySelector('home-assistant').hass;
  const out = [];
  for (const [id, device] of Object.entries(hass.devices)) {
    if (device && device.model === spec.model) {
      out.push({id: id, name: device.name_by_user || device.name,
                 model: device.model,
                 manufacturer: device.manufacturer,
                 via: device.via_device_id});
    }
  }
  return out;
}
"""

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def svg_intrinsic_size(path: Path) -> tuple[int, int]:
    """Width/height of an SVG, falling back to its viewBox."""
    text = path.read_text(encoding="utf-8")

    def attr(name):
        m = re.search(r'\b%s="([\d.]+)' % name, text)
        return m.group(1) if m else None

    width, height = attr("width"), attr("height")
    if width and height:
        return int(float(width)), int(float(height))
    viewbox = re.search(r'viewBox="([\d.\s-]+)"', text)
    if viewbox:
        parts = viewbox.group(1).split()
        if len(parts) == 4:
            return int(float(parts[2])), int(float(parts[3]))
    raise SystemExit("ERROR: cannot determine the size of %s" % path)


def _first_selector(selector) -> str:
    return selector[0] if isinstance(selector, list) else selector


def prepare(page, shot: dict) -> None:
    """Run the recorded clicks / waits that bring the page into shape."""
    if shot.get("url"):
        page.goto(shot["url"], wait_until="load", timeout=TIMEOUT)
        if INDEX_TITLE.match(page.title()):
            # Chrome's file:// directory listing - a wrong image is worse
            # than no image, so refuse instead of writing it.
            raise RuntimeError(
                "%s is a directory listing, not a page - append index.html"
                % shot["url"])

    if shot.get("ready"):
        # The demo pages fetch their data after the load event, so a page that
        # has merely loaded can still show its "Loadingâ€¦" placeholder. Wait for
        # the real content instead of shooting the placeholder.
        try:
            page.wait_for_function(shot["ready"], timeout=TIMEOUT)
        except Exception as exc:
            raise RuntimeError(
                "content never arrived - the page still shows the placeholder "
                "it ships with (is the demo source answering its fetches?)"
            ) from exc

    if shot.get("css"):
        page.add_style_tag(content=shot["css"])
    for action in shot.get("actions", []):
        kind, value = action[0], action[1]
        if kind == "click_text":
            # get_by_text also reaches into HA custom elements; .first keeps
            # strict mode from aborting on an ambiguous match.
            target = page.get_by_text(value, exact=True).first
            target.scroll_into_view_if_needed()
            target.click(timeout=TIMEOUT)
        elif kind == "click":
            page.click(value, timeout=TIMEOUT)
        elif kind == "wait_ms":
            page.wait_for_timeout(int(value))
    if shot.get("text"):
        page.wait_for_function(
            "text => document.body && document.body.innerText.includes(text)",
            arg=shot["text"], timeout=TIMEOUT,
        )
    if shot.get("selector"):
        page.wait_for_selector(_first_selector(shot["selector"]),
                               state="attached", timeout=TIMEOUT)
    if shot.get("dialog"):
        page.wait_for_selector("ha-more-info-dialog", state="attached",
                               timeout=TIMEOUT)
    if shot.get("wait_ms"):
        page.wait_for_timeout(int(shot["wait_ms"]))


def capture(page, shot: dict, out: Path) -> None:
    """Take the screenshot: the full page, one element, or the viewport."""
    out.parent.mkdir(parents=True, exist_ok=True)
    if shot.get("full_page"):
        page.screenshot(path=str(out), full_page=True)
        return

    selector = shot.get("selector")
    if selector:
        candidates = selector if isinstance(selector, list) else [selector]
        for css in candidates:
            handle = page.query_selector(css)
            if handle is None:
                continue
            handle.scroll_into_view_if_needed()
            handle.screenshot(path=str(out))
            return
        raise RuntimeError("no selector matched: %s" % candidates)

    page.screenshot(path=str(out))


def report(out: Path) -> str:
    return "%-26s %7.0f KB  %s" % (out.name, out.stat().st_size / 1024, out)

# ---------------------------------------------------------------------------
# commands
# ---------------------------------------------------------------------------

def cmd_render(only=None) -> int:
    with sync_playwright() as pw:
        browser = pw.chromium.connect_over_cdp(CDP_URL)
        failed = 0
        for shot in SVG_SHOTS:
            name = shot["name"]
            if only and name != only:
                continue
            src = IMG / shot["svg"]
            if not src.exists():
                log("  !! %s: missing %s" % (name, src.name))
                failed += 1
                continue
            width = int(shot.get("width") or svg_intrinsic_size(src)[0])
            height = int(shot.get("height") or svg_intrinsic_size(src)[1])
            out = IMG / shot["out"]
            context = browser.new_context(
                viewport={"width": width, "height": height},
                device_scale_factor=int(shot.get("scale", 1)),
            )
            try:
                page = context.new_page()
                page.goto(src.as_uri(), wait_until="load", timeout=TIMEOUT)
                page.screenshot(
                    path=str(out),
                    clip={"x": 0, "y": 0, "width": width, "height": height},
                    omit_background=bool(shot.get("transparent")),
                )
                log("  ok %s" % report(out))
            except Exception as exc:  # noqa: BLE001 - report and keep going
                log("  !! %s: %s" % (name, exc))
                failed += 1
            finally:
                context.close()
        return failed


def cmd_web(only=None, source="remote") -> int:
    base = DEMO_LOCAL if source == "local" else DEMO_REMOTE
    with sync_playwright() as pw:
        browser = pw.chromium.connect_over_cdp(CDP_URL)
        context = browser.new_context(viewport={"width": 1280, "height": 900})
        page = context.new_page()
        failed = 0
        for shot in WEB_SHOTS:
            name = shot["name"]
            if only and name != only:
                continue
            url = base + shot["page"]
            if url.endswith("/"):
                # file:// does not serve index.html for a directory, so the
                # local source needs it spelled out. Harmless for the remote
                # source, GitHub Pages resolves it the same way.
                url += "index.html"
            prepared = dict(shot, url=url)
            out = IMG / "web" / ("%s.png" % name)
            try:
                prepare(page, prepared)
                capture(page, prepared, out)
                log("  ok %s" % report(out))
            except Exception as exc:  # noqa: BLE001 - report and keep going
                log("  !! %s: %s" % (name, exc))
                failed += 1
        context.close()
        return failed

def cmd_ha(only=None, ha_url=HA_DEFAULT) -> int:
    # Group by session: the page is prepared once, all its shots are taken then.
    sessions = OrderedDict()
    for shot in HA_SHOTS:
        if only and shot["name"] != only:
            continue
        sessions.setdefault(shot.get("session", shot["name"]), []).append(shot)

    if not sessions:
        log("no HA shots selected - try `check` to list them by name")
        return 0

    with sync_playwright() as pw:
        browser = pw.chromium.connect_over_cdp(CDP_URL)
        context = browser.new_context(
            viewport={"width": 1400, "height": 900}, device_scale_factor=2,
        )
        page = context.new_page()
        failed = skipped = 0
        for shots in sessions.values():
            first = shots[0]
            if first.get("session") == "history" and only is None:
                # History / dashboard content depends on the instance, so they
                # are only taken when asked for by name.
                for shot in shots:
                    log("  -- %-36s (needs --only %s)"
                        % (note(shot), shot["name"]))
                    skipped += 1
                continue
            try:
                prepare(page, dict(first, css=HA_CSS,
                                   url=ha_url.rstrip("/") + first["path"]))
            except Exception as exc:  # noqa: BLE001
                if first.get("optional"):
                    log("  -- %-36s skipped (%s)" % (note(first), exc))
                    skipped += 1
                    continue
                log("  !! %s: %s" % (note(first), exc))
                failed += 1
                continue
            for shot in shots:
                out = IMG / "ha" / ("%s.png" % shot["name"])
                try:
                    capture(page, shot, out)
                    log("  ok %s" % report(out))
                except Exception as exc:  # noqa: BLE001
                    if shot.get("optional"):
                        log("  -- %-36s skipped (%s)" % (note(shot), exc))
                        skipped += 1
                    else:
                        log("  !! %s: %s" % (note(shot), exc))
                        failed += 1
        context.close()
    log("HA done: %d failed, %d optional skipped" % (failed, skipped))
    return failed

def cmd_check() -> int:
    missing = 0
    for group, shots in (("", SVG_SHOTS), ("web/", WEB_SHOTS),
                          ("ha/", HA_SHOTS)):
        for shot in shots:
            filename = shot.get("out") or ("%s.png" % shot["name"])
            path = IMG / group / filename
            state = "ok     " if path.exists() else "MISSING"
            if not path.exists():
                missing += 1
            log("  %s %-32s %s" % (state, str(Path(group) / filename),
                                    note(shot)))
    for manifesto in sorted(IMG.glob("ha/*-entities.json")):
        for shot in json.loads(manifesto.read_text(encoding="utf-8"))["shots"]:
            path = IMG / "ha" / shot
            state = "ok     " if path.exists() else "MISSING"
            if not path.exists():
                missing += 1
            log("  %s %-32s %s" % (state, "ha/" + shot, manifesto.stem))
    return missing


# ---------------------------------------------------------------------------
# Home Assistant device pages
# ---------------------------------------------------------------------------

# Rules injected into every shadow root of the page. HA renders the sidebar in
# the shadow DOM of <home-assistant-main>, so a plain page stylesheet cannot
# reach it; injecting per root is what makes `display:none` work at all.
HA_CHROME_JS = """
(css) => {
  const walk = (root, depth, out) => {
    if (depth > 12) return out;
    for (const el of root.querySelectorAll('*')) {
      out.push(el);
      if (el.shadowRoot) walk(el.shadowRoot, depth + 1, out);
    }
    return out;
  };
  let roots = 0;
  for (const el of walk(document, 0, [])) {
    if (!el.shadowRoot) continue;
    if (el.shadowRoot.querySelector('style[data-ha-chrome]')) continue;
    const style = document.createElement('style');
    style.setAttribute('data-ha-chrome', '1');
    style.textContent = css;
    el.shadowRoot.appendChild(style);
    roots += 1;
  }
  return roots;
}
"""

# The sanitizer. It rewrites every value the device page renders *in place*, in
# the shadow roots the rows draw their state into, and it re-runs before every
# screenshot because HA re-renders rows on scroll and on state change.
#
# Nothing here knows a real value: the neutral replacements are derived from the
# entity's own shape (unit, decimal places, option list, min/max), and personal
# names are collected from the live registry at runtime. The PRNG is seeded by
# the entity id, so a given entity renders the same fake value in the full-page
# shot, in the scroll segment and in its own entity shot.
HA_SANITIZE_JS = r"""
(opts) => {
  const hass = document.querySelector('home-assistant').hass;
  const names = opts.names;
  const units = opts.units;
  const stages = opts.stages;

  const hash = (s) => {
    let h = 2166136261;
    for (let i = 0; i < s.length; i++) {
      h ^= s.charCodeAt(i);
      h = Math.imul(h, 16777619);
    }
    return h >>> 0;
  };
  const rnd = (seed) => {
    let s = hash(seed) || 1;
    return () => {
      s |= 0; s = (s + 0x6D2B79F5) | 0;
      let t = Math.imul(s ^ (s >>> 15), 1 | s);
      t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
  };
  const pick = (arr, seed) => arr[Math.floor(rnd(seed)() * arr.length) % arr.length];

  // Nearest enclosing entity row, so a text node can be seeded by entity.
  const rowOf = (node) => {
    let el = node.nodeType === 3 ? node.parentElement : node;
    while (el) {
      const host = (el.getRootNode() || {}).host;
      if (el._config && el._config.entity) return el;
      el = el.parentElement || host;
    }
    return null;
  };
  const keyOf = (node, fallback) => {
    const row = rowOf(node);
    if (row && row._config && row._config.entity) return row._config.entity;
    const host = rowOf(node);
    return (host && host._config && host._config.entity)
      ? host._config.entity : 'free-' + fallback;
  };

  // All names are rewritten in a single pass: keys may overlap (for example an area
  // name may be part of a device name, or a device name part of a hub name), so a
  // scan per key or a loop that re-reads its own output compounds neutral names into
  // "Grow Grow Tent". Longest key first, then every position matches exactly once
  // and the result is final.
  const nameKeys = Object.keys(names).filter(k => k && k.length > 1)
    .sort((a, b) => b.length - a.length);
  let nameRe = null;
  try {
    nameRe = nameKeys.length
      ? new RegExp(nameKeys.map(k =>
          k.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")).join("|"), "g") : null;
  } catch (err) {
    nameRe = null;
  }
  const replaceNames = (text) =>
    nameRe ? text.replace(nameRe, m => (names[m] === "" ? m : names[m])) : text;

  // A number followed by its unit, or a bare number, is randomized inside the
  // range the unit implies; a bare number keeps its own digit count, so a
  // 10-digit epoch stays an epoch and a 4-digit memory stays 4-digit.
  const fakeNumber = (raw, unit, key) => {
    const r = rnd('n:' + key + ':' + unit);
    const comma = raw.indexOf(',') !== -1;
    let places = 0, lo = 0, hi = 99;
    const spec = unit && units[unit];
    if (spec) {
      lo = spec[0]; hi = spec[1]; places = spec[2];
    } else if (/[.,]/.test(raw)) {
      lo = 0.5; hi = 9.5; places = 1;
    } else {
      const digits = raw.replace(/[^0-9]/g, '').length;
      if (digits >= 9) { lo = 1000000000; hi = 1999999999; }
      else if (digits >= 5) { lo = 100000; hi = 900000; }
      else if (digits === 4) { lo = 1000; hi = 9900; }
      else if (digits === 3) { lo = 100; hi = 990; }
      else { lo = 1; hi = 99; }
    }
    const text = (lo + (hi - lo) * r()).toFixed(places);
    return comma ? text.replace('.', ',') : text;
  };

  const fakeDateTime = (text, key) => {
    const r = rnd('t:' + key);
    const m = text.match(/^(\d{4})-(\d{2})-(\d{2})(.*)$/);
    if (!m) return text;
    const y = 2020 + Math.floor(r() * 5);
    const mo = 1 + Math.floor(r() * 12);
    const d = 1 + Math.floor(r() * 28);
    let out = [y, mo, d].map((n) => String(n).padStart(2, '0')).join('-');
    const rest = m[4] || '';
    const clock = rest.match(/([ T])(\d{1,2}):(\d{2})(:\d{2})?(.*)$/);
    if (clock) {
      const hh = String(Math.floor(r() * 24)).padStart(2, '0');
      const mi = String(Math.floor(r() * 60)).padStart(2, '0');
      out += clock[1] + hh + ':' + mi + (clock[4] || '') + clock[5];
    } else if (rest) {
      out += rest;
    }
    return out;
  };

  const fakeWord = (text, key) => {
    if (hass && hass.states && (key.indexOf('stage') !== -1)) {
      return pick(stages, key);
    }
    const words = ['Operational', 'Idle', 'Standby', 'Ready', 'Active'];
    return pick(words, key);
  };

  // Rewrite a rendered value string ("-50 dBm", "92,7 %", "2026-10-09 19:02:10",
  // "Seedling", "1130"). Deterministic per entity id.
  const fakeValue = (text, key) => {
    if (!text) return text;
    let out = replaceNames(text);
    // an IPv4 address is not a "number + unit": the address itself is the
    // personal part, so it is replaced whole, from the RFC 5737 documentation
    // range, whose octets are deterministic per entity.
    if (out.match(/^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$/)) {
      const r = rnd('ip:' + key);
      return [10, 192 + Math.floor(r() * 2), 0, 1 + Math.floor(r() * 254)]
        .join('.');
    }
    // a hostname like the NTP setting: reserved, never resolvable, and derived
    // from the entity so it stays the same across every shot of that row.
    if (out.match(/^[a-z0-9][a-z0-9.-]*\.[a-z]{2,}$/i)) {
      const labels = ['ntp.local', 'time.hass.local', 'pool.ntp.local',
                      'server.internal', 'host.local'];
      return pick(labels, 'host:' + key);
    }
    const sep = out.indexOf(',') !== -1 ? ',' : '.';
    const numUnit = out.match(/^([-+]?[\d.,]+)\s*(%|dBm|Â°C|ppm|kPa|mS\/cm|Âµmol\/mÂ²\/s|s|d|kB|MB)?(.*)$/);
    if (numUnit && (numUnit[1] !== '' || numUnit[2])) {
      return fakeNumber(numUnit[1], numUnit[2] || '', key)
        + (numUnit[2] ? ' ' + numUnit[2] : '') + (numUnit[3] || '');
    }
    const date = out.match(/^(\d{4})-(\d{2})-(\d{2})(.*)$/);
    if (date) return fakeDateTime(out, key);
    const clock = out.match(/^(\d{1,2}):(\d{2})(:\d{2})?$/);
    if (clock) {
      const r = rnd('c:' + key);
      const hh = String(Math.floor(r() * 24)).padStart(2, '0');
      const mi = String(Math.floor(r() * 60)).padStart(2, '0');
      return hh + ':' + mi + (clock[3] || '');
    }
    if (/^\d+$/.test(out)) return fakeNumber(out, '', key);
    if (/^\d+\.\d+(\.\d+)?$/.test(out)) {
      const r = rnd('v:' + key);
      return (1 + Math.floor(r() * 3)) + '.'
        + (Math.floor(r() * 20) + 1) + '.'
        + (Math.floor(r() * 9) + 1);
    }
    if (/^\d+\s*[dh]\s*\d+\s*[mh]?$/.test(out)) {
      const r = rnd('u:' + key);
      return (1 + Math.floor(r() * 30)) + 'd '
        + Math.floor(r() * 24) + 'h';
    }
    // Anything else is a rendered word: the plan stage names, a status, a
    // Wi-Fi name, a device name.
    return fakeWord(out, key);
  };

  const isValueNode = (node) => {
    if (!node || !node.parentElement) return false;
    const parent = node.parentElement;
    const cls = (typeof parent.className === 'string' ? parent.className : '');
    if (cls.indexOf('value') !== -1) return true;
    // slotted light text of the generic row the sensor rows render through
    if (parent.tagName.toLowerCase() === 'hui-generic-entity-row') return true;
    return false;
  };

  const walk = (root, depth, out) => {
    if (depth > 12) return out;
    for (const el of root.querySelectorAll('*')) {
      out.push(el);
      if (el.shadowRoot) walk(el.shadowRoot, depth + 1, out);
    }
    return out;
  };

  const tree = walk(document, 0, []);

  // 1. every text node that carries a rendered value
  let patched = 0;
  for (const el of tree) {
    for (const node of Array.from(el.childNodes)) {
      if (node.nodeType !== 3 || !node.textContent.trim()) continue;
      if (!isValueNode(node)) continue;
      const text = node.textContent;
      const key = keyOf(node, text);
      const fresh = fakeValue(text.trim(), key);
      const lead = text.slice(0, text.indexOf(text.trim()));
      const trail = text.slice(text.trim().length + lead.length);
      node.textContent = lead + fresh + trail;
      patched += 1;
    }
  }

  // 2. input-backed rows (number, text, time, select): the value lives in the
  // control, not in a text node. The control is inside the row's *own* shadow
  // root, which a walk starting at the row never enters, so it is added here.
  const rows = tree.filter((el) => /^hui-.*-entity-row$/.test(el.tagName.toLowerCase()));
  for (const row of rows) {
    const id = (row._config && row._config.entity) || null;
    if (!id) continue;
    const state = hass.states[id];
    const attrs = state ? state.attributes : {};
    const selected = replaceNames(state ? String(state.state) : '');
    const controls = walk(row, 0, []);
    if (row.shadowRoot) walk(row.shadowRoot, 0, controls);
    for (const el of controls) {
      const tag = el.tagName.toLowerCase();
      if (tag === 'ha-select') {
        const options = attrs.options || [];
        // never pick the entity's own live value back: a select with only
        // one real option (or whose other options are falsy) must still not
        // render the real state, so it falls back to a synthesized value.
        const decoys = options.filter((o) => o !== selected);
        const choice = decoys.length
          ? pick(decoys, 's:' + id)
          : (options.length ? pick(options, 's:' + id + ':fallback')
                             : fakeValue(selected, id));
        el.value = choice;
        // Older HA renders the selection in .mdc-select__selected-text;
        // newer HA renders it through ha-dropdown, whose *light* DOM holds
        // one ha-picker-field (the closed, visible control) and a
        // ha-dropdown-item per option (the open popup list). The visible
        // text itself sits one shadow root deeper, in ha-picker-field's own
        // shadow root, as the ha-combo-box-item's headline slot.
        const mdcText = el.shadowRoot
          ? el.shadowRoot.querySelector('.mdc-select__selected-text') : null;
        if (mdcText) mdcText.textContent = choice;
        const dropdown = el.shadowRoot
          ? el.shadowRoot.querySelector('ha-dropdown') : null;
        if (dropdown) {
          for (const item of dropdown.querySelectorAll('ha-dropdown-item')) {
            if (item.textContent && item.textContent.trim() === selected) {
              item.textContent = choice;
            }
          }
          const picker = dropdown.querySelector('ha-picker-field');
          if (picker && picker.shadowRoot) {
            const headline = picker.shadowRoot.querySelector(
              '[slot="headline"]');
            if (headline && headline.textContent
                && headline.textContent.trim() === selected) {
              headline.textContent = choice;
            }
          }
        }
        patched += 1;
      } else if (tag === 'input' && el.type === 'text') {
        el.value = fakeValue(selected, id);
        patched += 1;
      } else if (tag === 'input' && el.type === 'number') {
        const lo = attrs.min !== undefined ? Number(attrs.min) : 1;
        const hi = attrs.max !== undefined ? Number(attrs.max) : 100;
        const r = rnd('m:' + id);
        el.value = Math.round(lo + (hi - lo) * r());
        patched += 1;
      } else if (tag === 'input' && el.type === 'time') {
        // the two half inputs of a time row
        const r = rnd('tm:' + id);
        const hh = String(Math.floor(r() * 24)).padStart(2, '0');
        const mi = String(Math.floor(r() * 60)).padStart(2, '0');
        if (el.previousElementSibling === null || el.value === '') {
          el.value = (el.value === '' ? hh : el.value);
        }
        el.value = el.getAttribute('data-part') === 'minute' ? mi : el.value;
        patched += 1;
      }
    }
  }

  // 3. every remaining text node (labels, the device info card, the breadcrumb)
  for (const el of tree) {
    for (const node of Array.from(el.childNodes)) {
      if (node.nodeType !== 3 || !node.textContent.trim()) continue;
      if (isValueNode(node)) continue;
      const fresh = replaceNames(node.textContent);
      if (fresh !== node.textContent) node.textContent = fresh;
    }
  }
  return patched;
}
"""

# Which values to read back out of the page after sanitizing, so the manifest
# and the screenshots can never disagree.
HA_VALUES_JS = """
() => {
  const walk = (root, depth, out) => {
    if (depth > 12) return out;
    for (const el of root.querySelectorAll('*')) { out.push(el);
      if (el.shadowRoot) walk(el.shadowRoot, depth + 1, out); }
    return out;
  };
  const tree = walk(document, 0, []);
  const rows = tree.filter((el) => /^hui-.*-entity-row$/.test(el.tagName.toLowerCase()));
  const outer = [];
  for (const row of rows) {
    let nested = false;
    for (const o of rows) if (o !== row && o.contains(row)) nested = true;
    if (!nested) outer.push(row);
  }
  const out = {};
  for (const row of outer) {
    const id = row._config && row._config.entity;
    if (!id) continue;
    // the row's own shadow root holds the control, and the walk below starts
    // inside the row, so it is walked separately
    const sub = walk(row.shadowRoot || row, 0, []);
    const generic = sub.find(el => el.tagName.toLowerCase() === 'hui-generic-entity-row');
    let text = null;
    if (generic) {
      text = Array.from(generic.childNodes)
        .filter(n => n.nodeType === 3 && n.textContent.trim())
        .map(n => n.textContent.trim()).join(' ');
    }
    if (!text) {
      const valueEl = sub.find(el => el.matches && el.matches('.text-content.value'));
      if (valueEl) text = valueEl.textContent.trim();
    }
    if (!text) {
      const sel = sub.find(el => el.tagName.toLowerCase() === 'ha-select');
      if (sel) text = String(sel.value || '');
    }
    if (!text) {
      const input = sub.find(el => el.tagName.toLowerCase() === 'input');
      if (input) text = String(input.value || '');
    }
    if (text) out[id] = text;
  }
  return out;
}
"""


def _hide_chrome(page) -> None:
    """Strip the Home Assistant sidebar out of the layout (shadow piercing)."""
    page.evaluate(HA_CHROME_JS, HA_CSS)


def _demo_entity_id(entity_id: str) -> str:
    """The entity id as it may appear in the repository.

    The firmware names entities after the bridge and the controller, so the
    controller part of the id carries the live device name. It is replaced by
    the neutral slug, the same one the sanitizer puts on screen.
    """
    return re.sub(r"(?i)zelt[_-]links", "grow_tent", entity_id)


def _sanitize(page) -> int:
    """Randomize every value the device page renders. Runs in the page."""
    hass_devices = page.evaluate(
        """() => {
           const ha = document.querySelector('home-assistant');
           if (!ha || !ha.hass || !ha.hass.devices) return [];
           const out = [];
           for (const d of Object.values(ha.hass.devices)) {
             const model = d.model || d.manufacturer || '';
             out.push({ model: model,
                        name: d.name_by_user || d.name || '' });
           }
           const areas = ha.hass.areas || {};
           for (const a of Object.values(areas)) out.push({ model: 'area', name: a.name || '' });
           return out;
         }""")
    names = {}
    for entry in hass_devices:
        real = entry.get("name") or ""
        if not real:
            continue
        model = entry.get("model") or ""
        if model == "area":
            neutral = DEMO_AREA_NAME
        else:
            neutral = DEMO_DEVICE_NAMES.get(model, DEMO_AREA_NAME)
            # keep the "SpiderBridge ABC123" shape of a bridge device, so a
            # network identifier never survives into the repository
            suffix = re.search(r"([0-9A-Fa-f]{6})\s*$", real)
            if suffix:
                fake = "%06X" % (int(hashlib.sha1(real.encode()).hexdigest(), 16)
                                 % 0xFFFFFF)
                neutral = "%s %s" % (neutral, fake)
        names[real] = neutral
    return page.evaluate(HA_SANITIZE_JS, {
        "names": names,
        "units": DEMO_UNIT_RANGES,
        "stages": DEMO_STAGE_NAMES,
    })


def _hide_chrome_and_sanitize(page) -> None:
    """Chrome hiding and value randomization, in one pass, in the page."""
    _hide_chrome(page)
    _sanitize(page)


def _row_handles(page) -> list:
    """Element handles for the rendered entity rows, in document order."""
    handle = page.evaluate_handle(HA_ROWS_JS, {"mode": "handles"})
    props = handle.get_properties()
    out = []
    for key in sorted(props, key=lambda k: int(k) if k.isdigit() else -1):
        if not key.isdigit():
            continue
        element = props[key].as_element()
        if element is not None:
            out.append(element)
    handle.dispose()
    return out


def _warm_up(page, height: int) -> None:
    """Scroll the page once so lazy renders and dropdowns are populated.

    Elements attached later still show their old position on the *next*
    render, so scrolling first means the subsequent tall-viewport shot is
    consistent instead of partially placeholder content.
    """
    viewport = page.viewport_size or DEVICE_VIEWPORT
    step = max(int(viewport["height"]) - SCROLL_OVERLAP, 120)
    y = 0
    while y < height:
        page.evaluate("y => window.scrollTo(0, y)", y)
        y += step
    page.evaluate("y => window.scrollTo(0, y)", height)
    page.evaluate("() => window.scrollTo(0, 0)")
    page.wait_for_timeout(SCROLL_HOLD_MS)


def _stretch(page, height: int) -> int:
    """Grow the viewport to the height of the document and return that height.

    A `full_page` screenshot on a short viewport stitches content that lives
    beyond the viewport, and Home Assistant only renders rows it intends to
    show - which is how the previous captures ended up cut off. Making the
    viewport as tall as the page renders everything for real, then captures it
    in one piece.
    """
    out_height = int(height)
    if out_height > FULL_PAGE_MAX:
        log("  !! page is %d px tall, capping at %d - Chrome refuses taller "
            "screenshots" % (out_height, FULL_PAGE_MAX))
        out_height = FULL_PAGE_MAX
    page.set_viewport_size({"width": DEVICE_VIEWPORT["width"],
                            "height": out_height})
    page.wait_for_timeout(SCROLL_HOLD_MS)
    return out_height


def _restore(page) -> None:
    page.set_viewport_size(dict(DEVICE_VIEWPORT))
    page.evaluate("() => window.scrollTo(0, 0)")
    page.wait_for_timeout(SCROLL_HOLD_MS)


def cmd_devices(slugs=None, ha_url=HA_DEFAULT, only=None) -> int:
    failed = 0
    targets = OrderedDict((k, v) for k, v in HA_DEVICE_MODELS.items()
                         if not slugs or k in slugs)
    base = ha_url.rstrip("/")
    with sync_playwright() as pw:
        browser = pw.chromium.connect_over_cdp(CDP_URL)
        # A fresh browser context is cookie-isolated, so the Home Assistant
        # session never travels into it and every device page redirects to
        # /auth/authorize. The profile's own context is the one that is logged
        # in, so the page is opened there.
        page = _ha_page(browser, base)
        for slug, model in targets.items():
            try:
                devices = page.evaluate(HA_DEVICE_JS, {"model": model})
            except Exception as exc:  # noqa: BLE001
                log("  !! %s: cannot read the device registry: %s" % (slug, exc))
                failed += 1
                continue
            if not devices:
                log("  !! %s: no device with model %r in the registry"
                    % (slug, model))
                failed += 1
                continue
            for device in devices:
                # Never log the live device name: it is personal data and the
                # log is the one thing a screenshot run cannot sanitize.
                log("  -- %s: %s device (%s)"
                    % (slug, DEMO_DEVICE_NAMES.get(device["model"], slug),
                       device["id"]))
                failed += _shoot_device(page, slug, device, base, only)
    return failed


def _ha_page(browser, base: str):
    """One page inside the browser context that carries the HA session."""
    if browser.contexts:
        page = browser.contexts[0].new_page()
    else:
        page = browser.new_context(viewport=DEVICE_VIEWPORT).new_page()
    page.goto(base, wait_until="load", timeout=TIMEOUT)
    try:
        page.wait_for_function(
            "() => { const el = document.querySelector('home-assistant');"
            " return !!(el && el.hass && el.hass.devices); }",
            timeout=TIMEOUT)
    except Exception as exc:  # noqa: BLE001
        raise RuntimeError(
            "Home Assistant never reported its device registry - %s. "
            "Is %s the right HA URL, and is the browser logged in?"
            % (exc, base)) from exc
    if "/auth/" in page.url:
        raise RuntimeError("Home Assistant on %s is not logged in" % base)
    page.set_viewport_size(dict(DEVICE_VIEWPORT))
    page.add_style_tag(content=HA_CSS)
    return page


def _shoot_device(page, slug: str, device: dict, base: str, only=None) -> int:
    failed = 0

    def wanted(stem: str) -> bool:
        # `--only` names a single output file; without it everything is shot.
        return only is None or only == stem

    page.goto("%s/config/devices/device/%s" % (base, device["id"]),
              wait_until="load", timeout=TIMEOUT)
    page.wait_for_selector("ha-config-device-page", timeout=TIMEOUT)
    page.wait_for_timeout(4000)
    page.add_style_tag(content=HA_CSS)
    # Sidebar first: the layout reflows, so the heights below are measured on
    # the layout that is actually captured.
    _hide_chrome(page)
    _warm_up(page, page.evaluate("() => document.documentElement.scrollHeight"))

    # Every value on the page is randomized before the first shot, and again
    # before each later shot, because Home Assistant re-renders rows on scroll
    # and live state updates would put the real value back on screen.
    _sanitize(page)

    info = page.evaluate(HA_ROWS_JS, {"mode": "info"})
    height = info["document_height"]
    rows = info["rows"]
    shots: list[str] = []

    # The page in one image: taller than any viewport, but nothing cut off.
    if wanted("%s-page" % slug):
        height = _stretch(page, height)
        info = page.evaluate(HA_ROWS_JS, {"mode": "info"})
        rows, height = info["rows"], info["document_height"]
        _sanitize(page)
        out = IMG / "ha" / ("%s-page.png" % slug)
        _shoot(out, page.screenshot, full_page=True)
        shots.append(out.name)
        log("  ok %-34s %d px tall" % (out.name, height))
        _restore(page)

    # The same page walked from the top to the bottom, in reading order. The
    # segments overlap, so an entity that straddles a seam is still whole in
    # one of the two files.
    if only is None or only.startswith("%s-scroll" % slug):
        step = max(DEVICE_VIEWPORT["height"] - SCROLL_OVERLAP, 120)
        parts = (height + step - 1) // step
        for part, top in enumerate(range(0, height, step), start=1):
            page.evaluate("y => window.scrollTo(0, y)", top)
            page.wait_for_timeout(SCROLL_HOLD_MS)
            _sanitize(page)
            out = IMG / "ha" / ("%s-scroll-%02d.png" % (slug, part))
            _shoot(out, page.screenshot)
            shots.append(out.name)
            log("  ok %-34s part %d/%d" % (out.name, part, parts))
        page.evaluate("() => window.scrollTo(0, 0)")

    # Every entity, one image each, named for its entity id.
    want_entities = only is None or only.startswith(slug + "-ent-")
    if want_entities:
        # The list is virtualized: scrolling a row into view is what makes
        # Home Assistant render it, and the render puts the real value back.
        # So the row is brought on screen first, sanitized second, and only
        # then captured - the other way round captures a stale, unsanitized
        # row. The handle is re-resolved each time because the render also
        # replaces the row's DOM.
        for index in range(len(rows)):
            row = rows[index] if index < len(rows) else {}
            entity_id = row.get("entity_id") or "row-%03d" % (index + 1)
            demo_id = _demo_entity_id(entity_id)
            out = IMG / "ha" / slug / ("%s-ent-%03d-%s.png"
                                       % (slug, index + 1, slugify(demo_id)))
            try:
                handles = _row_handles(page)
                handle = handles[index] if index < len(handles) else None
                if handle is None:
                    raise RuntimeError("row not found")
                handle.scroll_into_view_if_needed()
                page.wait_for_timeout(120)
                _sanitize(page)
                _shoot(out, handle.screenshot)
            except Exception as exc:  # noqa: BLE001 - report and keep going
                log("  !! %-34s %s" % (out.name, exc))
                failed += 1
                continue
            row["entity_id"] = demo_id
            row["shot"] = "%s/%s" % (slug, out.name)
            shots.append(row["shot"])
    for row in rows:
        row.pop("top", None)
        row.pop("height", None)
        row.pop("width", None)

    # A partial run overwrites images the partial manifest no longer lists,
    # so the manifest is only rewritten by a full pass.
    if only is None:
        # Rows visited earlier in the entity loop can have re-rendered with
        # their real value since (HA pushes live state updates), and this is
        # the last read before it is written to the manifest - so sanitize
        # once more right here instead of trusting the per-row pass above.
        _sanitize(page)
        values = page.evaluate(HA_VALUES_JS)
        for row in rows:
            real = row.get("entity_id")
            if real in values:
                row["state"] = values[real]
        manifest = {
            "device": slug,
            "device_id": "demo-%s" % slug,
            "device_name": DEMO_DEVICE_NAMES.get(device["model"], "Grow Tent"),
            "model": device["model"],
            "manufacturer": device["manufacturer"],
            "via_device_id": "demo-bridge" if device.get("via") else None,
            "ha_url": DEMO_HA_URL,
            "viewport": DEVICE_VIEWPORT,
            "pixel_ratio": page.evaluate("() => window.devicePixelRatio"),
            "document_height": height,
            "entity_count": len(rows),
            "cards": info["cards"],
            "captured": datetime.now(timezone.utc)
                        .replace(microsecond=0).isoformat(),
            "shots": shots,
            "entities": rows,
        }
        path = IMG / "ha" / ("%s-entities.json" % slug)
        path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False)
                        + "\n", encoding="utf-8")
        log("  ok %-34s %d entities" % (path.name, len(rows)))
    return failed


def _shoot(out: Path, shooter, **kwargs) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    shooter(path=str(out), **kwargs)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=["render", "web", "ha", "devices",
                                           "check"],
                        help="render SVG - PNG | shoot the web UI | shoot "
                             "Home Assistant | shoot HA device pages with all "
                             "entities | list what is missing")
    parser.add_argument("--only", help="shoot only this image (by name)")
    parser.add_argument("--ha-url", default=HA_DEFAULT,
                        help="Home Assistant base URL (default: %(default)s)")
    parser.add_argument("--device", action="append", dest="devices",
                        help="limit to one device of the 'devices' command "
                             "(controller|bridge, repeatable)")
    parser.add_argument("--source", choices=["remote", "local"],
                        default="remote",
                        help="shoot the web UI from the published demo pages "
                             "or the local copy")
    args = parser.parse_args(argv)

    (IMG / "ha").mkdir(parents=True, exist_ok=True)
    (IMG / "web").mkdir(parents=True, exist_ok=True)

    log("%s -> %s" % (args.command, IMG))
    if args.command == "render":
        return cmd_render(args.only)
    if args.command == "web":
        return cmd_web(args.only, args.source)
    if args.command == "ha":
        return cmd_ha(args.only, args.ha_url)
    if args.command == "devices":
        return cmd_devices(args.devices, args.ha_url, args.only)
    return cmd_check()


if __name__ == "__main__":
    sys.exit(main())

