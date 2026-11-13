# SPDX-License-Identifier: GPL-3.0-or-later
# There are deliberately no deploy targets: nothing here reaches a console.
CC := clang
PYTHON ?= python3
BUILD ?= build
CFLAGS ?= -O2 -g
WARN := -std=c11 -Wall -Wextra -Werror
CPPFLAGS := -Iinclude -D_POSIX_C_SOURCE=200809L
SDK_INCLUDE := .cache/sdk-v0.45/target/include
TEST_PORT := -DTILE_PORT=19190
MOCK_FLAGS := $(TEST_PORT) -Iinclude -idirafter $(SDK_INCLUDE) -D_SYS_STDINT_H_ -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror
LWIP_SRC := $(wildcard third_party/lwip/src/core/*.c) \
            $(wildcard third_party/lwip/src/core/ipv4/*.c) \
            $(wildcard third_party/lwip/src/api/*.c) third_party/lwip/src/netif/ethernet.c
# libsmb2 (LGPL-2.1-or-later, third_party/libsmb2/README.md) is third-party code: not held
# to -Werror. Its socket calls go through src/smb_net.c (include/smb_sock.h).
SMB2_SRC := $(wildcard third_party/libsmb2/lib/*.c)
# What src/secret.c takes from it: AES-128-CCM and HMAC-SHA256.
SMB2_CRYPTO := aes aes_reference aes128ccm hmac usha sha1 sha224-256 sha384-512
SMB2_CRYPTO_OBJ = $(patsubst %,$(BUILD)/obj/third_party/libsmb2/lib/%.o,$(SMB2_CRYPTO))
SMB2_FLAGS := -DHAVE_CONFIG_H -Ithird_party/libsmb2 -Ithird_party/libsmb2/include -Ithird_party/libsmb2/include/smb2 -include include/smb_sock.h
TESTS := $(BUILD)/test-rtl $(BUILD)/test-ncm $(BUILD)/test-files $(BUILD)/test-smb $(BUILD)/test-crash $(BUILD)/test-forward $(BUILD)/test-app

.PHONY: all test sanitize ps5 prepare clean
all: $(TESTS) $(BUILD)/page-server

test: all
	$(BUILD)/test-rtl
	$(BUILD)/test-ncm
	$(BUILD)/test-files
	$(BUILD)/test-smb
	$(BUILD)/test-crash
	$(BUILD)/test-forward
	rm -f $(BUILD)/test-nas.txt $(BUILD)/test-forward.txt
	$(BUILD)/test-app
	$(BUILD)/test-app 8156b
	$(BUILD)/test-app 8157
	$(BUILD)/test-app lost
	$(BUILD)/test-app none

src/app_assets.c: web/page.html assets/icon0.png scripts/embed_assets.py
	$(PYTHON) scripts/embed_assets.py

# The real sources against a simulated adapter, with the lwIP options of the console build.
NET_SRC := src/net.c src/http_server.c src/app_assets.c src/files.c src/rtl.c src/ncm.c src/local_http.c src/smb.c src/smb_client.c src/smb_net.c \
           src/smb_lwip.c src/secret.c src/forward.c src/forward_lwip.c port/sys_arch.c tests/tile_stub.c tests/peer.c tests/forward_services.c
NET_OBJ := $(patsubst %.c,$(BUILD)/obj/%.o,$(NET_SRC)) $(patsubst %.c,$(BUILD)/obj/%.o,$(LWIP_SRC)) $(patsubst %.c,$(BUILD)/obj/%.o,$(SMB2_SRC))
MOCKED := src/main.c src/adapter.c src/vendor.c src/vendor_regs.c tests/test_app.c
MOCKED_OBJ := $(patsubst %.c,$(BUILD)/mock/%.o,$(MOCKED))
HOST_FLAGS = $(TEST_PORT) -Iinclude -Iport -Ithird_party/lwip/src/include -Itests -D_POSIX_C_SOURCE=200809L $(EXTRA) $(CFLAGS) -std=gnu11 -pthread

$(BUILD)/obj/src/files.o: EXTRA := -D_DARWIN_C_SOURCE -DFILES_TESTING
$(BUILD)/mock/src/main.o: EXTRA := -Dmain=app_main -DSMB_STORE='"$(BUILD)/test-nas.txt"' -DFORWARD_STORE='"$(BUILD)/test-forward.txt"'
$(BUILD)/mock/src/adapter.o: EXTRA := -DADAPTER_WAIT_SECONDS=1
$(BUILD)/mock/tests/test_app.o: EXTRA := -DFORWARD_STORE='"$(BUILD)/test-forward.txt"'
$(BUILD)/obj/src/local_http.o: EXTRA := -D_DARWIN_C_SOURCE -DLOCAL_HTTP_TESTING

$(BUILD)/obj/src/smb_client.o: EXTRA := -Ithird_party/libsmb2/include
$(BUILD)/obj/src/smb_net.o: EXTRA := -D_DARWIN_C_SOURCE
$(BUILD)/obj/src/forward.o: EXTRA := -D_DARWIN_C_SOURCE
$(BUILD)/obj/tests/forward_services.o: EXTRA := -D_DARWIN_C_SOURCE
$(BUILD)/obj/src/secret.o: EXTRA := -D_DARWIN_C_SOURCE -Ithird_party/libsmb2/lib

$(BUILD)/obj/third_party/libsmb2/%.o: third_party/libsmb2/%.c third_party/libsmb2/config.h include/smb_sock.h
	@mkdir -p $(dir $@)
	$(CC) $(SMB2_FLAGS) $(CFLAGS) -D_DARWIN_C_SOURCE -w -c $< -o $@

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

# The NAS routes and the copy job, against a share that is a local folder (tests/smb_fake.c).
$(BUILD)/test-smb: src/smb.c src/files.c src/http_server.c src/app_assets.c tests/test_smb.c tests/smb_fake.c include/smb.h include/smb_client.h include/files.h include/http_server.h $(BUILD)/obj/src/secret.o $(SMB2_CRYPTO_OBJ)
	@mkdir -p $(dir $@)
	$(CC) -Iinclude -D_DARWIN_C_SOURCE -DFILES_TESTING $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror -pthread src/smb.c src/files.c src/http_server.c src/app_assets.c tests/test_smb.c tests/smb_fake.c $(BUILD)/obj/src/secret.o $(SMB2_CRYPTO_OBJ) -o $@

$(BUILD)/test-crash: src/crash.c tests/test_crash.c include/crash.h
	@mkdir -p $(dir $@)
	$(CC) -Iinclude $(CFLAGS) -std=c11 -Wall -Wextra -Werror -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE -DCRASH_TESTING src/crash.c tests/test_crash.c -o $@

# The relay with lwIP's side on loopback sockets (tests/forward_fake.c).
FORWARD_TEST := src/forward.c src/http_server.c src/app_assets.c tests/test_forward.c tests/forward_fake.c tests/forward_services.c
$(BUILD)/test-forward: $(FORWARD_TEST) include/forward.h include/forward_lwip.h include/http_server.h
	@mkdir -p $(dir $@)
	$(CC) -Iinclude -D_DARWIN_C_SOURCE $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror -pthread $(FORWARD_TEST) -o $@

$(BUILD)/test-app: $(MOCKED_OBJ) $(NET_OBJ)
	$(CC) $(CFLAGS) -pthread $^ -o $@

# For walking the page in a real browser.
PAGE_SERVER := tests/page_server.c src/files.c src/http_server.c src/app_assets.c src/smb.c tests/smb_fake.c src/forward.c tests/forward_fake.c
$(BUILD)/page-server: $(PAGE_SERVER) include/files.h include/http_server.h include/smb.h include/smb_client.h include/forward.h $(BUILD)/obj/src/secret.o $(SMB2_CRYPTO_OBJ)
	@mkdir -p $(dir $@)
	$(CC) -Iinclude -D_DARWIN_C_SOURCE -DFILES_TESTING $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror -pthread $(PAGE_SERVER) $(BUILD)/obj/src/secret.o $(SMB2_CRYPTO_OBJ) -o $@

sanitize:
	$(MAKE) BUILD=build/asan CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" test

ps5: src/app_assets.c
	$(PYTHON) scripts/build_ps5.py

prepare:
	$(PYTHON) scripts/prepare.py --sdk --zig

clean:
	rm -rf build build-ps5 src/app_assets.c
