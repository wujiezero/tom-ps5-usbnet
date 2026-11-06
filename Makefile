# SPDX-License-Identifier: GPL-3.0-or-later
# There are deliberately no deploy targets: nothing here reaches a console.
CC := clang
PYTHON ?= python3
BUILD ?= build
CFLAGS ?= -O2 -g
WARN := -std=c11 -Wall -Wextra -Werror
CPPFLAGS := -Iinclude -D_POSIX_C_SOURCE=200809L
SDK_INCLUDE := .cache/sdk-v0.45/target/include
MOCK_FLAGS := -Iinclude -idirafter $(SDK_INCLUDE) -D_SYS_STDINT_H_ -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror
LWIP_SRC := $(wildcard third_party/lwip/src/core/*.c) \
            $(wildcard third_party/lwip/src/core/ipv4/*.c) \
            $(wildcard third_party/lwip/src/api/*.c) third_party/lwip/src/netif/ethernet.c
TESTS := $(BUILD)/test-rtl $(BUILD)/test-ncm $(BUILD)/test-files $(BUILD)/test-crash $(BUILD)/test-app

.PHONY: all test sanitize ps5 prepare clean
all: $(TESTS) $(BUILD)/page-server

test: all
	$(BUILD)/test-rtl
	$(BUILD)/test-ncm
	$(BUILD)/test-files
	$(BUILD)/test-crash
	$(BUILD)/test-app

src/app_assets.c: web/page.html assets/icon0.png scripts/embed_assets.py
	$(PYTHON) scripts/embed_assets.py

# The real sources against a simulated adapter, with the lwIP options of the console build.
NET_SRC := src/net.c src/http_server.c src/app_assets.c src/files.c src/rtl.c src/ncm.c port/sys_arch.c tests/tile_stub.c tests/peer.c
NET_OBJ := $(patsubst %.c,$(BUILD)/obj/%.o,$(NET_SRC)) $(patsubst %.c,$(BUILD)/obj/%.o,$(LWIP_SRC))
MOCKED := src/main.c src/adapter.c src/vendor.c src/vendor_regs.c tests/test_app.c
MOCKED_OBJ := $(patsubst %.c,$(BUILD)/mock/%.o,$(MOCKED))
HOST_FLAGS = -Iinclude -Iport -Ithird_party/lwip/src/include -Itests -D_POSIX_C_SOURCE=200809L $(EXTRA) $(CFLAGS) -std=gnu11 -pthread

$(BUILD)/obj/src/files.o: EXTRA := -D_DARWIN_C_SOURCE -DFILES_TESTING
$(BUILD)/mock/src/main.o: EXTRA := -Dmain=app_main

$(BUILD)/obj/third_party/lwip/%.o: third_party/lwip/%.c include/lwipopts.h
	@mkdir -p $(dir $@)
	$(CC) $(HOST_FLAGS) -Wall -Wextra -c $< -o $@

$(BUILD)/obj/%.o: %.c $(wildcard include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(HOST_FLAGS) -Wall -Wextra -Werror -c $< -o $@

$(BUILD)/mock/%.o: %.c $(wildcard include/*.h) tests/mock_adapter.h tests/mock_descriptors.h
	@mkdir -p $(dir $@)
	$(CC) $(MOCK_FLAGS) -Itests -include tests/mock_adapter.h $(EXTRA) -pthread -c $< -o $@

$(BUILD)/test-rtl: src/rtl.c tests/test_rtl.c include/rtl.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) src/rtl.c tests/test_rtl.c -o $@

$(BUILD)/test-ncm: src/ncm.c tests/test_ncm.c include/ncm.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) src/ncm.c tests/test_ncm.c -o $@

$(BUILD)/test-files: src/files.c src/http_server.c src/app_assets.c tests/test_files.c include/files.h include/http_server.h
	@mkdir -p $(dir $@)
	$(CC) -Iinclude -D_DARWIN_C_SOURCE -DFILES_TESTING $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror -pthread src/files.c src/http_server.c src/app_assets.c tests/test_files.c -o $@

$(BUILD)/test-crash: src/crash.c tests/test_crash.c include/crash.h
	@mkdir -p $(dir $@)
	$(CC) -Iinclude $(CFLAGS) -std=c11 -Wall -Wextra -Werror -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE -DCRASH_TESTING src/crash.c tests/test_crash.c -o $@

$(BUILD)/test-app: $(MOCKED_OBJ) $(NET_OBJ)
	$(CC) $(CFLAGS) -pthread $^ -o $@

# For walking the page in a real browser.
$(BUILD)/page-server: tests/page_server.c src/files.c src/http_server.c src/app_assets.c include/files.h include/http_server.h
	@mkdir -p $(dir $@)
	$(CC) -Iinclude -D_DARWIN_C_SOURCE -DFILES_TESTING $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror -pthread tests/page_server.c src/files.c src/http_server.c src/app_assets.c -o $@

sanitize:
	$(MAKE) BUILD=build/asan CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" test

ps5: src/app_assets.c
	$(PYTHON) scripts/build_ps5.py

prepare:
	$(PYTHON) scripts/prepare.py --sdk --zig

clean:
	rm -rf build build-ps5 src/app_assets.c
