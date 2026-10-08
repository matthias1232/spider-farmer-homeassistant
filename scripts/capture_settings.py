#!/usr/bin/env python3
"""Capture the Settings page from a running bridge into a safe template.

The Settings page (``/``) is built in C from live configuration values, so it
cannot be extracted byte-for-byte like the JS-driven pages. To keep the demo a
1:1 copy of the real interface, this script fetches the real page from a running
bridge, replaces every personal value with a placeholder and writes
``scripts/settings_template.html``. ``scripts/extract_gui.py`` then fills those
placeholders with plausible demo values.

Usage (from the repository root, with a bridge reachable):

    python3 scripts/capture_settings.py http://<bridge-address>/

Only the part between ``<body>`` and ``</body></html>`` is kept.

No real value is stored here: the sanitizer below only knows *which fields* of
the page can hold personal data, never the data itself. Additionally you may
list remaining secrets in ``scripts/private_values.txt`` (one string per line,
``#`` starts a comment); that file is git-ignored, and the script aborts when
one of those strings survives in the output.

The script also aborts when a sensitive field still carries a value or when a
certificate block is left in the page, so no credential can slip into the
repository.
"""

import re
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "scripts" / "settings_template.html"
PRIVATE = ROOT / "scripts" / "private_values.txt"

# Page fields that can hold personal data. A non-empty value="..." is replaced
# with value="{{FIELD}}"; extract_gui.py fills the placeholder with a neutral
# demo value afterwards. Fields the page shows empty (a hidden password, for
# example) stay empty, so the demo renders exactly like the live page.
SENSITIVE_FIELDS = [
    "sta_ssid", "sta_pass", "ap_ssid", "ap_pass", "static_ip", "static_gw",
    "ap_subnet", "ha_uri", "ha_user", "ha_pass", "mqtt_uri", "mqtt_user",
    "mqtt_pass", "slog_host", "slog_port", "ota_url", "ntp_target",
    "dns_target", "br_name", "admin_pass",
]

# Textareas can hold whole certificates; they are emptied the same way.
SENSITIVE_AREAS = ["slog_ca", "mqtt_ca"]

# Text the page prints verbatim, with live values inside. The lookahead keeps
# the replacement idempotent, so a re-run over an already sanitised page is a
# no-op.
TEXT_REPLACEMENTS = [
    (r"\bSpiderBridge [0-9A-Fa-f]{6}\b", "SpiderBridge {{BR_SUFFIX}}"),
    (r"Resolver in use: <b>(?!\{\{)[^<]*</b>",
     "Resolver in use: <b>{{RESOLVER}} (DHCP)</b>"),
]

CERT_BLOCKS = ["BEGIN CERTIFICATE", "BEGIN PRIVATE KEY", "BEGIN RSA"]


def sanitize(body: str) -> str:
    """Replace every personal value in a captured page body with a placeholder."""
    for field in SENSITIVE_FIELDS:
        ph = "{{%s}}" % field.upper()
        body = re.sub(r'(name="%s"[^>]*?value=")[^"]+(")'
                      % re.escape(field), r"\g<1>" + ph + r"\g<2>", body)
        body = re.sub(r'(value=")[^"]+("[^>]*?name="%s")'
                      % re.escape(field), r"\g<1>" + ph + r"\g<2>", body)
    for field in SENSITIVE_AREAS:
        ph = "{{%s}}" % field.upper()
        pattern = (r'(<textarea\b[^>]*name="%s"[^>]*>)([^<]*)(</textarea>)'
                   % re.escape(field))
        body = re.sub(pattern,
                      lambda m: m.group(1) + (ph if m.group(2).strip()
                                              else m.group(2)) + m.group(3),
                      body)
    for pattern, replacement in TEXT_REPLACEMENTS:
        body = re.sub(pattern, replacement, body)
    return body


def _live(tag: str) -> str:
    """The value of an input tag when it is not a placeholder."""
    value = re.search(r'value="([^"]*)"', tag)
    if not value or not value.group(1):
        return ""
    if re.fullmatch(r"\{\{\w+\}\}", value.group(1)):
        return ""
    return value.group(1)


def problems(body: str) -> list:
    """Everything that must not reach the repository."""
    out = []
    for m in re.finditer(r"<input\b[^>]*>|<textarea\b[^>]*>.*?</textarea>",
                         body, re.S):
        tag = m.group(0)
        name = re.search(r'name="([^"]*)"', tag)
        if not name:
            continue
        if name.group(1) in SENSITIVE_FIELDS and _live(tag):
            out.append("field %r still carries a live value" % name.group(1))
        if name.group(1) in SENSITIVE_AREAS:
            inner = re.search(r">([^<]*)</textarea>", tag).group(1)
            if inner.strip() and "{{" not in inner:
                note = ("a stored certificate"
                        if any(b in inner for b in CERT_BLOCKS) else "content")
                out.append("textarea %r still carries %s"
                           % (name.group(1), note))
    for pattern, _ in TEXT_REPLACEMENTS:
        if re.search(pattern, body):
            out.append("verbatim text still present (%s)" % pattern)
    if PRIVATE.is_file():
        for line in PRIVATE.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line in body:
                out.append("value from private_values.txt still present")
    return out


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: capture_settings.py http://<bridge-address>/",
              file=sys.stderr)
        return 2
    base = sys.argv[1]
    url = base.rstrip("/") + "/"
    print("Fetching %s ..." % url)
    with urllib.request.urlopen(url, timeout=30) as r:
        html = r.read().decode("utf-8", "replace")

    m = re.search(r"<body>(.*)</body></html>", html, re.S)
    if not m:
        print("ERROR: could not find the page body", file=sys.stderr)
        return 1
    body = sanitize(m.group(1))

    bad = problems(body)
    if bad:
        for what in bad:
            print("ERROR: %s" % what, file=sys.stderr)
        return 1

    header = (
        "<!-- Settings page body, captured from the real bridge and sanitised.\n"
        "     Regenerate with scripts/capture_settings.py against a running\n"
        "     bridge. Placeholders are filled by scripts/extract_gui.py. -->\n"
    )
    OUT.write_text(header + body + "\n", encoding="utf-8")
    print("Wrote %s (%d bytes)" % (OUT, OUT.stat().st_size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
