# 简化的 Makefile for test-sender (w64devkit 兼容)
TARGET     := test-sender
VERSION    := 2.0
BUILD      ?= debug

# 编译器和工具 (Unix风格，w64devkit兼容)
CC         := gcc
MKDIR      := mkdir -p
RM         := rm -f
RMDIR      := rm -rf

# 目录和文件
PROTO_DIR  := protocol
OUTPUT_DIR := build/windows/$(BUILD)
TARGET_EXE := $(OUTPUT_DIR)/$(TARGET).exe

# 源文件
MAIN_SRC   := test-sender.c
PROTO_SRCS := $(PROTO_DIR)/protocol.c $(PROTO_DIR)/io_buffer.c
HEADERS    := $(PROTO_DIR)/protocol.h $(PROTO_DIR)/io_buffer.h

# 对象文件
MAIN_OBJ   := $(OUTPUT_DIR)/test-sender.o
PROTO_OBJS := $(OUTPUT_DIR)/$(PROTO_DIR)/protocol.o $(OUTPUT_DIR)/$(PROTO_DIR)/io_buffer.o
OBJS       := $(MAIN_OBJ) $(PROTO_OBJS)

# 编译选项
CFLAGS_BASE := -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-unknown-pragmas
CFLAGS_BASE += -D_WIN32_WINNT=0x0601 -I$(PROTO_DIR)

ifeq ($(BUILD),release)
    CFLAGS := $(CFLAGS_BASE) -O2 -DNDEBUG
else
    CFLAGS := $(CFLAGS_BASE) -O0 -g -DDEBUG
endif

LDFLAGS := -lws2_32

# 默认目标
all: $(TARGET_EXE)

# 创建目录 (Unix风格)
$(OUTPUT_DIR):
	@echo "Creating directories..."
	@$(MKDIR) $(OUTPUT_DIR)
	@$(MKDIR) $(OUTPUT_DIR)/$(PROTO_DIR)

# 链接
$(TARGET_EXE): $(OBJS)
	@echo "[LINK] $@"
	@$(CC) $(OBJS) -o "$@" $(LDFLAGS)
	@echo "Build completed: $(TARGET_EXE)"

# 编译主文件
$(MAIN_OBJ): $(MAIN_SRC) $(HEADERS) | $(OUTPUT_DIR)
	@echo "[CC] $<"
	@$(CC) $(CFLAGS) -c "$<" -o "$@"

# 编译协议文件
$(OUTPUT_DIR)/$(PROTO_DIR)/protocol.o: $(PROTO_DIR)/protocol.c $(HEADERS) | $(OUTPUT_DIR)
	@echo "[CC] $<"
	@$(CC) $(CFLAGS) -c "$<" -o "$@"

$(OUTPUT_DIR)/$(PROTO_DIR)/io_buffer.o: $(PROTO_DIR)/io_buffer.c $(HEADERS) | $(OUTPUT_DIR)
	@echo "[CC] $<"
	@$(CC) $(CFLAGS) -c "$<" -o "$@"

# 运行
run: $(TARGET_EXE)
	@echo "Starting test-sender..."
	@./$(TARGET_EXE)

# 清理 (Unix风格)
clean:
	@echo "Cleaning build files..."
	@$(RMDIR) build 2>/dev/null || echo "Nothing to clean"
	@$(RM) $(TARGET).exe 2>/dev/null || echo "Nothing to clean"
	@echo "Clean completed"

# 安装到当前目录
install: $(TARGET_EXE)
	@echo "Installing $(TARGET)..."
	@cp "$(TARGET_EXE)" "./$(TARGET).exe"
	@echo "Installed: ./$(TARGET).exe"

# 构建配置
debug:
	@$(MAKE) BUILD=debug

release:
	@$(MAKE) BUILD=release

# 信息
info:
	@echo "Build Information:"
	@echo "  Target:    $(TARGET) v$(VERSION)"
	@echo "  Config:    $(BUILD)"
	@echo "  Compiler:  $(CC)"
	@echo "  Output:    $(TARGET_EXE)"
	@echo "  Objects:   $(OBJS)"

# 帮助
help:
	@echo "Available targets:"
	@echo "  all      - Build the test-sender (default)"
	@echo "  debug    - Build debug version"
	@echo "  release  - Build release version"
	@echo "  run      - Build and run the simulator"
	@echo "  install  - Install to current directory"
	@echo "  clean    - Remove all build files"
	@echo "  info     - Show build information"
	@echo "  help     - Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make BUILD=release"
	@echo "  make clean"
	@echo "  make run"

.PHONY: all run clean install debug release info help