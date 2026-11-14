#!/bin/sh
# Run each display server and accessibility bus inside the invoking user's session.
set -eu
binary=${1:?usage: run-displays.sh /path/to/dunlin-copilot}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
runtime=$(mktemp -d)
chmod 700 "$runtime"
export XDG_RUNTIME_DIR="$runtime"
export PYTHONIOENCODING=utf-8
pid=
xorg_pid=
finish() {
    if [ -n "$pid" ]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
    if [ -n "$xorg_pid" ]; then kill "$xorg_pid" 2>/dev/null || true; wait "$xorg_pid" 2>/dev/null || true; fi
    rm -rf "$runtime"
}
trap finish EXIT INT TERM
if [ "${DUNLIN_XSERVER:-xvfb}" = xorg ]; then
    Xorg -config "$here/xorg-headless.conf" -displayfd 3 -noreset -nolisten tcp -logfile "$runtime/xorg.log" 3>"$runtime/display" >"$runtime/xorg-output" 2>&1 &
    pid=$!
    n=0
    while [ ! -s "$runtime/display" ]; do
        if ! kill -0 "$pid" 2>/dev/null || [ "$n" -gt 100 ]; then cat "$runtime/xorg-output" "$runtime/xorg.log" >&2; exit 1; fi
        n=$((n + 1)); sleep .1
    done
    export DISPLAY=":$(cat "$runtime/display")"
    GDK_BACKEND=x11 dbus-run-session -- python3 "$here/native_backend.py" "$binary"
    xorg_pid=$pid; pid=
else
    GDK_BACKEND=x11 xvfb-run -a -s '-screen 0 1280x900x24' dbus-run-session -- python3 "$here/native_backend.py" "$binary"
fi
# A nested Weston provides a keyboard/pointer seat as well as real pixman
# rendering on the dummy Xorg output. GTK still connects only over Wayland.
weston_backend=headless-backend.so
if [ -n "$xorg_pid" ]; then weston_backend=x11-backend.so; fi
weston --backend="$weston_backend" --use-pixman --socket=dunlin-test --width=1280 --height=900 --idle-time=0 --no-config >"$runtime/weston.log" 2>&1 &
pid=$!
n=0
while [ ! -S "$runtime/dunlin-test" ]; do
    if ! kill -0 "$pid" 2>/dev/null || [ "$n" -gt 100 ]; then cat "$runtime/weston.log" >&2; exit 1; fi
    n=$((n + 1))
    sleep .1
done
GDK_BACKEND=wayland WAYLAND_DISPLAY=dunlin-test dbus-run-session -- python3 "$here/native_backend.py" "$binary"
