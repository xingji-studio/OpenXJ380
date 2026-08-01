CXX ?= clang++
AR ?= ar
PLATFORM ?= $(if $(filter Windows_NT,$(OS)),windows,$(if $(filter Linux,$(shell uname -s 2>/dev/null)),linux,xj380))

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
TARGET := $(BUILD_DIR)/libStardustUI.a 

SRC_FILES := \
	src/file.cpp \
	src/network.cpp \
	src/sytel.cpp \
	src/text/font.cpp \
	src/text/text_renderer.cpp \
	src/text/rasterizer/rasterizer.cpp \
	src/text/truetype/face.cpp \
	src/window.cpp \
	src/components/base.cpp \
	src/components/lable.cpp \
	src/components/button.cpp \
	src/components/canvas.cpp \
	src/components/scrollbar.cpp \
	src/components/textbox.cpp \
	src/components/flex.cpp

ifeq ($(PLATFORM),windows)
SRC_FILES += src/platforms/windows.cpp
CPPFLAGS += -DSTARDUSTUI_WINDOWS
else ifeq ($(PLATFORM),linux)
SRC_FILES += src/platforms/linux.cpp
CPPFLAGS += -DSTARDUSTUI_LINUX
CPPFLAGS += -DSTARDUSTUI_USE_AB_GLYPH_RASTERIZER
CPPFLAGS += $(shell pkg-config --cflags sdl2 2>/dev/null)
else ifeq ($(PLATFORM),xj380)
SRC_FILES += src/platforms/xj380.cpp
CPPFLAGS += -DXJ380
else
$(error Unsupported PLATFORM '$(PLATFORM)'. Use PLATFORM=windows, PLATFORM=linux, or PLATFORM=xj380)
endif

OBJ_FILES := $(patsubst src/%.cpp,$(OBJ_DIR)/%.o,$(SRC_FILES))

CPPFLAGS += -I. -I./includes
CXXFLAGS ?= -O0 -g -std=gnu++17 -Wall -Wextra -Wpedantic -Wwrite-strings -fno-builtin
ifeq ($(PLATFORM),xj380)
CXXFLAGS += -ffreestanding -fno-exceptions -fno-rtti -fno-use-cxa-atexit -nostdinc
endif
ARFLAGS ?= rcs

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ_FILES)
	@mkdir -p $(dir $@)
	$(AR) $(ARFLAGS) $@ $(OBJ_FILES)

$(OBJ_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)
