#!/bin/bash
# setup-layers.sh — Clone Yocto layers needed to build this project
#
# Usage:
#   mkdir poky && cd poky
#   git clone git://git.yoctoproject.org/poky.git .
#   git checkout scarthgap
#   ../UART-sensor/setup-layers.sh
#
# After running this script, symlink or copy meta-rockchip from this repo:
#   ln -s /path/to/UART-sensor/meta-rockchip meta-rockchip

set -e

BRANCH="scarthgap"

echo "=== Cloning meta-openembedded (${BRANCH}) ==="
if [ ! -d "meta-openembedded" ]; then
    git clone -b "${BRANCH}" https://github.com/openembedded/meta-openembedded.git
else
    echo "meta-openembedded already exists, skipping."
fi

echo ""
echo "=== Done ==="
echo ""
echo "Next steps:"
echo "  1. Source the Yocto environment:"
echo "       source oe-init-build-env"
echo "  2. Add layers to bblayers.conf (or copy from this repo's build/conf/):"
echo "       bitbake-layers add-layer ../meta-openembedded/meta-oe"
echo "       bitbake-layers add-layer ../meta-openembedded/meta-python"
echo "       bitbake-layers add-layer ../meta-openembedded/meta-networking"
echo "       bitbake-layers add-layer /path/to/UART-sensor/meta-rockchip"
echo "  3. Copy or merge local.conf from this repo's build/conf/local.conf"
echo "  4. Build:"
echo "       bitbake core-image-minimal"

