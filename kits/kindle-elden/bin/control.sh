#!/bin/sh
set -eu
umask 077

bin=$(CDPATH= cd "$(dirname "$0")" && pwd)
app=$(dirname "$bin")
state=/var/local/elden-tools
interface=wg-elden
lock=/tmp/elden-tools.lock
product=elden_prc_wifi
backup_size=100663296
backup_hash=29a3171cffc97b4e7c073bbeb00fc7cb48223bc3611594224b723fbdf51e7070
backup="$state/boot_b-linux.img"
config="$state/wireguard.conf"
pidfile="$state/collector.pid"

fail() {
    printf '%s\n' "$*" >&2
    exit 1
}

fastboot() {
    seconds=$1
    shift
    timeout "$seconds" "$bin/fastboot" "$@"
}

getvar() {
    result=$(fastboot 12 -s "$serial" getvar "$1" 2>&1) || fail "$result"
    value=$(printf '%s\n' "$result" | awk -v prefix="$1:" '
        { sub(/\r$/, ""); sub(/^\(bootloader\)[[:space:]]+/, "") }
        index($0, prefix) == 1 { sub(/^[^ ]+[[:space:]]+/, ""); print; exit }
    ')
    [ -n "$value" ] || fail "Missing fastboot variable $1"
    printf '%s\n' "$value"
}

select_tablet() {
    devices=$(fastboot 8 devices) || fail "Fastboot enumeration failed"
    serial=$(printf '%s\n' "$devices" | awk '$2 == "fastboot" { print $1 }')
    case "$serial" in
        ''|*[!a-zA-Z0-9_.-]*) fail "Expected exactly one USB fastboot device" ;;
    esac
    connected=$(getvar product) || exit 1
    [ "$connected" = "$product" ] || fail "Unexpected fastboot product $connected"
    mode=$(getvar is-userspace) || exit 1
    [ "$mode" = no ] || fail "Enter bootloader fastboot on the tablet"
}

show_slot() {
    active=$(getvar current-slot) || exit 1
    case "$active" in a|b) printf 'Active tablet slot %s\n' "$active" ;; *) fail "Unexpected slot $active" ;; esac
}

activate() {
    select_tablet
    fastboot 30 -s "$serial" set_active "$1" || fail "Fastboot slot change failed"
    active=$(getvar current-slot) || exit 1
    [ "$active" = "$1" ] || fail "Bootloader reports slot $active after selecting $1"
    printf 'Active tablet slot %s\n' "$active"
}

restore_linux() {
    [ -f "$backup" ] || fail "Missing Linux backup at $backup"
    [ "$(wc -c < "$backup")" -eq "$backup_size" ] || fail "Linux backup size differs"
    checksum=$(sha256sum "$backup") || exit 1
    [ "${checksum%% *}" = "$backup_hash" ] || fail "Linux backup checksum differs"
    select_tablet
    size=$(getvar partition-size:boot_b) || exit 1
    case "$size" in 0x*|0X*) hex=${size#??} ;; *) fail "Invalid boot_b partition size" ;; esac
    case "$hex" in ''|*[!a-fA-F0-9]*) fail "Invalid boot_b partition size" ;; esac
    [ "${#hex}" -le 16 ] && [ "$((0x$hex))" -eq "$backup_size" ] ||
        fail "The boot_b partition is not 96 MiB"
    fastboot 180 -s "$serial" flash boot_b "$backup" || fail "Fastboot restore failed"
    printf '%s\n' "Linux backup restored to tablet boot_b"
    show_slot
}

wg() {
    "$bin/wg-lib/ld-musl-armhf.so.1" --library-path "$bin/wg-lib" "$bin/wg-arm" "$@"
}

log_rule() {
    iptables "$@" -i "$interface" -s 10.8.0.1/32 -d 10.8.0.8/32 \
        -p tcp --dport 8734 -j ACCEPT
}

allow_log_reader() {
    if ! log_rule -C INPUT 2>/dev/null; then
        log_rule -I INPUT 1 || return 1
        touch "$state/firewall-owned"
    fi
}

