CC ?= cc
AR ?= ar
BUILD_DIR ?= build
PREFIX ?= /usr/local

CPPFLAGS += -D_FORTIFY_SOURCE=2 -Iinclude -Isrc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror -fPIC
LDFLAGS ?=
LDLIBS += -lutil

LIB_OBJECT := $(BUILD_DIR)/kitty_pty_broker.o
CLI_OBJECT := $(BUILD_DIR)/main.o
TUI_OBJECT := $(BUILD_DIR)/tui.o
TEST_OBJECT := $(BUILD_DIR)/test_broker.o
STATIC_LIB := $(BUILD_DIR)/libkitty-pty-broker.a
SHARED_LIB := $(BUILD_DIR)/libkitty-pty-broker.so
CLI := $(BUILD_DIR)/kitty-pty-broker
TEST := $(BUILD_DIR)/test-broker

.PHONY: all clean install sanitize test

all: $(STATIC_LIB) $(SHARED_LIB) $(CLI)

$(BUILD_DIR):
	mkdir -p "$@"

$(LIB_OBJECT): src/kitty_pty_broker.c src/protocol.h include/kitty_pty_broker.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(CLI_OBJECT): src/main.c include/kitty_pty_broker.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(TUI_OBJECT): src/tui.c src/tui.h include/kitty_pty_broker.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(TEST_OBJECT): tests/test_broker.c include/kitty_pty_broker.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(STATIC_LIB): $(LIB_OBJECT)
	$(AR) rcs "$@" "$<"

$(SHARED_LIB): $(LIB_OBJECT)
	$(CC) -shared $(LDFLAGS) -Wl,-soname,libkitty-pty-broker.so -o "$@" "$<" $(LDLIBS)

$(CLI): $(CLI_OBJECT) $(TUI_OBJECT) $(SHARED_LIB)
	$(CC) $(LDFLAGS) -Wl,-rpath,'$$ORIGIN' -o "$@" $(CLI_OBJECT) $(TUI_OBJECT) -L$(BUILD_DIR) -lkitty-pty-broker $(LDLIBS)

$(TEST): $(TEST_OBJECT) $(SHARED_LIB)
	$(CC) $(LDFLAGS) -Wl,-rpath,'$$ORIGIN' -o "$@" $(TEST_OBJECT) -L$(BUILD_DIR) -lkitty-pty-broker $(LDLIBS)

test: $(TEST) $(CLI)
	"$(TEST)"

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS="-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fPIC -fsanitize=address,undefined" \
		LDFLAGS="-fsanitize=address,undefined" test

install: all
	install -d "$(DESTDIR)$(PREFIX)/include" "$(DESTDIR)$(PREFIX)/lib" "$(DESTDIR)$(PREFIX)/bin"
	install -m 0644 include/kitty_pty_broker.h "$(DESTDIR)$(PREFIX)/include/"
	install -m 0644 "$(STATIC_LIB)" "$(DESTDIR)$(PREFIX)/lib/"
	install -m 0755 "$(SHARED_LIB)" "$(DESTDIR)$(PREFIX)/lib/"
	install -m 0755 "$(CLI)" "$(DESTDIR)$(PREFIX)/bin/"

clean:
	rm -rf -- "$(BUILD_DIR)"
