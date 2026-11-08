CC ?= cc
AR ?= ar
BUILD ?= build
CFLAGS ?= -O2 -g
CPPFLAGS += -Iinclude
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
DESKTOP_CFLAGS = -D_POSIX_C_SOURCE=200809L $(shell pkg-config --cflags json-c libcurl)
DESKTOP_LIBS = $(shell pkg-config --libs json-c libcurl) -lm
ifeq ($(shell uname -s),Darwin)
JSON_C_PREFIX ?= /opt/homebrew/opt/json-c
DESKTOP_CFLAGS = -D_DARWIN_C_SOURCE -I$(JSON_C_PREFIX)/include
DESKTOP_LIBS = $(JSON_C_PREFIX)/lib/libjson-c.a -lcurl -lm
endif
KERNEL = $(patsubst %.c,$(BUILD)/%.o,$(wildcard dunlin-compact/kernel/*.c))
DESKTOP = $(patsubst %.c,$(BUILD)/%.o,$(wildcard dunlin-compact/platform/*.c dunlin-compact/plugins/*.c dunlin-compact/runtime/*.c))
CLI_SOURCES = $(filter-out dunlin-command/src/windows_main.c,$(wildcard dunlin-command/src/*.c))
CLI = $(patsubst %.c,$(BUILD)/%.o,$(CLI_SOURCES))
CLI_LIBS = -ledit
ARCHIVES = $(BUILD)/libdunlin.a $(BUILD)/libdunlin-kernel.a

.PHONY: all runtime-data kernel test check test-kernel
all: runtime-data $(BUILD)/dunlin
runtime-data:
	@mkdir -p runtime-data/work runtime-data/sessions
	@chmod 700 runtime-data/work runtime-data/sessions
kernel: $(BUILD)/libdunlin-kernel.a
$(BUILD)/dunlin-compact/kernel/%.o: dunlin-compact/kernel/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(DESKTOP_CFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/libdunlin-kernel.a: $(KERNEL)
	$(AR) rcs $@ $^
$(BUILD)/libdunlin.a: $(DESKTOP)
	$(AR) rcs $@ $^
$(BUILD)/dunlin: $(CLI) $(ARCHIVES)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(DESKTOP_LIBS) $(CLI_LIBS) -o $@
$(BUILD)/kernel-test: $(BUILD)/tests/kernel.o $(BUILD)/libdunlin-kernel.a
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@
$(BUILD)/model-test: $(BUILD)/tests/model_unit.o $(ARCHIVES)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(DESKTOP_LIBS) -o $@
test-kernel: $(BUILD)/kernel-test
	$(BUILD)/kernel-test
	$(BUILD)/kernel-test --trace > $(BUILD)/kernel.trace
	diff -u tests/cordis/expected.trace $(BUILD)/kernel.trace
check: test-agent-suite
test: all test-kernel test-agent test-embed $(BUILD)/model-test
	$(BUILD)/model-test
	LC_ALL=C.UTF-8 PYTHONDONTWRITEBYTECODE=1 python3 tests/launcher.py
	LC_ALL=C.UTF-8 PYTHONDONTWRITEBYTECODE=1 python3 tests/cli.py $(BUILD)/dunlin
	LC_ALL=C.UTF-8 PYTHONDONTWRITEBYTECODE=1 python3 tests/model_e2e.py $(BUILD)/dunlin
$(BUILD)/platform-random-test: $(BUILD)/tests/platform_random.o $(BUILD)/dunlin-compact/platform/random.o
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@
.PHONY: test-platform-random
test-platform-random: $(BUILD)/platform-random-test
	$(BUILD)/platform-random-test
test: test-platform-random
$(BUILD)/platform-clock-test: $(BUILD)/tests/platform_clock.o $(BUILD)/dunlin-compact/platform/clock.o
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@
.PHONY: test-platform-clock
test-platform-clock: $(BUILD)/platform-clock-test
	$(BUILD)/platform-clock-test
test: test-platform-clock

$(BUILD)/http-get-test: $(BUILD)/tests/http_get.o $(ARCHIVES)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(DESKTOP_LIBS) -o $@
.PHONY: test-http-get
test-http-get: $(BUILD)/http-get-test
	$(BUILD)/http-get-test
test: test-http-get

$(BUILD)/permission-modes-test: $(BUILD)/tests/permission_modes.o $(ARCHIVES)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(DESKTOP_LIBS) -o $@
.PHONY: test-permission-modes
test-permission-modes: $(BUILD)/permission-modes-test
	$(BUILD)/permission-modes-test
test: test-permission-modes

$(BUILD)/credential-broker-test: $(BUILD)/tests/credential_broker.o $(ARCHIVES)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(DESKTOP_LIBS) -o $@
.PHONY: test-credential-broker
test-credential-broker: $(BUILD)/credential-broker-test
	$(BUILD)/credential-broker-test
test: test-credential-broker
-include $(KERNEL:.o=.d) $(DESKTOP:.o=.d) $(wildcard $(BUILD)/dunlin-command/src/*.d) $(wildcard $(BUILD)/tests/*.d)

$(BUILD)/agent-test: $(BUILD)/tests/agent_unit.o $(ARCHIVES)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(DESKTOP_LIBS) -o $@
.PHONY: test-agent
test-agent: $(BUILD)/agent-test
	$(BUILD)/agent-test

$(BUILD)/embed-test: $(BUILD)/tests/embed.o $(ARCHIVES)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(DESKTOP_LIBS) -o $@
.PHONY: test-embed
test-embed: $(BUILD)/embed-test
	$(BUILD)/embed-test

$(BUILD)/cli-policy-test: $(BUILD)/tests/cli_policy.o $(BUILD)/dunlin-command/src/policy.o $(BUILD)/dunlin-command/src/live.o $(BUILD)/dunlin-command/src/tools_live.o $(ARCHIVES)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(DESKTOP_LIBS) $(CLI_LIBS) -o $@
.PHONY: test-cli-policy
test-cli-policy: $(BUILD)/cli-policy-test
	$(BUILD)/cli-policy-test
test: test-cli-policy

.PHONY: test-headers test-embedded
test-headers: all
	PYTHONDONTWRITEBYTECODE=1 python3 tests/public_headers.py $(BUILD)
test-embedded:
	./scripts/test-embedded.sh $(BUILD)/embedded

$(BUILD)/agent-contracts-test: $(BUILD)/tests/agent_contracts.o $(ARCHIVES)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(DESKTOP_LIBS) -o $@
$(BUILD)/runtime-profiles-test: $(BUILD)/tests/runtime_profiles.o $(ARCHIVES)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(DESKTOP_LIBS) -o $@
$(BUILD)/dunlin-compact/runtime/runtime_faults.o: dunlin-compact/runtime/runtime.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(DESKTOP_CFLAGS) $(CFLAGS) -Djson_object_object_add=dunlin_profile_test_add -MMD -MP -c $< -o $@
$(BUILD)/tests/runtime_profiles_faults.o: tests/runtime_profiles.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(DESKTOP_CFLAGS) $(CFLAGS) -DDUNLIN_PROFILE_FAULTS -MMD -MP -c $< -o $@
$(BUILD)/dunlin-compact/platform/session_faults.o: dunlin-compact/platform/session.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(DESKTOP_CFLAGS) $(CFLAGS) -DDUNLIN_SESSION_FAULTS -MMD -MP -c $< -o $@
$(BUILD)/runtime-profiles-faults-test: $(BUILD)/tests/runtime_profiles_faults.o $(BUILD)/dunlin-compact/runtime/runtime_faults.o $(BUILD)/dunlin-compact/platform/session_faults.o $(ARCHIVES)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(DESKTOP_LIBS) -o $@
$(BUILD)/protocol-test: $(BUILD)/tests/protocol_unit.o $(BUILD)/tests/protocol_responses_unit.o $(ARCHIVES)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(DESKTOP_LIBS) -o $@
.PHONY: test-agent-contracts test-runtime-profiles test-protocol test-shell-profiles test-agent-suite
test-agent-contracts: $(BUILD)/agent-contracts-test
	$(BUILD)/agent-contracts-test
test-runtime-profiles: $(BUILD)/runtime-profiles-test $(BUILD)/runtime-profiles-faults-test
	$(BUILD)/runtime-profiles-test
	$(BUILD)/runtime-profiles-faults-test
test-protocol: all $(BUILD)/protocol-test
	$(BUILD)/protocol-test
	LC_ALL=C.UTF-8 PYTHONDONTWRITEBYTECODE=1 python3 tests/protocol_e2e.py $(BUILD)/dunlin
test-shell-profiles: all
	LC_ALL=C.UTF-8 PYTHONDONTWRITEBYTECODE=1 python3 tests/shell_profiles.py $(BUILD)/dunlin
test-agent-suite: test test-agent-contracts test-runtime-profiles test-protocol test-shell-profiles test-headers test-embedded
test: test-fixture
.PHONY: test-fixture
test-fixture:
	PYTHONDONTWRITEBYTECODE=1 python3 tests/test_mock_model.py
	PYTHONDONTWRITEBYTECODE=1 python3 tests/test_agent_runner.py
-include $(BUILD)/dunlin-compact/runtime/runtime_faults.d $(BUILD)/dunlin-compact/platform/session_faults.d

$(BUILD)/intent-test: $(BUILD)/tests/intent_unit.o $(ARCHIVES)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(DESKTOP_LIBS) -o $@
.PHONY: test-intent-progress
test-intent-progress: all $(BUILD)/intent-test
	$(BUILD)/intent-test
	PYTHONDONTWRITEBYTECODE=1 python3 tests/intent_progress.py $(BUILD)/dunlin
test: test-intent-progress

.PHONY: test-shell-live-ui
test-shell-live-ui: all
	PYTHONDONTWRITEBYTECODE=1 python3 tests/shell_live_ui.py $(BUILD)/dunlin
test: test-shell-live-ui

# Private Loop allocation/ownership regression; production build has no hooks.
$(BUILD)/dunlin-compact/plugins/loop_faults.o: dunlin-compact/plugins/loop.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(DESKTOP_CFLAGS) $(CFLAGS) -Djson_object_new_object=dunlin_loop_test_object -Djson_object_new_array=dunlin_loop_test_array -Djson_object_new_string=dunlin_loop_test_string -Djson_object_new_int=dunlin_loop_test_int -Djson_object_new_boolean=dunlin_loop_test_bool -Djson_object_object_add=dunlin_loop_test_add -Djson_object_array_add=dunlin_loop_test_push -MMD -MP -c $< -o $@
$(BUILD)/loop-alloc-test: $(BUILD)/tests/loop_alloc.o $(BUILD)/dunlin-compact/plugins/loop_faults.o $(ARCHIVES)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(DESKTOP_LIBS) -o $@
.PHONY: test-loop-alloc
test-loop-alloc: $(BUILD)/loop-alloc-test
	$(BUILD)/loop-alloc-test
test-agent-contracts: test-loop-alloc

$(BUILD)/dunlin-compact/platform/session_list_faults.o: dunlin-compact/platform/session.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(DESKTOP_CFLAGS) $(CFLAGS) -Djson_object_array_add=dunlin_session_test_push -MMD -MP -c $< -o $@
$(BUILD)/session-list-alloc-test: $(BUILD)/tests/session_list_alloc.o $(BUILD)/dunlin-compact/platform/session_list_faults.o $(ARCHIVES)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(DESKTOP_LIBS) -o $@
.PHONY: test-session-list-alloc
test-session-list-alloc: $(BUILD)/session-list-alloc-test
	$(BUILD)/session-list-alloc-test
test-agent-contracts: test-session-list-alloc
