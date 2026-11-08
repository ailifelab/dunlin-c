#!/bin/sh
# Both execution targets run the same C CLI, tool registry and approval path.
set -eu
usage() {
    echo "Usage: $0 [--session ID | ID] [--linux|--noble|--mac] [--task|--auto]" >&2
    echo "No ID creates a fresh session. /help lists local commands after startup." >&2
    echo "Options: --permissions read-only|strict, --approval-default yes|no" >&2
    echo "         --model ID, --json or --events (default: readable text)" >&2
    echo "         --update rebuilds the persistent Linux container CLI in place" >&2
    exit "${1:-64}"
}
root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
trial=${DUNLIN_TRIAL_DIR:-"$root/dunlin-trial"}
session=
target=--linux
mode=
permission_mode=read-only
approval_default=no
output_mode=
model=
update=
while [ "$#" -gt 0 ]; do
    case $1 in
    --help|-h) usage 0;;
    --update) update=--update; shift;;
    --permissions) [ "$#" -ge 2 ] || usage; permission_mode=$2; shift 2;;
    --approval-default) [ "$#" -ge 2 ] || usage; approval_default=$2; shift 2;;
    --json) if [ "$output_mode" != --events ]; then output_mode=--json; fi; shift;;
    --events) output_mode=--events; shift;;
    --model)
        [ "$#" -ge 2 ] && [ -n "$2" ] || usage
        case $2 in --*) usage;; esac
        model=$2; shift 2;;
    --session)
        [ "$#" -ge 2 ] && [ -z "$session" ] && [ -n "$2" ] || usage
        case $2 in --*) usage;; esac
        session=$2; shift 2;;
    --linux|--noble|--mac) target=$1; shift;;
    --task|--auto) mode=$1; shift;;
    --*) usage;;
    *) [ -z "$session" ] || usage; session=$1; shift;;
    esac
done
case $mode in
    ''|--auto) set --;;
    --task) set -- --require-tool shell; echo 'Task mode: every turn requires an approved shell command.' >&2;;
    *) echo "Unknown tool mode: $mode" >&2; exit 64;;
esac
case $permission_mode in read-only|strict) ;; *) usage;; esac
case $approval_default in yes|no) ;; *) usage;; esac
set -- --permissions "$permission_mode" --approval-default "$approval_default" "$@"
if [ -n "$session" ]; then set -- --session "$session" "$@"; fi
if [ -n "$model" ]; then set -- --model "$model" "$@"; fi
if [ -n "$output_mode" ]; then set -- "$output_mode" "$@"; fi
case $target in
--mac)
    if [ "$(uname -s)" != Darwin ]; then echo "The Mac target requires macOS." >&2; exit 64; fi
    # Read the literal value. Never evaluate the contents of the private env file.
    while IFS= read -r line || [ -n "$line" ]; do
        case $line in OMLX_API_KEY=*) export "$line";; esac
    done < "$trial/model.env"
    if [ -f "$trial/openai-sol/api-key" ]; then
        DUNLIN_OPENAI_SOL_KEY=$(cat "$trial/openai-sol/api-key")
        export DUNLIN_OPENAI_SOL_KEY
    fi
    printf 'Execution target: macOS host.\n' >&2
    exec "$root/dist/0.2.12/macos-arm64/dunlin" --config "$trial/qwen-mac.json" "$@" chat
    ;;
--linux|--noble)
    if [ -n "$update" ]; then set -- "$update" "$@"; fi
    exec "$root/scripts/qwen-dev.sh" "$@"
    ;;
*) echo "Unknown execution target. Use --linux, --noble or --mac." >&2; exit 64;;
esac
