# Device Simulator v2.1 - Simple Unix-compatible Makefile
# Fixed for MSYS2/UCRT64 environments

TARGET     := device-simulator
VERSION    := 2.1
BUILD      ?= debug
MODE       ?= simulation

# Build directories - Unix style only
OUTPUT_DIR := build/linux/$(BUILD)
EXE_EXT    := 
TARGET_EXE := $(OUTPUT_DIR)/$(TARGET)$(EXE_EXT)

# Compiler setup based on mode
ifeq ($(MODE),mcu)
    # MCU toolchain
    CC         := arm-none-eabi-gcc
    OBJCOPY    := arm-none-eabi-objcopy
    SIZE       := arm-none-eabi-size
    MCU_FLAGS  := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
    MODE_FLAGS := -DUSE_HAL_DRIVER -DSTM32F407xx $(MCU_FLAGS)
    LDSCRIPT   := STM32F407VGTx_FLASH.ld
    LDFLAGS    := -T$(LDSCRIPT) -lc -lm -lnosys
else
    # Simulation mode (default)
    CC         := gcc
    MODE_FLAGS := -DSIMULATION_MODE
    LDFLAGS    := -lws2_32 -lwinmm
endif

# Source files
MAIN_SRC    := main.c
CORE_SRCS   := device_simulator.c platform_abstraction.c
PROTO_SRCS  := protocol/protocol.c protocol/io_buffer.c

# Conditional sources
ifeq ($(MODE),mcu)
    CORE_SRCS += mcu_hal.c
endif

ALL_SRCS := $(MAIN_SRC) $(CORE_SRCS) $(PROTO_SRCS)

# Object files
OBJS := $(ALL_SRCS:%.c=$(OUTPUT_DIR)/%.o)

# Headers
HEADERS := device_simulator.h config.h protocol/protocol.h protocol/io_buffer.h
ifeq ($(MODE),mcu)
    HEADERS += mcu_hal.h
endif

# Compiler flags
CFLAGS_BASE := -std=c11 -Wall -Wextra -Wno-unused-parameter
CFLAGS_BASE += -I. -Iprotocol $(MODE_FLAGS)

ifeq ($(BUILD),release)
    CFLAGS := $(CFLAGS_BASE) -O2 -DNDEBUG
else ifeq ($(BUILD),profile)
    CFLAGS := $(CFLAGS_BASE) -O2 -g -pg -DPROFILE
    LDFLAGS += -pg
else
    CFLAGS := $(CFLAGS_BASE) -O0 -g -DDEBUG
endif

# Default target
all: $(TARGET_EXE)

# Create directories
$(OUTPUT_DIR):
	@echo "Creating build directories..."
	@mkdir -p $(OUTPUT_DIR)/protocol

# Link target
$(TARGET_EXE): $(OBJS) | $(OUTPUT_DIR)
	@echo "[LINK] $@"
	@$(CC) $(OBJS) -o "$@" $(LDFLAGS)
	@echo "Build completed: $(TARGET_EXE)"
ifeq ($(MODE),mcu)
	@echo "Generating HEX and BIN files..."
	@$(SIZE) "$@"
	@$(OBJCOPY) -O ihex "$@" "$(OUTPUT_DIR)/$(TARGET).hex"
	@$(OBJCOPY) -O binary "$@" "$(OUTPUT_DIR)/$(TARGET).bin"
	@echo "Generated: $(TARGET).hex, $(TARGET).bin"
endif

# Compile source files
$(OUTPUT_DIR)/%.o: %.c $(HEADERS) | $(OUTPUT_DIR)
	@echo "[CC] $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c "$<" -o "$@"

# Run (simulation only)
run: $(TARGET_EXE)
ifeq ($(MODE),simulation)
	@echo "Starting $(TARGET)..."
	@./$(TARGET_EXE)
else
	@echo "Run target only available in simulation mode"
	@echo "Use 'make flash' to deploy to MCU"
endif

# Flash MCU (MCU mode only)
flash: $(TARGET_EXE)
ifeq ($(MODE),mcu)
	@echo "Flashing MCU..."
	@st-flash write "$(OUTPUT_DIR)/$(TARGET).bin" 0x8000000
	@echo "Flash complete"
else
	@echo "Flash target only available in MCU mode"
endif

# Quick build targets
simulation:
	@$(MAKE) MODE=simulation

mcu:
	@$(MAKE) MODE=mcu

# Build configurations
debug:
	@$(MAKE) BUILD=debug

release:
	@$(MAKE) BUILD=release

profile:
	@$(MAKE) BUILD=profile

