CXX ?= g++
CC ?= gcc
LD ?= ld
PLATFORM ?= $(if $(filter Windows_NT,$(OS)),windows,$(if $(filter Linux,$(shell uname -s 2>/dev/null)),linux,xj380))

BUILD_ROOT := build
BUILD_DIR := $(BUILD_ROOT)/$(PLATFORM)
OBJ_DIR := $(BUILD_DIR)/obj
HOST_WINDOWS := $(filter Windows_NT,$(OS))

FRAMEWORK_DIR := ../..
ROOT_DIR := ../../../..
ABGR_WRAPPER_DIR := $(FRAMEWORK_DIR)/third_party/ab_glyph_rasterizer/wrapper
ABGR_LIB := $(ABGR_WRAPPER_DIR)/target/release/libstardustui_ab_glyph_rasterizer_capi.a

SRC_FILES := \
	layout.cpp \
	$(FRAMEWORK_DIR)/src/file.cpp \
	$(FRAMEWORK_DIR)/src/network.cpp \
	$(FRAMEWORK_DIR)/src/theme.cpp \
	$(FRAMEWORK_DIR)/src/sytel.cpp \
	$(FRAMEWORK_DIR)/src/text/font.cpp \
	$(FRAMEWORK_DIR)/src/text/text_renderer.cpp \
	$(FRAMEWORK_DIR)/src/text/rasterizer/rasterizer.cpp \
	$(FRAMEWORK_DIR)/src/text/truetype/face.cpp \
	$(FRAMEWORK_DIR)/src/window.cpp \
	$(FRAMEWORK_DIR)/src/components/base.cpp \
	$(FRAMEWORK_DIR)/src/components/lable.cpp \
	$(FRAMEWORK_DIR)/src/components/button.cpp \
	$(FRAMEWORK_DIR)/src/components/canvas.cpp \
	$(FRAMEWORK_DIR)/src/components/scrollbar.cpp \
	$(FRAMEWORK_DIR)/src/components/textbox.cpp \
	$(FRAMEWORK_DIR)/src/components/flex.cpp

OBJ_FILES := \
	$(OBJ_DIR)/layout.o \
	$(OBJ_DIR)/framework/file.o \
	$(OBJ_DIR)/framework/network.o \
	$(OBJ_DIR)/framework/theme.o \
	$(OBJ_DIR)/framework/sytel.o \
	$(OBJ_DIR)/framework/text/font.o \
	$(OBJ_DIR)/framework/text/text_renderer.o \
	$(OBJ_DIR)/framework/text/rasterizer/rasterizer.o \
	$(OBJ_DIR)/framework/text/truetype/face.o \
	$(OBJ_DIR)/framework/window.o \
	$(OBJ_DIR)/framework/components/base.o \
	$(OBJ_DIR)/framework/components/lable.o \
	$(OBJ_DIR)/framework/components/button.o \
	$(OBJ_DIR)/framework/components/canvas.o \
	$(OBJ_DIR)/framework/components/scrollbar.o \
	$(OBJ_DIR)/framework/components/textbox.o \
	$(OBJ_DIR)/framework/components/flex.o

FRAMEWORK_HEADERS := \
	$(FRAMEWORK_DIR)/settings.hpp \
	$(FRAMEWORK_DIR)/includes/string.hpp \
	$(FRAMEWORK_DIR)/includes/vector.hpp \
	$(FRAMEWORK_DIR)/includes/file.hpp \
	$(FRAMEWORK_DIR)/includes/network.hpp \
	$(FRAMEWORK_DIR)/includes/theme.hpp \
	$(FRAMEWORK_DIR)/includes/text/font.hpp \
	$(FRAMEWORK_DIR)/includes/text/text_renderer.hpp \
	$(FRAMEWORK_DIR)/includes/text/rasterizer/geometry.hpp \
	$(FRAMEWORK_DIR)/includes/text/rasterizer/rasterizer.hpp \
	$(FRAMEWORK_DIR)/includes/text/truetype/face.hpp \
	$(FRAMEWORK_DIR)/includes/window.hpp \
	$(FRAMEWORK_DIR)/includes/sytle.hpp \
	$(FRAMEWORK_DIR)/includes/components/base.hpp \
	$(FRAMEWORK_DIR)/includes/components/lable.hpp \
	$(FRAMEWORK_DIR)/includes/components/button.hpp \
	$(FRAMEWORK_DIR)/includes/components/canvas.hpp \
	$(FRAMEWORK_DIR)/includes/components/scrollbar.hpp \
	$(FRAMEWORK_DIR)/includes/components/textbox.hpp \
	$(FRAMEWORK_DIR)/includes/components/flex.hpp \
	$(FRAMEWORK_DIR)/platforms/platform.hpp

CPPFLAGS += -I$(FRAMEWORK_DIR) -I$(FRAMEWORK_DIR)/includes
COMMON_CXXFLAGS := -O0 -Wall -g -std=gnu++17

