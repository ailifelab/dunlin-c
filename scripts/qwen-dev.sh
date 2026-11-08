#!/bin/sh
# One persistent writable Agent development container; update its CLI in place.
set -eu
root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
trial=${DUNLIN_TRIAL_DIR:-"$root/dunlin-trial"}
name=dunlin-agent-dev
update=no
if [ "${1:-}" = --update ]; then update=yes; shift; fi
if ! docker container inspect "$name" >/dev/null 2>&1; then
    docker create --name "$name" --init --env-file "$trial/model.env" \
        --mount "type=bind,source=$trial/qwen-dev.json,target=/var/lib/dunlin/model.json,readonly" \
        --mount "type=bind,source=$trial/work,target=/var/lib/dunlin/work" \
        --mount "type=bind,source=$root/Makefile,target=/src/Makefile,readonly" \
        --mount "type=bind,source=$root/dunlin-command/src,target=/src/dunlin-command/src,readonly" \
        --mount "type=bind,source=$root/include,target=/src/include,readonly" \
        --mount "type=bind,source=$root/dunlin-compact/kernel,target=/src/dunlin-compact/kernel,readonly" \
        --mount "type=bind,source=$root/dunlin-compact/platform,target=/src/dunlin-compact/platform,readonly" \
        --mount "type=bind,source=$root/dunlin-compact/plugins,target=/src/dunlin-compact/plugins,readonly" \
        --mount "type=bind,source=$root/dunlin-compact/runtime,target=/src/dunlin-compact/runtime,readonly" \
        --workdir /src ubuntu:24.04 sleep infinity >/dev/null
    update=yes
fi
if [ "$(docker inspect --format '{{.State.Running}}' "$name")" != true ]; then
    docker start "$name" >/dev/null
fi
if ! docker exec "$name" test -f /var/lib/dunlin/toolchain-ready; then
    docker exec "$name" sh -ec '
        apt-get update
        DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends build-essential pkg-config libjson-c-dev libcurl4-openssl-dev libedit-dev ca-certificates curl
        rm -rf /var/lib/apt/lists/*
        mkdir -p /var/lib/dunlin
        touch /var/lib/dunlin/toolchain-ready
    '
fi
if [ "$update" = yes ] || ! docker exec "$name" test -x /opt/dunlin/dunlin; then
    docker exec "$name" sh -ec '
        mkdir /var/lib/dunlin/update.lock || { echo "Another runtime update is in progress" >&2; exit 1; }
        trap "rmdir /var/lib/dunlin/update.lock" EXIT
        make -j2 BUILD=/var/lib/dunlin/build all
        mkdir -p /opt/dunlin
        cp /var/lib/dunlin/build/dunlin /opt/dunlin/dunlin.next
        chmod 755 /opt/dunlin/dunlin.next
        mv /opt/dunlin/dunlin.next /opt/dunlin/dunlin
    '
fi
printf 'Execution target: persistent writable Linux container %s.\n' "$name" >&2
if [ -f "$trial/openai-sol/api-key" ]; then
    DUNLIN_OPENAI_SOL_KEY=$(cat "$trial/openai-sol/api-key")
    export DUNLIN_OPENAI_SOL_KEY
    set -- --env DUNLIN_OPENAI_SOL_KEY "$name" /opt/dunlin/dunlin --config /var/lib/dunlin/model.json "$@" chat
    exec docker exec -it "$@"
fi
exec docker exec -it "$name" /opt/dunlin/dunlin --config /var/lib/dunlin/model.json "$@" chat