# Combined targets
sim-debug:
	@$(MAKE) MODE=simulation BUILD=debug

sim-release:
	@$(MAKE) MODE=simulation BUILD=release

mcu-debug:
	@$(MAKE) MODE=mcu BUILD=debug

mcu-release:
	@$(MAKE) MODE=mcu BUILD=release

# Development targets
test: sim-debug
	@echo "Running basic test..."
	@./$(OUTPUT_DIR)/$(TARGET)$(EXE_EXT) --help

install: $(TARGET_EXE)
	@echo "Installing $(TARGET)..."
	@cp "$(TARGET_EXE)" "./$(TARGET)$(EXE_EXT)"
	@echo "Installed: ./$(TARGET)$(EXE_EXT)"

# Clean targets
clean:
	@echo "Cleaning build files..."
	@rm -rf build 2>/dev/null || echo "Nothing to clean"
	@rm -f $(TARGET)$(EXE_EXT) 2>/dev/null || echo "Nothing to clean"
	@echo "Clean completed"

clean-sim:
	@echo "Cleaning simulation builds..."
	@rm -rf build/*/simulation 2>/dev/null || echo "Nothing to clean"

clean-mcu:
	@echo "Cleaning MCU builds..."
	@rm -rf build/*/mcu 2>/dev/null || echo "Nothing to clean"

rebuild: clean all

# Information
info:
	@echo "Build Configuration:"
	@echo "  Project:   $(TARGET) v$(VERSION)"
	@echo "  Mode:      $(MODE)"
	@echo "  Build:     $(BUILD)"
	@echo "  Compiler:  $(CC)"
	@echo "  Target:    $(TARGET_EXE)"
	@echo "  Sources:   $(words $(ALL_SRCS)) files"

status: info
	@echo ""
	@echo "Available Executables:"
	@find build -name "$(TARGET)*" -type f 2>/dev/null | sort || echo "  None found"

# Help
help:
	@echo "Device Simulator v$(VERSION) Build System"
	@echo ""
	@echo "Basic Usage:"
	@echo "  make                    # Build simulation mode (debug)"
	@echo "  make MODE=mcu          # Build for MCU target"
	@echo "  make BUILD=release     # Build optimized version"
	@echo ""
	@echo "Quick Targets:"
	@echo "  make simulation        # Build for PC simulation"
	@echo "  make mcu              # Build for MCU deployment"
	@echo "  make sim-release      # Optimized simulation build"
	@echo "  make mcu-release      # Optimized MCU build"
	@echo ""
	@echo "Actions:"
	@echo "  make run              # Build and run (simulation only)"
	@echo "  make flash            # Build and flash MCU"
	@echo "  make test             # Build and basic test"
	@echo "  make install          # Install to current directory"
	@echo ""
	@echo "Maintenance:"
	@echo "  make clean            # Remove all build files"
	@echo "  make rebuild          # Clean and build"
	@echo "  make info             # Show build configuration"
	@echo "  make status           # Show available builds"
	@echo ""
	@echo "Build Options:"
	@echo "  BUILD=debug           # Debug symbols, no optimization"
	@echo "  BUILD=release         # Optimized for production"
	@echo "  BUILD=profile         # Performance profiling enabled"
	@echo ""
	@echo "Target Modes:"
	@echo "  MODE=simulation       # PC simulation (default)"
	@echo "  MODE=mcu             # MCU firmware"
	@echo ""
	@echo "Examples:"
	@echo "  make MODE=simulation BUILD=release run"
	@echo "  make MODE=mcu BUILD=release flash"
	@echo "  make sim-debug && make mcu-release"

# Check dependencies
check-deps:
	@echo "Checking build dependencies..."
ifeq ($(MODE),simulation)
	@$(CC) --version >/dev/null 2>&1 || (echo "Error: GCC not found"; exit 1)
	@echo "GCC compiler available"
else
	@$(CC) --version >/dev/null 2>&1 || (echo "Error: ARM GCC not found"; exit 1)
	@$(OBJCOPY) --version >/dev/null 2>&1 || (echo "Error: objcopy not found"; exit 1)
	@echo "ARM toolchain available"
endif
	@echo "Dependencies OK"

# Development convenience
dev: sim-debug
	@echo "Development build ready"
	@echo "Run with: make run"

prod: sim-release mcu-release
	@echo "Production builds completed"

# Phony targets
.PHONY: all run flash clean rebuild install test help info status
.PHONY: simulation mcu debug release profile
.PHONY: sim-debug sim-release mcu-debug mcu-release
.PHONY: clean-sim clean-mcu check-deps dev prod