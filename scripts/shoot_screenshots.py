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
import re
import sys
from collections import OrderedDict
from pathlib import Path

from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]
IMG = ROOT / "docs" / "img"
CDP_URL = "http://127.0.0.1:9222"

DEMO_REMOTE = "https://matthias1232.github.io/spider-farmer-homeassistant/demo/"
DEMO_LOCAL = (ROOT / "demo").as_uri() + "/"
HA_DEFAULT = "http://homeassistant.local:8123"
TIMEOUT = 25_000

# Hiding the HA chrome makes the screenshots about the device, not about the
# HA version. `--app-drawer-width:0` is what HA reads to lay out the content,
# `ha-drawer` is the sidebar itself (hiding the host hides its shadow DOM).
HA_CSS = """
:root, body { --app-drawer-width: 0px; }
ha-drawer, ha-sidebar, ha-menu-button, .app-drawer, .mdc-drawer {
  display: none !important;
}
"""

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
# The demo pages ship with a placeholder ("Loading…" / "Waiting for traffic…")
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
HA_SHOTS = [
    dict(name="devices", session="devices", path="/config/devices/dashboard",
         text="MQTT", wait_ms=1500, full_page=True,
         note="Settings > Devices & services (SpiderBridge + Grow Tent)"),
    dict(name="controller-full", session="controller",
         path="/config/devices/dashboard",
         actions=[["click_text", "Grow Tent"]],
         text="Grow Tent", wait_ms=2500, full_page=True,
         note="device page 'Grow Tent'"),
    dict(name="controller-controls", session="controller",
         path="/config/devices/dashboard",
         actions=[["click_text", "Grow Tent"]],
         text="Controls", wait_ms=2500,
         selector=["ha-card:has-text('Controls')"],
         note="Controls card (fans, lights, outlets, plan)"),
    dict(name="controller-config", session="controller",
         path="/config/devices/dashboard",
         actions=[["click_text", "Grow Tent"]],
         text="Configuration", wait_ms=2500,
         selector=["ha-card:has-text('Configuration')"],
         note="Configuration card (schedules, targets, alarms, calibration)"),
    dict(name="controller-sensors", session="controller",
         path="/config/devices/dashboard",
         actions=[["click_text", "Grow Tent"]],
         text="Sensors", wait_ms=2500,
         selector=["ha-card:has-text('Sensors')"],
         note="Sensors card (climate readings)"),
    dict(name="controller-diagnostic", session="controller",
         path="/config/devices/dashboard",
         actions=[["click_text", "Grow Tent"]],
         text="Diagnostic", wait_ms=2500,
         selector=["ha-card:has-text('Diagnostic')"],
         note="Diagnostic card"),
    dict(name="bridge-device", session="bridge",
         path="/config/devices/dashboard",
         actions=[["click_text", "SpiderBridge"]],
         text="SpiderBridge", wait_ms=2500, full_page=True,
         note="device page 'SpiderBridge' (the bridge itself)"),
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
        # has merely loaded can still show its "Loading…" placeholder. Wait for
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
    return missing


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=["render", "web", "ha", "check"],
                        help="render SVG - PNG | shoot the web UI | shoot "
                             "Home Assistant | list what is missing")
    parser.add_argument("--only", help="shoot only this image (by name)")
    parser.add_argument("--ha-url", default=HA_DEFAULT,
                        help="Home Assistant base URL (default: %(default)s)")
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
    return cmd_check()


if __name__ == "__main__":
    sys.exit(main())
