#!/bin/sh
# Prints the largest RAM symbols of the final image (used when fitting into 64 KB).
DIR=$(dirname "$0")
NM=$(find /root/ncs/toolchains /opt -name arm-zephyr-eabi-nm 2>/dev/null | head -1)
echo RAMTOP_BEGIN
"$NM" -S --size-sort -r -C "$DIR/build/zephyr/remapper.elf" | grep -i ' [bBdD] ' | head -60
echo RAMTOP_END
