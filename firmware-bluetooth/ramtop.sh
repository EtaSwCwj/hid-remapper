#!/bin/sh
# Runs after the final link.
# - prints the largest RAM symbols (used when fitting into 64 KB)
# - the CI workflow uploads build/zephyr/remapper.uf2; boards without a UF2
#   bootloader get their .hex under that name so the artifact still exists
DIR=$(dirname "$0")
NM=$(find /root/ncs/toolchains /opt -name arm-zephyr-eabi-nm 2>/dev/null | head -1)
echo RAMTOP_BEGIN
"$NM" -S --size-sort -r -C "$DIR/build/zephyr/remapper.elf" | grep -i ' [bBdD] ' | head -40
echo RAMTOP_END
if [ ! -f "$DIR/build/zephyr/remapper.uf2" ] && [ -f "$DIR/build/zephyr/remapper.hex" ]; then
    cp "$DIR/build/zephyr/remapper.hex" "$DIR/build/zephyr/remapper.uf2"
    echo "copied remapper.hex to remapper.uf2 (this artifact is an Intel HEX file)"
fi
