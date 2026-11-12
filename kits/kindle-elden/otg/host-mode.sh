#!/bin/sh
set -eu

name=11210000.xhci
device=/sys/bus/platform/devices/$name
driver=/sys/bus/platform/drivers/xhci-mtk
role=/sys/class/extcon/extcon0/state
module=elden_kindle_otg
state=/tmp/elden-host-mode

status() {
    cat "$role"
    if [ -L "$driver/$name" ]; then
        printf '%s\n' 'MediaTek xHCI driver bound'
    fi
    for usb in /sys/bus/usb/devices/*; do
        [ -r "$usb/idVendor" ] || continue
        printf '%s %s:%s\n' "${usb##*/}" "$(cat "$usb/idVendor")" "$(cat "$usb/idProduct")"
    done
}

stop() {
    if [ -L "$driver/$name" ]; then printf '%s\n' "$name" >"$driver/unbind"; fi
    printf '\n' >"$device/driver_override"
    if [ -f "$state/power" ]; then
        cat "$state/power" >"$device/power/control"
        rm "$state/power"
    fi
    if [ -d "/sys/module/$module" ]; then rmmod "$module"; fi
}

start() {
    [ -f "$1" ]
    [ ! -L "$device/driver" ] && [ ! -d "/sys/module/$module" ]
    case "$(cat "$device/driver_override")" in '(null)'|'') ;; *) return 1 ;; esac
    grep -qx 'USB=0' "$role" || {
        printf '%s\n' 'Disconnect USB input power before starting host mode' >&2
        return 1
    }

    trap 'stop; rmdir "$state/lock"' EXIT
    insmod "$1" enable_host=1
    sleep 1
    grep -qx 'USB-HOST=1' "$role"
    grep -qx 'USB=0' "$role"
    cat "$device/power/control" >"$state/power"
    printf 'xhci-mtk\n' >"$device/driver_override"
    printf '%s\n' "$name" >"$driver/bind"
    printf 'on\n' >"$device/power/control"
    set -- "$device"/usb*
    [ -r "$1/idVendor" ]
    trap 'rmdir "$state/lock"' EXIT
    status
}

[ "$(uname -r)" = 5.15.41-lab126 ]
[ "$(tr '\000' '\n' <"$device/of_node/compatible")" = mediatek,mt8512-xhci ]

case "${1:-status}" in
    status) status; exit ;;
    start|stop) ;;
    *) printf 'Usage: %s {status|start MODULE.ko|stop}\n' "$0" >&2; exit 2 ;;
esac

umask 077
mkdir -p "$state"
mkdir "$state/lock"
trap 'rmdir "$state/lock"' EXIT
trap 'exit 1' HUP INT TERM

case "$1" in
    start) start "${2:?Pass the path to the host module}" ;;
    stop)
        [ -d "/sys/module/$module" ]
        [ "$(cat "$device/driver_override")" = xhci-mtk ]
        stop
        ;;
esac
