#!/bin/sh
# Host policy regression only; no network, serial or physical IO.
set -eu
cd "$(dirname "$0")/.."
build=${1:-build/device-agent-host}
cjson=${DUNLIN_CJSON_DIR:-build/pio-core/packages/framework-espidf/components/json/cJSON}
test -f "$cjson/cJSON.c" || { echo 'Pinned ESP-IDF cJSON sources unavailable; configure DUNLIN_CJSON_DIR.' >&2; exit 1; }
mkdir -p "$build"
# Keep project warnings strict. Apple's SDK deprecations inside pinned third-party
# cJSON remain visible, without editing the SDK or suppressing project warnings.
${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror -Wno-error=deprecated-declarations ${CFLAGS:--O2 -g} -I"$cjson" -c "$cjson/cJSON.c" -o "$build/cJSON.o"
${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror ${CFLAGS:--O2 -g} -I"$cjson" \
 dunlin-copilot/esp32/agent/agent.c tests/embedded/agent.c "$build/cJSON.o" -lm -o "$build/policy-test"
"$build/policy-test"

${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror ${CFLAGS:--O2 -g} -I"$cjson" \
 dunlin-copilot/esp32/agent/agent.c tests/embedded/agent_alloc.c "$build/cJSON.o" -lm -o "$build/allocator-test"
"$build/allocator-test"

${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror ${CFLAGS:--O2 -g} -I"$cjson" \
 dunlin-copilot/esp32/agent/agent.c tests/embedded/agent_chat.c "$build/cJSON.o" -lm -o "$build/chat-test"
"$build/chat-test"

${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror ${CFLAGS:--O2 -g} -I"$cjson" \
 dunlin-copilot/esp32/agent/agent.c dunlin-copilot/esp32/common/http_date.c tests/embedded/agent_memory.c "$build/cJSON.o" -lm -o "$build/memory-test"
"$build/memory-test"
${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror ${CFLAGS:--O2 -g} -DAE_AGENT_ENABLE_PYTHON=0 -I"$cjson" \
 dunlin-copilot/esp32/agent/agent.c tests/embedded/agent_python_disabled.c "$build/cJSON.o" -lm -o "$build/python-disabled-test"
"$build/python-disabled-test"

${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror ${CFLAGS:--O2 -g} -I"$cjson" \
 dunlin-copilot/esp32/agent/agent.c tests/embedded/agent_audit.c "$build/cJSON.o" -lm -o "$build/audit-test"
for scenario in 0 1 2 3; do "$build/audit-test" "$scenario"; done
