# Makefile for step 1: ELF explorer.
#
# Targets:
#   make             — build the mini-readelf tool and both example binaries
#   make tool        — build only the mini-readelf tool
#   make examples    — build only the example hello-world binaries
#   make outputs     — generate snapshots in examples/outputs/ (readelf and LD_DEBUG)
#   make run         — build everything and dump the tool's output for both examples
#   make clean       — remove build artifacts

CC      := gcc
CFLAGS  := -std=c11 -O2 -Wall -Wextra -Wpedantic

TOOL    := src/mini-readelf
TOOL_SRC:= src/mini-readelf.c

EX_DIR  := examples
OUT_DIR := $(EX_DIR)/outputs
HELLO_D := $(EX_DIR)/hello-dynamic
HELLO_S := $(EX_DIR)/hello-static

.PHONY: all tool examples outputs run clean

all: tool examples

tool: $(TOOL)

$(TOOL): $(TOOL_SRC)
	$(CC) $(CFLAGS) $< -o $@

examples: $(HELLO_D) $(HELLO_S)

$(HELLO_D): $(EX_DIR)/hello-dynamic.c
	$(CC) $(CFLAGS) $< -o $@

# -static forces everything to be bundled in. Requires libc6-dev (Ubuntu)
# or glibc-static (Fedora/RHEL) to be installed.
$(HELLO_S): $(EX_DIR)/hello-static.c
	$(CC) $(CFLAGS) -static $< -o $@

# Generate snapshots that the walkthrough doc references.
# Uses `script -qc` only if we want a PTY; plain redirect is fine for these.
outputs: all
	@mkdir -p $(OUT_DIR)
	@echo "  [snap] $(OUT_DIR)/hello-dynamic.readelf.txt"
	@./$(TOOL) $(HELLO_D) > $(OUT_DIR)/hello-dynamic.readelf.txt
	@echo "  [snap] $(OUT_DIR)/hello-static.readelf.txt"
	@./$(TOOL) $(HELLO_S)  > $(OUT_DIR)/hello-static.readelf.txt
	@echo "  [snap] $(OUT_DIR)/hello-dynamic.lddebug.txt"
	@LD_DEBUG=libs $(HELLO_D) > /dev/null 2> $(OUT_DIR)/hello-dynamic.lddebug.txt || true
	@echo "  [snap] $(OUT_DIR)/hello-dynamic.strace.txt"
	@strace -o $(OUT_DIR)/hello-dynamic.strace.txt $(HELLO_D) > /dev/null 2>&1 || \
		echo "    (strace not installed — skipping)"
	@echo "Done. See $(OUT_DIR)/"

run: all
	@echo "=============================="
	@echo "=== hello-dynamic ============"
	@echo "=============================="
	./$(TOOL) $(HELLO_D)
	@echo ""
	@echo "=============================="
	@echo "=== hello-static ============="
	@echo "=============================="
	./$(TOOL) $(HELLO_S)

clean:
	rm -f $(TOOL) $(HELLO_D) $(HELLO_S)
