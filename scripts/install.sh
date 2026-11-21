#!/bin/bash
# Install Vial module to Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/vial" ]; then
    echo "Error: dist/vial not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing Vial Module ==="

# Deploy to Move (clean old patches first so removed/filtered files don't linger)
echo "Copying module to Move..."
ssh ableton@move.local "mkdir -p /data/UserData/schwung/modules/sound_generators/vial && rm -rf /data/UserData/schwung/modules/sound_generators/vial/vial-data/patches"
scp -r dist/vial/* ableton@move.local:/data/UserData/schwung/modules/sound_generators/vial/

# Install chain presets if they exist
if [ -d "src/chain_patches" ]; then
    echo "Installing chain presets..."
    scp src/chain_patches/*.json ableton@move.local:/data/UserData/schwung/patches/
fi

# Set permissions
echo "Setting permissions..."
ssh ableton@move.local "chmod -R a+rw /data/UserData/schwung/modules/sound_generators/vial"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: /data/UserData/schwung/modules/sound_generators/vial/"
echo ""
echo "Restart Move Anything to load the new module."
