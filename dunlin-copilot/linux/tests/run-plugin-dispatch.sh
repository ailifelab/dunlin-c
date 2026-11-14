#!/bin/sh
# Exercise an installed original workbench via the native GTK/WebKit surface and C bridge.
set -eu
build=$(CDPATH= cd -- "${1:?Linux core build directory}" && pwd)
assets=$(CDPATH= cd -- "${2:?prepared DSH ExtensionAssets directory}" && pwd)
node=${3:?absolute path to compatible Node}
package=${4:?extracted original dsh-project-manager 0.57.0 with an import manifest}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
linux=$(dirname "$here")
root=$(dirname "$(dirname "$linux")")
out=$(mktemp -d)
trap 'rm -rf -- "$out"' EXIT
webkit=webkit2gtk-4.1
if ! pkg-config --exists "$webkit"; then webkit=webkit2gtk-4.0; fi
cc -std=c11 -D_GNU_SOURCE -DDUNLIN_WITH_WEBKIT=1 -Wno-deprecated-declarations \
    -I"$root/include" -I"$linux" -I"$build/deps/include" $(pkg-config --cflags gtk+-3.0 libsecret-1 "$webkit") \
    "$linux/ui.c" "$linux/surface.c" "$linux/activity.c" "$linux/settings.c" "$here/plugin_dispatch.c" \
    -L"$build/lib" -l:libdunlin-desktop.so.1 -Wl,-rpath,"$build/lib" \
    $(pkg-config --libs gtk+-3.0 libsecret-1 "$webkit") -o "$out/workbench-test"
python3 "$here/plugin_dispatch.py" "$out/workbench-test" "$assets" "$node" "$package"
