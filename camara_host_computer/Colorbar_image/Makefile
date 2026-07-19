CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
CPPFLAGS += -Iinclude

BUILD_DIR := build
TARGET := $(BUILD_DIR)/pcie_color_rx
SOURCES := src/pcie_color_rx.c

.PHONY: all app driver clean

all: app driver

app: $(TARGET)

$(TARGET): $(SOURCES) include/colorbar_pcie_rx.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o $@

$(BUILD_DIR):
	mkdir -p $@

driver:
	$(MAKE) -C driver

clean:
	rm -rf $(BUILD_DIR)
	$(MAKE) -C driver clean
