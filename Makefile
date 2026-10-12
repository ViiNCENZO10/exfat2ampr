#   exfat2ampr - on-console AMPR LZ4 packer (ps5-payload-sdk)
#   SPDX-License-Identifier: GPL-3.0-or-later
#
#   make deps      download LZ4 sources and libSceAmpr.sprx (once)
#   make           build exfat2ampr.elf for the PS5 (needs PS5_PAYLOAD_SDK)
#   make host      build exfat2ampr-host for Linux (CI validation, no SDK needed)
#   make test      send the ELF to the console (PS5_HOST / PS5_PORT)

PS5_HOST ?= ps5
PS5_PORT ?= 9021
PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk

ELF      := exfat2ampr.elf
HOSTBIN  := exfat2ampr-host
# 1.<number of commits + VERSION_OFFSET>: grows with every change, the same number as the app's contentVersion
# (01.000.NNN). The public repository starts its history anew: its VERSION_OFFSET keeps the numbers going.
VERSION  := $(shell n=$$(git rev-list --count HEAD 2>/dev/null) && o=$$(cat VERSION_OFFSET 2>/dev/null || echo 0) && echo 1.$$((n + o)) || echo dev)

LZ4_VERSION := 1.10.0
LZ4_URL     := https://raw.githubusercontent.com/lz4/lz4/v$(LZ4_VERSION)/lib
LZ4_DIR     := third_party/lz4
LZ4_SRCS    := $(LZ4_DIR)/lz4.c $(LZ4_DIR)/lz4hc.c
LZ4_HDRS    := $(LZ4_DIR)/lz4.h $(LZ4_DIR)/lz4hc.h

# AMPR emulator embedded as a fallback (ShadowMountPlus' copy is preferred at runtime)
SPRX_VERSION := 0.4.2.1
SPRX_URL     := https://github.com/drakmor/ampr_emu/releases/download/$(SPRX_VERSION)/libSceAmpr.sprx
SPRX         := third_party/libSceAmpr.sprx
# debug build with the APR command journal (/app0/ampr_commands.bin), used by the trace mode
SPRX_DEBUG_URL := https://github.com/drakmor/ampr_emu/releases/download/$(SPRX_VERSION)/libSceAmpr.sprx-$(SPRX_VERSION)-test-debug-pack
SPRX_DEBUG     := third_party/libSceAmpr_debug.sprx
# game plugins the engine keeps up to date in the games' fakelib (drakmor's replacement modules, GPL-3.0):
#   pgo_stub -> libScePlayGo.sprx (PlayGo: every chunk reported installed)
#   dlc_emu  -> libSceAppContent.sprx, libSceGameUpdate.sprx, libSceNpEntitlementAccess.sprx (DLC listed in dlc_emu.ini)
# pinned releases, "nolog" builds, checked against tools/plugins.sha256 (src/plugins.c also knows the older releases)
PGO_VERSION := 0.6
PGO_URL     := https://github.com/drakmor/pgo_stub/releases/download/$(PGO_VERSION)/libScePlayGo.sprx
DLC_VERSION := 0.3
DLC_URL     := https://github.com/drakmor/dlc_emu/releases/download/$(DLC_VERSION)/dlc_emu_$(DLC_VERSION).zip
PLUGIN_DIR  := third_party/plugins
PLUGINS     := $(PLUGIN_DIR)/libScePlayGo.sprx $(PLUGIN_DIR)/dlc_emu.ok

CORE_SRCS := src/util.c src/sha256.c src/scan.c src/profile.c src/ampr_index.c src/ampr_pack.c src/verify.c src/smp.c src/trace.c src/pair.c src/storage.c src/bench.c src/selfupdate.c src/activity.c src/plugins.c src/diag.c src/covers.c src/estimate.c src/job.c src/httpd.c
GEN       := gen/index_html.c gen/libSceAmpr_sprx.c gen/libSceAmpr_debug_sprx.c gen/plugin_playgo.c gen/plugin_appcontent.c gen/plugin_gameupdate.c gen/plugin_entitlement.c
COMMON_CFLAGS := -O2 -g -Wall -Wextra -std=gnu11 -Isrc -I$(LZ4_DIR) -DE2A_VERSION=\"$(VERSION)\" -DLOG_JSON_CAP=20000

# ---------------------------------------------------------------- PS5 build --
PS5_SRCS := src/main.c $(CORE_SRCS)
PS5_OBJS := $(PS5_SRCS:src/%.c=build/ps5/%.o) $(GEN:gen/%.c=build/ps5/gen_%.o) $(LZ4_SRCS:$(LZ4_DIR)/%.c=build/ps5/lz4_%.o)
PS5_CFLAGS := $(COMMON_CFLAGS) -Werror -D_BSD_SOURCE -ffunction-sections -fdata-sections
PS5_LDFLAGS := -Wl,--gc-sections

all: $(ELF)

$(ELF): PS5_CC := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
$(ELF): $(PS5_OBJS)
	@test -x $(PS5_CC) || (echo "PS5_PAYLOAD_SDK not found at $(PS5_PAYLOAD_SDK)"; exit 1)
	$(PS5_CC) $(PS5_LDFLAGS) -o $@ $^
	-$(PS5_PAYLOAD_SDK)/bin/prospero-strip --strip-all $@

