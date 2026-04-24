#!/usr/bin/env bash
#
# Release script for MicroPython LoRaWAN firmware.
#
# Usage:
#   ./scripts/release.sh v1.0.0-lorawan
#
# What it does:
#   1. Checks that the build output exists
#   2. Tags the current commit (if not already tagged)
#   3. Copies binaries to the gh-pages branch (web flasher)
#   4. Creates a GitHub Release with the binaries attached
#
# Requirements: git, gh (GitHub CLI, authenticated)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/ports/esp32/build-LILYGO_TTGO_TBEAM"

# --- Check arguments ---
VERSION="${1:-}"
if [ -z "$VERSION" ]; then
    echo "Usage: $0 <version-tag>"
    echo "Example: $0 v1.0.0-lorawan"
    exit 1
fi

# --- Check build output ---
BOOTLOADER="$BUILD_DIR/bootloader/bootloader.bin"
PARTITION="$BUILD_DIR/partition_table/partition-table.bin"
FIRMWARE="$BUILD_DIR/micropython.bin"

for f in "$BOOTLOADER" "$PARTITION" "$FIRMWARE"; do
    if [ ! -f "$f" ]; then
        echo "Error: $f not found. Run a build first."
        exit 1
    fi
done

echo "Firmware size: $(du -h "$FIRMWARE" | cut -f1)"

# --- Get current branch ---
CURRENT_BRANCH="$(git -C "$REPO_ROOT" rev-parse --abbrev-ref HEAD)"

# --- Tag (annotated) if it doesn't exist yet ---
if git -C "$REPO_ROOT" rev-parse "$VERSION" >/dev/null 2>&1; then
    echo "Tag $VERSION already exists, skipping."
else
    echo "Creating tag $VERSION ..."
    git -C "$REPO_ROOT" tag -a "$VERSION" -m "Release $VERSION"
    git -C "$REPO_ROOT" push origin "$VERSION"
fi

# --- Update gh-pages branch ---
echo "Updating gh-pages branch ..."
git -C "$REPO_ROOT" checkout gh-pages

cp "$BOOTLOADER" "$REPO_ROOT/bootloader.bin"
cp "$PARTITION"   "$REPO_ROOT/partition-table.bin"
cp "$FIRMWARE"    "$REPO_ROOT/micropython.bin"

# Update version in manifest.json
sed -i.bak "s/\"version\": \".*\"/\"version\": \"$VERSION\"/" "$REPO_ROOT/manifest.json"
rm -f "$REPO_ROOT/manifest.json.bak"

git -C "$REPO_ROOT" add bootloader.bin partition-table.bin micropython.bin manifest.json
git -C "$REPO_ROOT" commit -m "Update firmware to $VERSION" || echo "No changes to commit."
git -C "$REPO_ROOT" push origin gh-pages

# --- Back to original branch ---
git -C "$REPO_ROOT" checkout "$CURRENT_BRANCH"

# --- Create GitHub Release ---
echo "Creating GitHub Release $VERSION ..."
gh release create "$VERSION" \
    --repo "$(git -C "$REPO_ROOT" remote get-url origin)" \
    --title "$VERSION" \
    --notes "MicroPython LoRaWAN firmware for LILYGO T-Beam (v0.7–v1.2).

Flash from browser: https://nunomcruz.github.io/micropython-lorawan/

Or manually with esptool:
\`\`\`
esptool.py --chip esp32 --baud 460800 write_flash \\
    --flash_mode dio --flash_size 4MB --flash_freq 80m \\
    0x1000 bootloader.bin \\
    0x8000 partition-table.bin \\
    0x10000 micropython.bin
\`\`\`" \
    "$BOOTLOADER#bootloader.bin" \
    "$PARTITION#partition-table.bin" \
    "$FIRMWARE#micropython.bin"

echo ""
echo "Done! Release $VERSION published."
echo "Web flasher: https://nunomcruz.github.io/micropython-lorawan/"
