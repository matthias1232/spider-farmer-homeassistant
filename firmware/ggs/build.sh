#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Builds SpiderBridge-ESP32 v2 for ESP32-WROOM-32 and produces a flashable
# factory image.
#
# Uses a locally installed ESP-IDF (default: /opt/esp/esp-idf).
#
# Usage:
#   ./build.sh            # version 1.0.0
#   ./build.sh 1.2.0       # explicit version
# ---------------------------------------------------------------------------
set -euo pipefail

VERSION="${1:-1.0.0}"

IDF_PATH_DEFAULT="/opt/esp/esp-idf"
IDF_DIR="${IDF_PATH:-$IDF_PATH_DEFAULT}"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="$PROJECT_DIR/dist"

if [ ! -f "$IDF_DIR/export.sh" ]; then
    echo "ESP-IDF not found at $IDF_DIR" >&2
    echo "Override with IDF_PATH=/path/to/esp-idf ./build.sh" >&2
    exit 1
fi

# shellcheck disable=SC1091
. "$IDF_DIR/export.sh" >/dev/null 2>&1

mkdir -p "$DIST_DIR"
cd "$PROJECT_DIR"

build_dir="build/esp32"

idf.py -B "$build_dir" set-target esp32
idf.py -B "$build_dir" -DPROJECT_VER="$VERSION" build

echo ""
echo "=== Creating the factory image ==="

out_name="spiderbridge-esp32-v2-factory.bin"

# merge_bin combines bootloader, partition table, OTA data area and app
# into ONE image with correct internal offsets, so it can be flashed at 0x0.
esptool.py --chip esp32 merge_bin \
    -o "dist/$out_name" \
    --flash_mode dio \
    --flash_size 4MB \
    0x1000  "$build_dir/bootloader/bootloader.bin" \
    0x8000  "$build_dir/partition_table/partition-table.bin" \
    0xf000  "$build_dir/ota_data_initial.bin" \
    0x20000 "$build_dir/spiderbridge_esp32_v2.bin"

size_bytes=$(stat -c%s "$DIST_DIR/$out_name")
echo "$out_name created ($((size_bytes / 1024)) KB)"

echo ""
echo "=== Done ==="
ls -lh "$DIST_DIR"
echo ""
echo "Flash with (BOOT button held if auto-reset does not work):"
echo "  esptool.py --chip esp32 --port COM3 write-flash 0x0 dist/$out_name"