build/ps5/%.o: src/%.c src/*.h $(LZ4_HDRS)
	@mkdir -p build/ps5
	$(PS5_PAYLOAD_SDK)/bin/prospero-clang $(PS5_CFLAGS) -c -o $@ $<

build/ps5/gen_%.o: gen/%.c
	@mkdir -p build/ps5
	$(PS5_PAYLOAD_SDK)/bin/prospero-clang $(PS5_CFLAGS) -c -o $@ $<

# lz4hc.c #includes lz4.c, so both sources must be on disk before either compiles
build/ps5/lz4_%.o: $(LZ4_DIR)/%.c $(LZ4_SRCS) $(LZ4_HDRS)
	@mkdir -p build/ps5
	$(PS5_PAYLOAD_SDK)/bin/prospero-clang $(COMMON_CFLAGS) -D_BSD_SOURCE -Wno-unused-function -c -o $@ $<

# --------------------------------------------------------------- host build --
HOSTCC ?= cc
HOST_SRCS := src/host_main.c $(CORE_SRCS)
HOST_OBJS := $(HOST_SRCS:src/%.c=build/host/%.o) $(GEN:gen/%.c=build/host/gen_%.o) $(LZ4_SRCS:$(LZ4_DIR)/%.c=build/host/lz4_%.o)
HOST_CFLAGS := $(COMMON_CFLAGS) -Werror -D_GNU_SOURCE -DE2A_HOST -pthread

host: $(HOSTBIN)

$(HOSTBIN): $(HOST_OBJS)
	$(HOSTCC) -pthread -o $@ $^ -lm

build/host/%.o: src/%.c src/*.h $(LZ4_HDRS)
	@mkdir -p build/host
	$(HOSTCC) $(HOST_CFLAGS) -c -o $@ $<

build/host/gen_%.o: gen/%.c
	@mkdir -p build/host
	$(HOSTCC) $(HOST_CFLAGS) -c -o $@ $<

build/host/lz4_%.o: $(LZ4_DIR)/%.c $(LZ4_SRCS) $(LZ4_HDRS)
	@mkdir -p build/host
	$(HOSTCC) $(COMMON_CFLAGS) -D_GNU_SOURCE -Wno-unused-function -c -o $@ $<

# ---- generated assets (xxd -i) ----
gen/index_html.c: web/index.html
	@mkdir -p gen
	cd web && xxd -i index.html > ../$@

gen/libSceAmpr_sprx.c: $(SPRX)
	@mkdir -p gen
	cd third_party && xxd -i libSceAmpr.sprx > ../$@

gen/libSceAmpr_debug_sprx.c: $(SPRX_DEBUG)
	@mkdir -p gen
	cd third_party && xxd -i libSceAmpr_debug.sprx > ../$@

gen/plugin_playgo.c: $(PLUGIN_DIR)/libScePlayGo.sprx
	@mkdir -p gen
	cd $(PLUGIN_DIR) && xxd -i libScePlayGo.sprx > ../../$@

gen/plugin_appcontent.c: $(PLUGIN_DIR)/dlc_emu.ok
	@mkdir -p gen
	cd $(PLUGIN_DIR) && xxd -i libSceAppContent.sprx > ../../$@

gen/plugin_gameupdate.c: $(PLUGIN_DIR)/dlc_emu.ok
	@mkdir -p gen
	cd $(PLUGIN_DIR) && xxd -i libSceGameUpdate.sprx > ../../$@

gen/plugin_entitlement.c: $(PLUGIN_DIR)/dlc_emu.ok
	@mkdir -p gen
	cd $(PLUGIN_DIR) && xxd -i libSceNpEntitlementAccess.sprx > ../../$@

# ---- third party downloads (done once; BSD / GPL compatible) ----
$(LZ4_DIR)/%.c:
	@mkdir -p $(LZ4_DIR)
	curl -fsSL -o $@ $(LZ4_URL)/$(notdir $@)

$(LZ4_DIR)/%.h:
	@mkdir -p $(LZ4_DIR)
	curl -fsSL -o $@ $(LZ4_URL)/$(notdir $@)

$(SPRX):
	@mkdir -p third_party
	curl -fsSL -o $@ $(SPRX_URL)

$(SPRX_DEBUG):
	@mkdir -p third_party
	curl -fsSL -o $@ $(SPRX_DEBUG_URL)

.DELETE_ON_ERROR:   # a download whose digest does not match is removed, never kept as "up to date"

# the plugins: every file checked against tools/plugins.sha256 before it is embedded
$(PLUGIN_DIR)/libScePlayGo.sprx:
	@mkdir -p $(PLUGIN_DIR)
	curl -fsSL -o $@ $(PGO_URL)
	cd $(PLUGIN_DIR) && sha256sum --check --strict --ignore-missing ../../tools/plugins.sha256

$(PLUGIN_DIR)/dlc_emu.ok:
	@mkdir -p $(PLUGIN_DIR)
	curl -fsSL -o $(PLUGIN_DIR)/dlc_emu.zip $(DLC_URL)
	cd $(PLUGIN_DIR) && rm -rf nolog && unzip -qo dlc_emu.zip 'nolog/*' && mv -f nolog/*.sprx . && rmdir nolog
	cd $(PLUGIN_DIR) && sha256sum --check --strict --ignore-missing ../../tools/plugins.sha256
	touch $@

deps: $(LZ4_SRCS) $(LZ4_HDRS) $(SPRX) $(SPRX_DEBUG) $(PLUGINS)

clean:
	rm -rf $(ELF) $(HOSTBIN) build gen/*.c

distclean: clean
	rm -rf $(LZ4_DIR) $(SPRX) $(SPRX_DEBUG) $(PLUGIN_DIR) gen

version:
	@echo $(VERSION)

test: $(ELF)
	$(PS5_PAYLOAD_SDK)/bin/prospero-deploy -h $(PS5_HOST) -p $(PS5_PORT) $(ELF)

.PHONY: all host clean distclean deps test version
