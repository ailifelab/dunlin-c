ROOT := ../..
BUILD ?= $(abspath $(ROOT)/build/linux)
PREFIX ?= /usr
DEPS := $(BUILD)/deps
CACHE ?= $(BUILD)/downloads
JOBS ?= 4

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -fPIC -Wall -Wextra
CPPFLAGS += -D_GNU_SOURCE -DDUNLIN_WITH_DSH -I$(DEPS)/include -I$(ROOT)/include -I$(ROOT)/vendor/libyaml/include -include $(ROOT)/modules/config_yaml/build_config.h
WEBKIT ?= $(shell pkg-config --exists webkit2gtk-4.1 && echo webkit2gtk-4.1 || echo webkit2gtk-4.0)
GUI_PKGS := gtk+-3.0 libsecret-1 libxml-2.0 $(WEBKIT)
LINUX_ARCH ?= $(shell dpkg --print-architecture | sed 's/amd64/x64/')
ASSETS := $(BUILD)/ExtensionAssets
STRATEGY_DIR := $(BUILD)/ContextStrategies/extractive
STRATEGY_FILES := $(addprefix $(STRATEGY_DIR)/,dunlin.json strategy.py)
ASSET_INPUTS := $(ROOT)/scripts/prepare-dsh-assets.py $(ROOT)/scripts/build-dsh-web.mjs $(wildcard $(ROOT)/hosts/node-dsh/* $(ROOT)/compat/dsh/web/*.*)
CORE_SRC := $(wildcard $(ROOT)/desktop/*.c $(ROOT)/modules/*/*.c $(ROOT)/compat/dsh/*.c $(ROOT)/vendor/libyaml/src/*.c $(ROOT)/dunlin-compact/kernel/*.c $(ROOT)/dunlin-compact/runtime/*.c $(ROOT)/dunlin-compact/plugins/*.c $(ROOT)/dunlin-compact/platform/*.c)
CORE_OBJ := $(patsubst $(ROOT)/%.c,$(BUILD)/obj/%.o,$(CORE_SRC))
UI_SRC := $(wildcard *.c)
UI_OBJ := $(patsubst %.c,$(BUILD)/gui/%.o,$(UI_SRC))
PINS := packaging/prepare_deps.py $(ROOT)/vendor/native-macos/pins.json $(ROOT)/vendor/media-curl/pin.json
CORE_LIBS := $(DEPS)/lib/libjson-c.a $(DEPS)/lib/libsqlite3.a $(DEPS)/lib/libcurl.a $(DEPS)/lib/libssl.a $(DEPS)/lib/libcrypto.a -lz -ldl -lpthread -lutil -lm

.PHONY: all deps core install deb check-backend FORCE
all: $(BUILD)/dunlin-copilot $(ASSETS)/assets.json $(STRATEGY_FILES)
deps: $(DEPS)/ready
core: $(BUILD)/lib/libdunlin-desktop.so.1

$(STRATEGY_DIR)/%: $(ROOT)/compat/context/extractive/%
	install -Dm644 $< $@

ifneq ($(strip $(DSH_ASSETS)),)
$(ASSETS)/assets.json: $(DSH_ASSETS)/assets.json
	python3 packaging/install_assets.py $(DSH_ASSETS) $(ASSETS) $(LINUX_ARCH)
else
$(ASSETS)/assets.json: $(ASSET_INPUTS)
	python3 $(ROOT)/scripts/prepare-dsh-assets.py $(ASSETS) --linux-arch $(LINUX_ARCH)
endif

$(DEPS)/ready: $(PINS)
	@mkdir -p $(BUILD)
	python3 packaging/prepare_deps.py $(DEPS) --cache $(CACHE) --jobs $(JOBS) $(if $(OFFLINE),--offline)
	@touch $@

$(BUILD)/obj/%.o: $(ROOT)/%.c $(DEPS)/ready
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

# Record the complete object set: adding/removing a translation unit can
# change the library even when a restored object predates the last link.
$(BUILD)/core-sources.list: FORCE
	@mkdir -p $(@D)
	@printf '%s\n' $(sort $(CORE_SRC)) > $@.tmp
	@if cmp -s $@.tmp $@; then rm $@.tmp; else mv $@.tmp $@; fi

$(BUILD)/lib/libdunlin-desktop.so.1: $(CORE_OBJ) $(BUILD)/core-sources.list packaging/desktop.exports
	@mkdir -p $(@D)
	$(CC) -shared -Wl,-z,defs,-z,relro,-z,now,-soname,libdunlin-desktop.so.1 -Wl,-Bsymbolic-functions,--version-script=packaging/desktop.exports $(CORE_OBJ) $(CORE_LIBS) -o $@

$(BUILD)/gui/%.o: %.c $(DEPS)/ready
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DDUNLIN_WITH_WEBKIT=1 $(shell pkg-config --cflags $(GUI_PKGS)) -MMD -MP -c $< -o $@

$(BUILD)/ui-sources.list: FORCE
	@mkdir -p $(@D)
	@printf '%s\n' $(sort $(UI_SRC)) > $@.tmp
	@if cmp -s $@.tmp $@; then rm $@.tmp; else mv $@.tmp $@; fi

$(BUILD)/dunlin-copilot: $(UI_OBJ) $(BUILD)/ui-sources.list $(BUILD)/lib/libdunlin-desktop.so.1
	$(CC) $(UI_OBJ) -L$(BUILD)/lib -l:libdunlin-desktop.so.1 $(shell pkg-config --libs $(GUI_PKGS)) -Wl,-z,relro,-z,now -Wl,-rpath,'$$ORIGIN/lib:$$ORIGIN/../lib/dunlin-copilot' -o $@

install: all
	install -Dm755 $(BUILD)/dunlin-copilot $(DESTDIR)$(PREFIX)/bin/dunlin-copilot
	install -Dm644 $(BUILD)/lib/libdunlin-desktop.so.1 $(DESTDIR)$(PREFIX)/lib/dunlin-copilot/libdunlin-desktop.so.1
	install -Dm644 packaging/org.dunlin.copilot.desktop $(DESTDIR)$(PREFIX)/share/applications/org.dunlin.copilot.desktop
	install -Dm644 ../assets/DunlinCopilot.png $(DESTDIR)$(PREFIX)/share/pixmaps/org.dunlin.copilot.png
	install -d $(DESTDIR)$(PREFIX)/share/dunlin-copilot
	python3 packaging/install_assets.py $(ASSETS) $(DESTDIR)$(PREFIX)/share/dunlin-copilot/ExtensionAssets $(LINUX_ARCH)
	install -d $(DESTDIR)$(PREFIX)/share/dunlin-copilot/ContextStrategies/extractive
	install -m644 $(STRATEGY_FILES) $(DESTDIR)$(PREFIX)/share/dunlin-copilot/ContextStrategies/extractive/
	install -d $(DESTDIR)$(PREFIX)/share/doc/dunlin-copilot
	install -m644 $(DEPS)/licenses/*.txt $(DESTDIR)$(PREFIX)/share/doc/dunlin-copilot/
	install -m644 $(ROOT)/vendor/libyaml/License $(DESTDIR)$(PREFIX)/share/doc/dunlin-copilot/libyaml.txt

deb: all
	python3 packaging/deb.py --build $(BUILD)

check-backend: all
	sh tests/run-displays.sh $(BUILD)/dunlin-copilot

-include $(CORE_OBJ:.o=.d) $(UI_OBJ:.o=.d)
