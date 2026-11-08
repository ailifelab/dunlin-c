#!/bin/sh
# Prepare official pinned embed port. No serial/device/network side effects.
set -eu
cd "$(dirname "$0")/.."
python3 -B scripts/generate-device-config.py
root="$PWD"
vendor="$root/build/vendor/micropython-1.26.1"
component="$root/dunlin-copilot/esp32/esp32/components/micropython"
test -f "$vendor/ports/embed/embed.mk"
cat > "$component/embed.mk" <<'MAKE'
MICROPYTHON_TOP = ../../../../../build/vendor/micropython-1.26.1
SRC_QSTR += $(MICROPYTHON_TOP)/extmod/modjson.c ../../../python/mpbridge.c
include $(MICROPYTHON_TOP)/ports/embed/embed.mk
MAKE
(cd "$component" && make -f embed.mk PACKAGE_DIR=generated)
cp "$vendor/extmod/modjson.c" "$component/generated/extmod/modjson.c"
cp "$vendor/LICENSE" "$component/LICENSE-MicroPython"
