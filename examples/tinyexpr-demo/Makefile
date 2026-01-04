ROOT_DIR := $(abspath ../..)
RAE := $(ROOT_DIR)/compiler/bin/rae
THIRD_PARTY := $(ROOT_DIR)/third_party/tinyexpr
BUILD_DIR := $(CURDIR)/build
OUT_C := $(BUILD_DIR)/tinyexpr_demo.c
RUNTIME_C := $(BUILD_DIR)/rae_runtime.c
RUNTIME_H := $(BUILD_DIR)/rae_runtime.h
TARGET := $(BUILD_DIR)/tinyexpr_demo

.PHONY: all demo clean

all: demo

demo: $(TARGET)
	@echo "Built tinyexpr demo -> $(TARGET)"

$(TARGET): $(OUT_C) $(THIRD_PARTY)/tinyexpr.c $(THIRD_PARTY)/rae_tinyexpr.c
	@mkdir -p $(BUILD_DIR)
	cc -I$(BUILD_DIR) -I$(THIRD_PARTY) \
	  $(OUT_C) \
	  $(THIRD_PARTY)/rae_tinyexpr.c \
	  $(THIRD_PARTY)/tinyexpr.c \
	  $(RUNTIME_C) \
	  -o $(TARGET)

$(OUT_C): main.rae
	@mkdir -p $(BUILD_DIR)
	$(RAE) build --emit-c $< --out $(OUT_C)

clean:
	rm -rf $(BUILD_DIR)