ifeq ($(PLATFORM),windows)
TARGET := $(BUILD_DIR)/layout.exe
SRC_FILES += $(FRAMEWORK_DIR)/src/platforms/windows.cpp
OBJ_FILES += $(OBJ_DIR)/framework/platforms/windows.o
CPPFLAGS += -DSTARDUSTUI_WINDOWS
CXXFLAGS ?= $(COMMON_CXXFLAGS)
LDFLAGS += -static-libgcc -static-libstdc++
LDLIBS += -luser32 -lgdi32 -lmsimg32
LDLIBS += -lws2_32
else ifeq ($(PLATFORM),linux)
TARGET := $(BUILD_DIR)/layout
SRC_FILES += $(FRAMEWORK_DIR)/src/platforms/linux.cpp
OBJ_FILES += $(OBJ_DIR)/framework/platforms/linux.o
CPPFLAGS += -DSTARDUSTUI_LINUX
CPPFLAGS += -DSTARDUSTUI_USE_AB_GLYPH_RASTERIZER
CPPFLAGS += $(shell pkg-config --cflags sdl2 2>/dev/null)
CXXFLAGS ?= $(COMMON_CXXFLAGS)
LDLIBS += $(ABGR_LIB) $(shell pkg-config --libs sdl2 2>/dev/null) -ldl -lpthread -lm
else ifeq ($(PLATFORM),xj380)
TARGET := $(BUILD_DIR)/layout.elf
APP_OUT := ../../../../out/layout.elf
STAGING_APPS_DIR := ../../../../out/XJ380/apps

XAPI_DIR := $(ROOT_DIR)/user/xapi
XAPI_OUT_DIR := $(ROOT_DIR)/out/xapi
XAPI_START_OBJECT := $(XAPI_OUT_DIR)/start.cpp.o
XAPI_CORE_OBJECTS = $(shell find $(XAPI_OUT_DIR) -maxdepth 1 -name '*.o' | grep -v 'constart.cpp.o' | grep -v 'start.cpp.o')
XAPI_HEADERS := $(shell find $(XAPI_DIR)/include -type f \( -name '*.h' -o -name '*.hpp' \))
FRAMEWORK_XJ380_XAPI_HEADERS := $(shell find $(FRAMEWORK_DIR)/platforms/xj380/xapi -type f \( -name '*.h' -o -name '*.hpp' \))

SRC_FILES += $(FRAMEWORK_DIR)/src/platforms/xj380.cpp
OBJ_FILES += $(OBJ_DIR)/framework/platforms/xj380.o
CPPFLAGS += -DXJ380
CXXFLAGS ?= -O2 -Wall -g -ffreestanding -fno-builtin -m64 -mstackrealign -std=gnu++17 -fno-stack-protector -fno-exceptions -fno-rtti -fno-use-cxa-atexit -fshort-wchar -nostdinc -Wno-write-strings
LDFLAGS ?= -Ttext=0x200000 -e xapi_start
else
$(error Unsupported PLATFORM '$(PLATFORM)'. Use PLATFORM=windows, PLATFORM=linux, or PLATFORM=xj380)
endif

.PHONY: all clean copy xapi

all: $(TARGET)

define make-dir
$(if $(HOST_WINDOWS),if not exist "$(subst /,\,$(1))" mkdir "$(subst /,\,$(1))",mkdir -p "$(1)")
endef

ifeq ($(PLATFORM),xj380)
xapi:
	@$(MAKE) -C $(XAPI_DIR) build_all_api

$(TARGET): xapi $(OBJ_FILES)
	@$(call make-dir,$(dir $@))
	@$(LD) $(LDFLAGS) $(XAPI_START_OBJECT) $(XAPI_CORE_OBJECTS) $(XAPI_OUT_DIR)/liballoc-x86_64.a $(OBJ_FILES) -o $@
	@echo "LINK $(TARGET)"

copy: $(TARGET)
	@$(call make-dir,$(dir $(APP_OUT)))
	@cp -a $(TARGET) $(APP_OUT)
	@if [ -d "$(STAGING_APPS_DIR)" ]; then cp -a $(TARGET) $(STAGING_APPS_DIR)/layout.elf; fi
	@echo "COPY $(TARGET) -> $(APP_OUT)"
else
xapi:
	@echo "xapi is only used for PLATFORM=xj380"

$(ABGR_LIB):
	@cargo build --manifest-path $(ABGR_WRAPPER_DIR)/Cargo.toml --release
	@echo "CARGO $(ABGR_WRAPPER_DIR)"

$(TARGET): $(OBJ_FILES) $(if $(filter linux,$(PLATFORM)),$(ABGR_LIB))
	@$(call make-dir,$(dir $@))
	@$(CXX) $(OBJ_FILES) $(LDFLAGS) $(LDLIBS) -o $@
	@echo "LINK $(TARGET)"

copy: $(TARGET)
	@echo "copy is only used for PLATFORM=xj380"
endif

$(OBJ_DIR)/layout.o: layout.cpp $(FRAMEWORK_HEADERS)
	@$(call make-dir,$(dir $@))
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
	@echo "CPP $< -> $@"

$(OBJ_DIR)/framework/window.o: $(FRAMEWORK_DIR)/src/window.cpp $(FRAMEWORK_HEADERS)
	@$(call make-dir,$(dir $@))
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
	@echo "CPP $< -> $@"

