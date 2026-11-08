#!/bin/sh
# The TS inputs are verified even when the checkout is a git archive.
set -eu
: "${DSH_SOURCE:?set DSH_SOURCE to the pinned deepseek-harness checkout}"
: "${NODE:=node}"
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/vendor" "$tmp/node_modules/@deepseek-ai"
cp -R "$DSH_SOURCE/vendor/cordis" "$DSH_SOURCE/vendor/cosmokit" "$tmp/vendor/"
(cd "$tmp" && sha256sum -c "$here/source.sha256" >&2)
# Only change test package resolution. All executable TS is upstream source.
printf '%s\n' '{"name":"@deepseek-ai/cosmokit","type":"module","exports":"./src/index.ts"}' >"$tmp/vendor/cosmokit/package.json"
ln -s ../../vendor/cosmokit "$tmp/node_modules/@deepseek-ai/cosmokit"
cp "$here/reference.mjs" "$tmp/main.mjs"
"$NODE" --disable-warning=ExperimentalWarning --experimental-transform-types "$tmp/main.mjs"
