#!/bin/sh
set -eu
umask 077

state=/var/local/elden-tools
case "${1:-}" in
    wireguard) target=wireguard.conf ;;
    backup) target=boot_b-linux.img ;;
    *) printf 'Usage: %s {wireguard|backup} < file\n' "$0" >&2; exit 2 ;;
esac
[ "$(id -u)" -eq 0 ]
mkdir -p "$state"
chmod 700 "$state"
temporary=$(mktemp "$state/staging.XXXXXX")
trap 'rm -f "$temporary"' EXIT
trap 'exit 1' HUP INT TERM
cat >"$temporary"
[ -s "$temporary" ]
if [ "$target" = boot_b-linux.img ]; then
    checksum=$(sha256sum "$temporary")
    if [ "${checksum%% *}" != 29a3171cffc97b4e7c073bbeb00fc7cb48223bc3611594224b723fbdf51e7070 ]; then
        printf '%s\n' "Linux backup checksum differs" >&2
        exit 1
    fi
fi
chmod 600 "$temporary"
mv "$temporary" "$state/$target"
printf 'Staged %s\n' "$target"
