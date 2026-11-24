#!/bin/sh
# Builds 3dsmaps.cia from 3dsmaps.elf (run `make` first).
# Needs makerom and bannertool on PATH, or set MAKEROM / BANNERTOOL.
set -e
cd "$(dirname "$0")/.."

MAKEROM=${MAKEROM:-makerom}
BANNERTOOL=${BANNERTOOL:-bannertool}
TITLE="3DS Maps"
DESC="OpenStreetMap viewer with phone GPS"
AUTHOR="3DS MAKE"

"$BANNERTOOL" makebanner -i cia/banner.png -a cia/audio.wav -o cia/banner.bnr
"$BANNERTOOL" makesmdh -s "$TITLE" -l "$DESC" -p "$AUTHOR" -i cia/icon.png -o cia/icon.icn

"$MAKEROM" -f cia -o 3dsmaps.cia -target t -exefslogo \
	-elf 3dsmaps.elf -rsf cia/app.rsf \
	-icon cia/icon.icn -banner cia/banner.bnr \
	-DAPP_TITLE=3dsmaps -DAPP_PRODUCT_CODE=CTR-H-3DSM \
	-DAPP_UNIQUE_ID=0xF3D5A -DAPP_ROMFS=romfs

ls -l 3dsmaps.cia
