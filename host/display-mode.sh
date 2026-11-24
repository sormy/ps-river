#!/usr/bin/env bash
# Sets the virtual monitor to a client's resolution for a stream (Sunshine runs it before an app
# starts, with the client's size), and back to 1920x1080 without arguments (after it ends). A
# size the monitor does not have falls back to 1920x1080. It never fails the stream: what goes
# wrong is logged to /tmp/ps-river-display-mode.log and the monitor stays as it is.
# Usage: display-mode.sh [width height]

readonly DEFAULT_MODE=1920x1080
readonly LOG=/tmp/ps-river-display-mode.log

export DISPLAY=${DISPLAY:-:1}
{
    output=$(xrandr | awk '/ connected/ { print $1; exit }')
    mode=${1:+$1x$2}
    xrandr --output "$output" --mode "${mode:-$DEFAULT_MODE}" ||
        xrandr --output "$output" --mode "$DEFAULT_MODE"
} >>"$LOG" 2>&1 || echo "$(date): display-mode $* failed (DISPLAY=$DISPLAY)" >>"$LOG"
exit 0
