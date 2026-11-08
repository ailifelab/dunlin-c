#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export MACOSX_DEPLOYMENT_TARGET=26.0
native_deps="$(python3 -B scripts/build-macos-deps.py)"
media_deps="$(python3 -B scripts/build-media-curl.py)"
if [ -e build/native-deps/media-current ] && [ ! -L build/native-deps/media-current ]; then
  printf '%s\n' 'Refusing to replace an unmanaged media dependency directory' >&2; exit 2
fi
ln -sfn "$(basename "$media_deps")" build/native-deps/media-current
core_build="${DUNLIN_CORE_BUILD_DIR:-build/desktop/native-26-source}"
case "$core_build" in build/desktop/*) ;; *) printf '%s\n' 'Core build output must stay under build/desktop' >&2; exit 2;; esac
make BUILD="$core_build" JSON_C_PREFIX="$native_deps" CFLAGS='-O2 -g -std=c11 -Wall -Wextra -Wpedantic -mmacosx-version-min=26.0' "$core_build/libdunlin.a" "$core_build/libdunlin-kernel.a"
mkdir -p "$core_build/objects-v51"
set --
for source in desktop/*.c modules/*/*.c compat/dsh/*.c vendor/libyaml/src/*.c; do
  object="$core_build/objects-v51/$(printf '%s' "$source" | tr / _)o"
  cc -O2 -g -std=c11 -mmacosx-version-min=26.0 -D_DARWIN_C_SOURCE -DDUNLIN_WITH_DSH -Wall -Wextra -Iinclude -Ivendor/libyaml/include -include modules/config_yaml/build_config.h -I"$native_deps/include" -I"$media_deps/include" -c "$source" -o "$object"
  set -- "$@" "$object"
done
ar rcs "$core_build/libdunlin-desktop.next.a" "$@"
mv -f "$core_build/libdunlin-desktop.next.a" "$core_build/libdunlin-desktop.a"
