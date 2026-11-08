#!/bin/sh
# Host-only, no hardware, model, serial device or SDK required.
set -eu
cd "$(dirname "$0")/.."
build=${1:-build/embedded-host}
mkdir -p "$build"
${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror -Wmissing-prototypes -Iinclude \
 ${CFLAGS:--O2 -g} dunlin-compact/kernel/kernel.c dunlin-copilot/esp32/common/pool.c dunlin-copilot/esp32/common/probe.c dunlin-copilot/esp32/io/service.c \
 dunlin-copilot/esp32/serial/commands.c dunlin-copilot/esp32/serial/serial.c tests/embedded/test.c -o "$build/test"
"$build/test"
${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror -Wmissing-prototypes -Iinclude -Itests/embedded/rtos \
 ${CFLAGS:--O2 -g} dunlin-compact/kernel/kernel.c dunlin-copilot/esp32/common/pool.c dunlin-copilot/esp32/common/probe.c dunlin-copilot/esp32/io/service.c \
 dunlin-copilot/esp32/serial/commands.c dunlin-copilot/esp32/serial/serial.c dunlin-copilot/esp32/esp32/src/executor.c tests/embedded/rtos.c -o "$build/rtos-test"
"$build/rtos-test"
"$build/rtos-test" owner
"$build/rtos-test" serial
"$build/rtos-test" queue

# S3 operator-shell routing/rendering is portable C; no board/service needed.
${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror -Wmissing-prototypes -Iinclude \
 ${CFLAGS:--O2 -g} dunlin-copilot/esp32/serial/shell.c tests/embedded/shell.c -o "$build/shell-test"
"$build/shell-test"

# Private persisted profile compatibility; no SDK/device/network or real secrets.
${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Itests/embedded/nvs \
 ${CFLAGS:--O2 -g} dunlin-copilot/esp32/esp32/src/profile.c tests/embedded/profile.c -o "$build/profile-test"
"$build/profile-test"

# HTTPS policy parsing is host-only C; actual TLS is verified separately on board.
${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude \
 ${CFLAGS:--O2 -g} dunlin-copilot/esp32/common/https_policy.c tests/embedded/https_policy.c -o "$build/https-policy-test"
"$build/https-policy-test"

# Actual UART output path with recording driver and deterministic RTOS harness.
${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Itests/embedded/rtos \
 ${CFLAGS:--O2 -g} dunlin-compact/kernel/kernel.c dunlin-copilot/esp32/common/pool.c dunlin-copilot/esp32/common/probe.c dunlin-copilot/esp32/io/service.c \
 dunlin-copilot/esp32/serial/commands.c dunlin-copilot/esp32/serial/serial.c dunlin-copilot/esp32/serial/shell.c tests/embedded/uart_output.c -o "$build/uart-output-test"
"$build/uart-output-test"

# Production UART driver wrapper: overflow notifications and saturation fallback.
${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Itests/embedded/rtos \
 ${CFLAGS:--O2 -g} dunlin-copilot/esp32/esp32/src/uart_console.c tests/embedded/uart_console.c -o "$build/uart-console-test"
"$build/uart-console-test"
