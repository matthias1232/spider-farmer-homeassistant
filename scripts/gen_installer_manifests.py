#!/usr/bin/env python3
"""Generate the installer's module list and the auto-detect manifests.

CI writes one ESP Web Tools manifest per firmware and target
(`manifest-<firmware>-<target>.json`). This script runs in the publish job, after
those files and the .bin images have been copied into the site directory, and adds:

  * `manifest-<firmware>.json`  - every build of one firmware in one manifest.
    ESP Web Tools reads the chip family from the connected board and picks the
    matching build, which is the "auto-detect" choice in the installer.
  * `modules.json`              - the list the installer page turns into its
    module select box. It is built from firmware.json and from the manifests
    that really exist, so a target only shows up once it has been built.

Usage (from the repository root):

    python3 scripts/gen_installer_manifests.py [SITE_DIR]      # default: site
"""

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# ESP Web Tools chip family for each IDF target (must match what the
# esp-web-tools dialog reports, see FAMILY in .github/workflows/build.yml).
FAMILY = {
    "esp32": "ESP32",
    "esp32s2": "ESP32-S2",
    "esp32s3": "ESP32-S3",
    "esp32c2": "ESP32-C2",
    "esp32c3": "ESP32-C3",
    "esp32c5": "ESP32-C5",
    "esp32c6": "ESP32-C6",
    "esp32h2": "ESP32-H2",
    "esp32p4": "ESP32-P4",
}


def load(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def main() -> int:
    site = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "site"
    firmwares = load(ROOT / "firmware.json")
    out = {"firmwares": []}
    errors = []

    for fw in firmwares:
        modules = []
        merged_builds = []
        name = fw.get("name", fw["id"])
        version = ""
        for target in fw["targets"]:
            mpath = site / ("manifest-%s-%s.json" % (fw["id"], target))
            if not mpath.exists():
                # Not built (failed or skipped): do not offer it.
                errors.append("%s/%s: %s missing, not offered" % (fw["id"], target, mpath.name))
                continue
            manifest = load(mpath)
            for build in manifest.get("builds", []):
                for part in build.get("parts", []):
                    if not (site / part["path"]).exists():
                        errors.append("%s/%s: image %s missing" % (fw["id"], target, part["path"]))
                        break
                else:
                    merged_builds.append(build)
                    version = manifest.get("version", version)
                    info = (fw.get("modules") or {}).get(target, {})
                    modules.append({
                        "target": target,
                        "family": build["chipFamily"],
                        "label": info.get("label") or "%s (%s)" % (FAMILY.get(target, target), target),
                        "tested": info.get("tested", ""),
                        "manifest": mpath.name,
                    })

        if not modules:
            continue

        auto = {
            "name": name,
            "version": version,
            "new_install_prompt_erase": True,
            "builds": merged_builds,
        }
        auto_name = "manifest-%s.json" % fw["id"]
        (site / auto_name).write_text(json.dumps(auto, indent=2) + "\n", encoding="utf-8")
        out["firmwares"].append({
            "id": fw["id"],
            "name": name,
            "version": version,
            "auto": auto_name,
            "modules": modules,
        })

    if not out["firmwares"]:
        print("ERROR: no installable firmware found in %s" % site, file=sys.stderr)
        for e in errors:
            print("  " + e, file=sys.stderr)
        return 1

    (site / "modules.json").write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    for e in errors:
        print("WARNING: " + e, file=sys.stderr)
    n = sum(len(f["modules"]) for f in out["firmwares"])
    print("OK: modules.json with %d module(s) in %d firmware(s)" % (n, len(out["firmwares"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
