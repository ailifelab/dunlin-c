# Third-party components

Retained upstream source and licenses:

- libyaml: `vendor/libyaml/License` and `pin.json`.
- Cordis and Cosmokit from the fixed DeepSeek Harness baseline: `compat/dsh/pin.json`, `compat/dsh/vendor/*/LICENSE`.
- DSH UI slots/renderer: `compat/dsh/web/vendor/*/LICENSE` and pinned provenance files.
- MicroPython embedded source: `dunlin-copilot/esp32/esp32/components/micropython/LICENSE-MicroPython` and `UPSTREAM.json`.
- Build-time json-c/OpenSSL/curl: pinned by `vendor/native-macos/pins.json` and `vendor/media-curl/pin.json`. Build scripts retain their licenses and bundle them in the application.
- Node build dependencies are locked under `hosts/node-dsh/`; their upstream licenses apply.

Community index links do not bundle third-party plugins or relicense their code.
Dunlin's own project license has not yet been selected; making the repository public
alone does not grant an additional open-source license. Upstream license notices remain unchanged.
