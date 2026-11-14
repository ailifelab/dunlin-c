#!/bin/sh
# Xcode invokes only the C/services build and bundle resources, never SwiftPM,
# launching engines, signing, uploading, or accessing user/model state.
set -eu
cd "$(dirname "$0")/.."
case "${1:-}" in
  core)
    case "${CONFIGURATION:-}" in Debug|Release) ;; *) exit 2;; esac
    export DUNLIN_CORE_BUILD_DIR="build/desktop/xcode-26-$CONFIGURATION"
    sh scripts/build-macos-core.sh
    ;;
  resources)
    test -n "${TARGET_BUILD_DIR:-}"
    test -n "${UNLOCALIZED_RESOURCES_FOLDER_PATH:-}"
    resources="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH"
    mkdir -p "$resources/ThirdParty"
    mkdir -p "$resources/ContextStrategies/extractive"
    cp compat/context/extractive/dunlin.json compat/context/extractive/strategy.py "$resources/ContextStrategies/extractive/"
    cp dunlin-copilot/assets/DunlinCopilot.icns "$resources/"
    cp vendor/libyaml/License "$resources/ThirdParty/libyaml-MIT.txt"
    cp build/native-deps/macos26/licenses/json-c.txt "$resources/ThirdParty/json-c-MIT.txt"
    cp build/native-deps/macos26/licenses/openssl.txt "$resources/ThirdParty/OpenSSL-Apache-2.0.txt"
    cp build/native-deps/media-current/COPYING "$resources/ThirdParty/curl.txt"
    # Optional compatibility assets are build inputs, not an installed engine.
    if [ ! -f build/v51/dsh-toolchain-v2/node_modules/esbuild/package.json ] || ! cmp -s hosts/node-dsh/toolchain-lock.json build/v51/dsh-toolchain-v2/package-lock.json; then
      mkdir -p build/v51/dsh-toolchain-v2
      cp hosts/node-dsh/toolchain-package.json build/v51/dsh-toolchain-v2/package.json
      cp hosts/node-dsh/toolchain-lock.json build/v51/dsh-toolchain-v2/package-lock.json
      npm ci --prefix build/v51/dsh-toolchain-v2 --ignore-scripts --no-audit --no-fund --userconfig /dev/null --cache build/v51/dsh-dependency-cache
    fi
    python3 -B scripts/prepare-dsh-assets.py "build/v51/xcode-assets-$CONFIGURATION"
    rm -rf "$resources/ExtensionAssets.next"
    cp -R "build/v51/xcode-assets-$CONFIGURATION" "$resources/ExtensionAssets.next"
    rm -rf "$resources/ExtensionAssets"
    mv "$resources/ExtensionAssets.next" "$resources/ExtensionAssets"
    ;;
  *) exit 2;;
esac
