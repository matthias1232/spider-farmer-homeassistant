#!/usr/bin/env python3
"""Verify that every image referenced by docs/index.html and README.md exists."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# 1. docs/index.html -> img/... references
html = (ROOT / "docs" / "index.html").read_text(encoding="utf-8")
missing = []
for m in re.finditer(r'src="([^"]+)"', html):
    ref = m.group(1)
    # treat relative-to-repo paths
    if ref.startswith(("http://", "https://", "#")):
        continue
    cand = ROOT / "docs" / ref
    if not cand.exists():
        missing.append(("docs/index.html", ref, cand))

# 2. README.md -> docs/img/... references
md = (ROOT / "README.md").read_text(encoding="utf-8")
for m in re.finditer(r'\(docs/img/([^)]+)\)', md):
    ref = "docs/img/" + m.group(1)
    # the README may link to a .png that has no .svg variant; accept png/svg
    for ext in (m.group(0).split(".")[-1],):
        pass
    if not (ROOT / ref).exists():
        missing.append(("README.md", ref, ROOT / ref))

if missing:
    print("MISSING ASSETS:")
    for where, ref, _ in missing:
        print(f"  {where}: {ref}")
    sys.exit(1)
print("All referenced assets exist.")
