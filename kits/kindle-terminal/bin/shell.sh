#!/bin/sh
set -eu

# Override the terminfo path compiled into the upstream Kindle binary.
app=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
export TERM=xterm
export TERMINFO="$app/vte/terminfo"
exec /bin/sh -i
