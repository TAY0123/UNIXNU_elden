#!/bin/sh
set -eu

app=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
export DISPLAY=${DISPLAY:-:0}
export TERM=xterm
export TERMINFO="$app/vte/terminfo"
cd /mnt/us

exec >"$app/launch.log" 2>&1
printf '%s\n' "Starting kTerm"
status=0
"$app/bin/kterm" -k 1 -c 0 -f Monospace -s 9 -u B \
    -l "$app/layouts/keyboard.xml" -e "/bin/sh $app/bin/shell.sh" || status=$?
printf 'kTerm exit status %s\n' "$status"
exit "$status"