$(OBJ_DIR)/framework/file.o: $(FRAMEWORK_DIR)/src/file.cpp $(FRAMEWORK_HEADERS)
	@$(call make-dir,$(dir $@))
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
	@echo "CPP $< -> $@"

$(OBJ_DIR)/framework/network.o: $(FRAMEWORK_DIR)/src/network.cpp $(FRAMEWORK_HEADERS)
	@$(call make-dir,$(dir $@))
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
	@echo "CPP $< -> $@"

$(OBJ_DIR)/framework/theme.o: $(FRAMEWORK_DIR)/src/theme.cpp $(FRAMEWORK_HEADERS)
	@$(call make-dir,$(dir $@))
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
	@echo "CPP $< -> $@"

$(OBJ_DIR)/framework/sytel.o: $(FRAMEWORK_DIR)/src/sytel.cpp $(FRAMEWORK_HEADERS)
	@$(call make-dir,$(dir $@))
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
	@echo "CPP $< -> $@"

$(OBJ_DIR)/framework/text/font.o: $(FRAMEWORK_DIR)/src/text/font.cpp $(FRAMEWORK_HEADERS)
	@$(call make-dir,$(dir $@))
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
	@echo "CPP $< -> $@"

$(OBJ_DIR)/framework/text/text_renderer.o: $(FRAMEWORK_DIR)/src/text/text_renderer.cpp $(FRAMEWORK_HEADERS)
	@$(call make-dir,$(dir $@))
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
	@echo "CPP $< -> $@"

$(OBJ_DIR)/framework/text/rasterizer/rasterizer.o: $(FRAMEWORK_DIR)/src/text/rasterizer/rasterizer.cpp $(FRAMEWORK_HEADERS)
	@$(call make-dir,$(dir $@))
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
	@echo "CPP $< -> $@"

$(OBJ_DIR)/framework/text/truetype/face.o: $(FRAMEWORK_DIR)/src/text/truetype/face.cpp $(FRAMEWORK_HEADERS)
	@$(call make-dir,$(dir $@))
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
	@echo "CPP $< -> $@"

$(OBJ_DIR)/framework/platforms/windows.o: $(FRAMEWORK_DIR)/src/platforms/windows.cpp $(FRAMEWORK_HEADERS)
	@$(call make-dir,$(dir $@))
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
	@echo "CPP $< -> $@"

$(OBJ_DIR)/framework/platforms/linux.o: $(FRAMEWORK_DIR)/src/platforms/linux.cpp $(FRAMEWORK_HEADERS)
	@$(call make-dir,$(dir $@))
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
	@echo "CPP $< -> $@"

$(OBJ_DIR)/framework/platforms/xj380.o: $(FRAMEWORK_DIR)/src/platforms/xj380.cpp $(FRAMEWORK_HEADERS) $(XAPI_HEADERS) $(FRAMEWORK_XJ380_XAPI_HEADERS)
	@$(call make-dir,$(dir $@))
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
	@echo "CPP $< -> $@"

$(OBJ_DIR)/framework/components/base.o: $(FRAMEWORK_DIR)/src/components/base.cpp $(FRAMEWORK_HEADERS)
	@$(call make-dir,$(dir $@))
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
	@echo "CPP $< -> $@"

$(OBJ_DIR)/framework/components/lable.o: $(FRAMEWORK_DIR)/src/components/lable.cpp $(FRAMEWORK_HEADERS)
	@$(call make-dir,$(dir $@))
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
	@echo "CPP $< -> $@"

$(OBJ_DIR)/framework/components/button.o: $(FRAMEWORK_DIR)/src/components/button.cpp $(FRAMEWORK_HEADERS)
	@$(call make-dir,$(dir $@))
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
	@echo "CPP $< -> $@"

$(OBJ_DIR)/framework/components/canvas.o: $(FRAMEWORK_DIR)/src/components/canvas.cpp $(FRAMEWORK_HEADERS)
	@$(call make-dir,$(dir $@))
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
	@echo "CPP $< -> $@"

$(OBJ_DIR)/framework/components/scrollbar.o: $(FRAMEWORK_DIR)/src/components/scrollbar.cpp $(FRAMEWORK_HEADERS)
	@$(call make-dir,$(dir $@))
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
	@echo "CPP $< -> $@"

$(OBJ_DIR)/framework/components/textbox.o: $(FRAMEWORK_DIR)/src/components/textbox.cpp $(FRAMEWORK_HEADERS)
	@$(call make-dir,$(dir $@))
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
	@echo "CPP $< -> $@"

$(OBJ_DIR)/framework/components/flex.o: $(FRAMEWORK_DIR)/src/components/flex.cpp $(FRAMEWORK_HEADERS)
	@$(call make-dir,$(dir $@))
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
	@echo "CPP $< -> $@"

clean:
ifeq ($(HOST_WINDOWS),Windows_NT)
	@if exist $(BUILD_DIR) rmdir /s /q $(BUILD_DIR)
else
	rm -rf $(BUILD_DIR)
endif
