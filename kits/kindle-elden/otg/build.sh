#!/bin/sh
set -eu

if [ "$#" -ne 4 ]; then
    printf 'Usage: %s KERNEL_TREE LIVE_CONFIG STOCK_MODULE OUTPUT_DIR\n' "$0" >&2
    exit 2
fi

source_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
kernel=$(realpath "$1")
config=$(realpath "$2")
stock_module=$(realpath "$3")
mkdir -p "$4"
output=$(realpath "$4")
cross=${CROSS_COMPILE:-arm-linux-gnueabihf-}
jobs=${JOBS:-8}
objects='drivers/extcon/extcon.o kernel/params.o kernel/module.o drivers/base/bus.o
drivers/base/core.o drivers/base/platform.o drivers/clk/clk.o drivers/clk/clkdev.o'

kbuild() {
    make -C "$kernel" ARCH=arm CROSS_COMPILE="$cross" O="$output/kernel" -j"$jobs" "$@"
}

mkdir -p "$output/kernel" "$output/module"
cp "$config" "$output/kernel/.config"
cp "$source_dir/Makefile" "$source_dir/elden_kindle_otg.c" "$output/module/"
kbuild olddefconfig modules_prepare
# Object names are fixed above and intentionally split into separate arguments.
kbuild $objects

for object in $objects; do
    "${cross}nm" "$output/kernel/$object" |
        awk '$2 == "A" && $3 ~ /^__crc_/ {
            sub(/^__crc_/, "", $3)
            printf "0x%s\t%s\tvmlinux\tEXPORT_SYMBOL\t\n", $1, $3
        }'
done >"$output/module/exports.symvers"

modprobe --show-modversions "$stock_module" >"$output/stock-versions"
layout=$(awk '$2 == "module_layout" {print $1}' "$output/stock-versions")
test -n "$layout"
awk -v version="$layout" '$2 == "module_layout" && $1 == version {found=1} END {exit !found}' \
    "$output/module/exports.symvers"
awk '$2 == "__aeabi_unwind_cpp_pr0" {
    printf "%s\t%s\tvmlinux\tEXPORT_SYMBOL\t\n", $1, $2
    found=1
} END {exit !found}' "$output/stock-versions" >>"$output/module/exports.symvers"

kbuild M="$output/module" KBUILD_EXTRA_SYMBOLS="$output/module/exports.symvers" modules
module="$output/module/elden_kindle_otg.ko"
[ "$(modinfo -F vermagic "$module")" = "$(modinfo -F vermagic "$stock_module")" ]
modprobe --show-modversions "$module"
printf '%s\n' "$module"
