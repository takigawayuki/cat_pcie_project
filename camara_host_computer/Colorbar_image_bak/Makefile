CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
CPPFLAGS += -Iinclude

BUILD_DIR := build
TARGET := $(BUILD_DIR)/pcie_color_rx
GUI_TARGET := $(BUILD_DIR)/pcie_color_gui
SOURCES := src/pcie_color_rx.c
GUI_SOURCES := src/pcie_color_gui.c
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-2.0 gthread-2.0 2>/dev/null)
GTK_LIBS := $(shell pkg-config --libs gtk+-2.0 gthread-2.0 2>/dev/null)

.PHONY: all app gui driver clean

all: app gui driver

app: $(TARGET)

gui: $(GUI_TARGET)

$(TARGET): $(SOURCES) include/colorbar_pcie_rx.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o $@

$(GUI_TARGET): $(GUI_SOURCES) include/colorbar_pcie_rx.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GTK_CFLAGS) $(GUI_SOURCES) -o $@ $(GTK_LIBS)

$(BUILD_DIR):
	mkdir -p $@

driver:
	$(MAKE) -C driver

clean:
	rm -rf $(BUILD_DIR)
	$(MAKE) -C driver clean
