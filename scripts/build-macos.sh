#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
# Keep deployment-target objects separate from earlier 27.2 development builds.
export MACOSX_DEPLOYMENT_TARGET=26.0
native_deps="$(python3 -B scripts/build-macos-deps.py)"
sh scripts/build-macos-core.sh
swift build --package-path dunlin-copilot/macos --scratch-path build/desktop/swift-26-source
app="dist/macos/Dunlin Copilot.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
mkdir -p "$app/Contents/Resources/ThirdParty"
cp dunlin-copilot/assets/DunlinCopilot.icns "$app/Contents/Resources/"
cp vendor/libyaml/License "$app/Contents/Resources/ThirdParty/libyaml-MIT.txt"
cp "$native_deps/licenses/json-c.txt" "$app/Contents/Resources/ThirdParty/json-c-MIT.txt"
cp "$native_deps/licenses/openssl.txt" "$app/Contents/Resources/ThirdParty/OpenSSL-Apache-2.0.txt"
cp build/native-deps/media-current/COPYING "$app/Contents/Resources/ThirdParty/curl.txt"
if [ ! -f build/v51/dsh-toolchain/node_modules/esbuild/package.json ]; then
  mkdir -p build/v51/dsh-toolchain
  cp hosts/node-dsh/toolchain-package.json build/v51/dsh-toolchain/package.json
  cp hosts/node-dsh/toolchain-lock.json build/v51/dsh-toolchain/package-lock.json
  npm ci --prefix build/v51/dsh-toolchain --ignore-scripts --no-audit --no-fund
fi
python3 -B scripts/prepare-dsh-assets.py
# No interpreter is bundled or started. These optional, fixed adapter assets
# are verified before the user enables a separately installed engine.
rm -rf "$app/Contents/Resources/ExtensionAssets.next"
cp -R build/v51/dsh-assets "$app/Contents/Resources/ExtensionAssets.next"
rm -rf "$app/Contents/Resources/ExtensionAssets"
mv "$app/Contents/Resources/ExtensionAssets.next" "$app/Contents/Resources/ExtensionAssets"
cp -X build/desktop/swift-26-source/debug/DunlinApp "$app/Contents/MacOS/.DunlinApp.new"
chmod 755 "$app/Contents/MacOS/.DunlinApp.new"
mv -f "$app/Contents/MacOS/.DunlinApp.new" "$app/Contents/MacOS/DunlinApp"
python3 - "$app/Contents/Info.plist" <<'PYINFO'
import plistlib,sys
from pathlib import Path
info=plistlib.loads(Path('dunlin-copilot/macos/Configuration/Info.plist').read_bytes())
values={'EXECUTABLE_NAME':'DunlinApp','PRODUCT_BUNDLE_IDENTIFIER':'org.dunlin.desktop','PRODUCT_NAME':'Dunlin Copilot','MARKETING_VERSION':'0.3.1-dev','CURRENT_PROJECT_VERSION':'3','MACOSX_DEPLOYMENT_TARGET':'26.0'}
for key,value in info.items():
    if isinstance(value,str):
        for name,replacement in values.items():value=value.replace('$('+name+')',replacement)
        if '$(' in value:raise ValueError('unresolved build setting')
        info[key]=value
Path(sys.argv[1]).write_bytes(plistlib.dumps(info))
PYINFO
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"
printf '%s\n' "$app"