vpn_up() {
    if ip link show dev "$interface" >/dev/null 2>&1; then
        wg show "$interface" >/dev/null && allow_log_reader
        return
    fi
    [ -s "$config" ] || return 1
    address=$(awk -F= '/^[[:space:]]*Address[[:space:]]*=/ {gsub(/[[:space:]]/, "", $2); print $2; exit}' "$config")
    [ "${address%%,*}" = 10.8.0.8/32 ] || return 1
    peer_config=$(mktemp "$state/peer.XXXXXX")
    awk '/^[[:space:]]*(Address|DNS|MTU)[[:space:]]*=/ {next} {print}' "$config" >"$peer_config"
    if ! ip link add "$interface" type wireguard 2>/dev/null && ! "$bin/wireguard-go" "$interface"; then
        rm -f "$peer_config"
        return 1
    fi
    if ! wg setconf "$interface" "$peer_config" ||
        ! ip addr add 10.8.0.8/32 dev "$interface" ||
        ! ip link set dev "$interface" mtu 1420 up ||
        ! ip route add 10.8.0.0/24 dev "$interface"; then
        rm -f "$peer_config"
        ip link del "$interface"
        return 1
    fi
    rm -f "$peer_config"
    allow_log_reader || return 1
    printf '%s\n' "WireGuard interface up at 10.8.0.8"
}

collector_pid() {
    [ -f "$pidfile" ] || return 1
    pid=$(cat "$pidfile") || return 1
    case "$pid" in ''|*[!0-9]*) return 1 ;; esac
    [ "$(readlink "/proc/$pid/exe" 2>/dev/null)" = "$bin/collector" ] || return 1
    printf '%s\n' "$pid"
}

start() {
    if ! collector_pid >/dev/null; then
        nohup "$bin/collector" -directory "$state/logs" -usb-reader "$bin/usb-cdc" \
            </dev/null >>"$state/collector.log" 2>&1 &
        printf '%s\n' "$!" >"$pidfile"
        sleep 1
        collector_pid >/dev/null || fail "Collector failed to start. Check $state/collector.log"
    fi
    printf '%s\n' "Boot-log collector running"
    vpn_up || fail "WireGuard unavailable. USB capture remains active"
}

stop() {
    if pid=$(collector_pid); then kill "$pid"; fi
    rm -f "$pidfile"
    if [ -f "$state/firewall-owned" ]; then
        if log_rule -C INPUT 2>/dev/null; then log_rule -D INPUT; fi
        rm -f "$state/firewall-owned"
    fi
    if ip link show dev "$interface" >/dev/null 2>&1; then ip link del "$interface"; fi
    printf '%s\n' "Collector and WireGuard stopped"
}

status() {
    if collector_pid >/dev/null; then printf '%s\n' "Collector running"; fi
    if [ -c /dev/net/tun ]; then printf '%s\n' "TUN present"; fi
    if [ -s "$config" ]; then printf '%s\n' "Private WireGuard profile installed"; fi
    if [ -s "$backup" ]; then printf '%s\n' "Linux backup staged"; fi
    if ip link show dev "$interface" >/dev/null 2>&1; then
        ip addr show dev "$interface"
        wg show "$interface" latest-handshakes | awk '{print "WireGuard latest handshake " $2}'
    fi
    for role in /sys/class/usb_role/*/role; do
        [ -r "$role" ] || continue
        printf 'USB role '
        cat "$role"
    done
    for bus in /sys/bus/usb/devices/usb*; do
        [ -d "$bus" ] || continue
        printf 'USB host bus %s\n' "${bus##*/}"
    done
    (select_tablet; show_slot) || true
}

case "${1:-status}" in
    start|stop|status|activate-a|activate-b|restore-linux|reboot) action=${1:-status} ;;
    *) fail "Usage: $0 {status|start|stop|activate-a|activate-b|restore-linux|reboot}" ;;
esac
[ "$(id -u)" -eq 0 ] || fail "Run this toolkit as Kindle root"
mkdir -p "$state"
chmod 700 "$state"
mkdir "$lock" 2>/dev/null || fail "An Elden action is already running"
trap 'rmdir "$lock"' EXIT
trap 'exit 1' HUP INT TERM

set +e
(
    set -e
    printf '%s %s\n' "$(date -u +%FT%TZ)" "$action"
    case "$action" in
        start) start ;;
        stop) stop ;;
        status) status ;;
        activate-a) activate a ;;
        activate-b) activate b ;;
        restore-linux) restore_linux ;;
        reboot) select_tablet; fastboot 30 -s "$serial" reboot ;;
    esac
) >"$state/action.log" 2>&1
result=$?
set -e
if [ "$result" -ne 0 ]; then printf 'Exit status %s\n' "$result" >>"$state/action.log"; fi
cat "$state/action.log"
cp "$state/action.log" "$app/status.txt"
exit "$result"
