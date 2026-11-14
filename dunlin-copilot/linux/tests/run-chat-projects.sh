#!/bin/sh
# Use a built Dunlin desktop library and the caller's existing X11 or Wayland display.
set -eu
build=$(CDPATH= cd -- "${1:?supply the Linux core build directory}" && pwd)
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
linux=$(dirname "$here")
root=$(dirname "$(dirname "$linux")")
out=$(mktemp -d)
trap 'rm -rf -- "$out"' EXIT
# Use the pinned json-c types and the core's exported json-c runtime together.
${CC:-cc} -D_GNU_SOURCE -std=c11 -O2 -Wall -Wextra -Werror -I"$root/include" -I"$linux" -I"$build/deps/include" \
    $(pkg-config --cflags gtk+-3.0 libsecret-1 libxml-2.0) \
    "$linux/chat.c" "$linux/rich_message.c" "$linux/activity.c" "$linux/ui.c" "$linux/settings.c" "$here/chat_projects.c" \
    -L"$build/lib" -l:libdunlin-desktop.so.1 -Wl,-rpath,"$build/lib" \
    $(pkg-config --libs gtk+-3.0 libsecret-1 libxml-2.0) -o "$out/chat-projects"
python3 "$here/chat_projects.py" "$out/chat-projects"
