CC      = gcc
CFLAGS  = -Wall -Wextra -Werror -std=c99 -D_GNU_SOURCE -g -O2
LDFLAGS = -lncurses
INCLUDES = -Iinclude

SRC_DIR = src
OBJ_DIR = build
BIN     = piperewind

SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/pipeline.c \
       $(SRC_DIR)/capture.c \
       $(SRC_DIR)/trace.c \
       $(SRC_DIR)/tui.c \
       $(SRC_DIR)/procstate.c \
       $(SRC_DIR)/diff.c

OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

.PHONY: all clean test

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

clean:
	rm -rf $(OBJ_DIR) $(BIN) *.prt

# Quick smoke test
test: $(BIN)
	@echo "=== Test 1: Simple two-stage pipeline ==="
	./$(BIN) record -v -o test1.prt "echo hello world | cat"
	./$(BIN) dump test1.prt
	@echo ""
	@echo "=== Test 2: Three-stage pipeline ==="
	./$(BIN) record -v -o test2.prt "echo -e 'banana\napple\ncherry' | sort | head -2"
	./$(BIN) dump test2.prt
	@echo ""
	@echo "=== Test 3: Longer pipeline ==="
	./$(BIN) record -v -o test3.prt "echo -e 'a\nb\na\nc\nb\na' | sort | uniq -c | sort -rn"
	./$(BIN) dump test3.prt
	@echo ""
	@echo "=== Test 4: Single-quoted arguments ==="
	./$(BIN) record -v -o test4.prt "echo 'hello world' | grep 'hello world'"
	./$(BIN) dump test4.prt
	@echo ""
	@echo "=== Test 5: Double-quoted arguments ==="
	./$(BIN) record -v -o test5.prt 'echo "foo bar baz" | cat'
	./$(BIN) dump test5.prt
	@echo ""
	@echo "All tests passed."

