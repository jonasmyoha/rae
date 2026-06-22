# Coordinate-unit prototype — build/run wrapper.
# Run from the rae/ checkout:  make -C examples/101_coord_proto run
RAE      := $(CURDIR)/../../compiler/bin/rae
SRC      := $(CURDIR)/main.rae
BUILD    := $(CURDIR)/build
OUT_C    := $(BUILD)/main.c
RUNTIME_C:= $(BUILD)/rae_runtime.c
BIN      := $(BUILD)/coord_proto

CFLAGS  := -O2 -I/opt/homebrew/include -DRAE_HAS_RAYLIB
LDFLAGS := -L/opt/homebrew/lib /opt/homebrew/lib/libraylib.a \
           -framework CoreVideo -framework IOKit -framework Cocoa -framework OpenGL

.PHONY: build run clean
$(BUILD):
	@mkdir -p $(BUILD)
build: $(BUILD)
	$(RAE) build --target compiled --emit-c --entry $(SRC) --out $(OUT_C)
	gcc $(CFLAGS) -o $(BIN) $(OUT_C) $(RUNTIME_C) -I$(BUILD) $(LDFLAGS)
run: build
	$(BIN)
clean:
	rm -rf $(BUILD)
