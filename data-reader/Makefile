# ====== 项目配置 ======
TARGET     := serialread
VERSION    := 1.0

# 源文件和包含目录
SRCS       := serialread.c protocol/protocol.c protocol/io_buffer.c shared_memory.c ipc_communication.c
INC_DIRS   := protocol
CC         := gcc

# 构建类型
BUILD      ?= debug

# ====== 平台检测 ======
ifeq ($(OS),Windows_NT)
    PLATFORM := windows
    EXE_EXT  := .exe
    LIBS     := -lkernel32 -luser32 -lws2_32
    RMDIR    := rm -rf
    MKDIR    := mkdir -p
    SEP      := /
else
    PLATFORM := linux
    EXE_EXT  := 
    LIBS     := -lpthread -lm
    RMDIR    := rm -rf
    MKDIR    := mkdir -p
    SEP      := /
endif

# ====== 输出目录 ======
BUILD_DIR  := build$(SEP)$(PLATFORM)$(SEP)$(BUILD)
TARGET_EXE := $(BUILD_DIR)$(SEP)$(TARGET)$(EXE_EXT)

# 对象文件和依赖文件
OBJS       := $(SRCS:%.c=$(BUILD_DIR)$(SEP)%.o)
DEPS       := $(OBJS:.o=.d)

# ====== 编译选项 ======
CFLAGS_COMMON := -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-unknown-pragmas
CFLAGS_COMMON += -DVERSION=\"$(VERSION)\" -DPLATFORM_$(shell echo $(PLATFORM) | tr a-z A-Z)=1
CFLAGS_COMMON += $(addprefix -I,$(INC_DIRS)) -MMD -MP

ifeq ($(BUILD),release)
    CFLAGS := $(CFLAGS_COMMON) -O2 -DNDEBUG
else ifeq ($(BUILD),profile)
    CFLAGS := $(CFLAGS_COMMON) -O2 -g -pg
else
    CFLAGS := $(CFLAGS_COMMON) -O0 -g -DDEBUG
endif

LDFLAGS := $(LIBS)

# 颜色输出
ifndef NO_COLOR
    RED    := \033[31m
    GREEN  := \033[32m
    YELLOW := \033[33m
    BLUE   := \033[34m
    RESET  := \033[0m
endif

# ====== 构建规则 ======
.PHONY: all clean rebuild run run-socket test debug release profile help info

all: $(TARGET_EXE)

# 创建构建目录
$(BUILD_DIR):
	@echo "$(BLUE)[INFO]$(RESET) Creating build directory: $(BUILD_DIR)"
	@$(MKDIR) $(BUILD_DIR)
	@$(MKDIR) $(BUILD_DIR)/protocol

# 链接可执行文件
$(TARGET_EXE): $(OBJS) | $(BUILD_DIR)
	@echo "$(GREEN)[LINK]$(RESET) $@"
	@$(CC) $(OBJS) -o "$@" $(LDFLAGS)
	@echo "$(GREEN)[DONE]$(RESET) Build completed: $(TARGET_EXE)"

# 编译规则 - 统一使用Unix风格
$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	@echo "$(YELLOW)[CC]$(RESET) $<"
	@$(CC) $(CFLAGS) -c "$<" -o "$@"

# ====== 运行目标 ======
run: $(TARGET_EXE)
	@echo "$(BLUE)[RUN]$(RESET) Starting $(TARGET) (Serial mode, COM7)"
	@cd . && "$(TARGET_EXE)"

run-socket: $(TARGET_EXE)
	@echo "$(BLUE)[RUN]$(RESET) Starting $(TARGET) (Socket mode, 127.0.0.1:9001)"
	@cd . && "$(TARGET_EXE)" -s

# 测试与test-sender的连接
test: $(TARGET_EXE)
	@echo "$(BLUE)[TEST]$(RESET) Testing connection to test-sender..."
	@echo "Make sure test-sender is running on 127.0.0.1:9001"
	@cd . && "$(TARGET_EXE)" -s 127.0.0.1 9001

# 测试不同COM口
test-com3: $(TARGET_EXE)
	@echo "$(BLUE)[TEST]$(RESET) Testing COM3 connection..."
	@cd . && "$(TARGET_EXE)" 3

test-com7: $(TARGET_EXE)
	@echo "$(BLUE)[TEST]$(RESET) Testing COM7 connection..."
	@cd . && "$(TARGET_EXE)" 7

# ====== 构建配置 ======
debug:
	@$(MAKE) BUILD=debug

release:
	@$(MAKE) BUILD=release

profile:
	@$(MAKE) BUILD=profile

# ====== 清理 ======
clean:
	@echo "$(RED)[CLEAN]$(RESET) Removing build files..."
	@$(RMDIR) build 2>/dev/null || echo "Nothing to clean"
	@rm -f ./$(TARGET)$(EXE_EXT) 2>/dev/null || echo ""
	@echo "$(RED)[DONE]$(RESET) Clean completed"

# 重新构建
rebuild: clean all

# ====== 安装到当前目录 ======
install: $(TARGET_EXE)
	@echo "$(GREEN)[INSTALL]$(RESET) Installing $(TARGET)..."
	@cp "$(TARGET_EXE)" "./$(TARGET)$(EXE_EXT)"
	@echo "$(GREEN)[DONE]$(RESET) Installed: ./$(TARGET)$(EXE_EXT)"

# ====== 信息显示 ======
info:
	@echo "$(BLUE)Build Information:$(RESET)"
	@echo "  Target:    $(TARGET) v$(VERSION)"
	@echo "  Platform:  $(PLATFORM)"
	@echo "  Config:    $(BUILD)"
	@echo "  Compiler:  $(CC)"
	@echo "  Output:    $(TARGET_EXE)"
	@echo "  Sources:   $(SRCS)"
	@echo "  Objects:   $(OBJS)"
	@echo "  Includes:  $(INC_DIRS)"

# 帮助信息
help:
	@echo "$(BLUE)Available Targets:$(RESET)"
	@echo "  all         - Build the program (default)"
	@echo "  debug       - Build debug version"
	@echo "  release     - Build release version"
	@echo "  profile     - Build profiling version"
	@echo "  run         - Build and run (serial COM7)"
	@echo "  run-socket  - Build and run (socket mode)"
	@echo "  test        - Test connection to test-sender"
	@echo "  test-com3   - Test serial COM3 connection"
	@echo "  test-com7   - Test serial COM7 connection"
	@echo "  install     - Install to current directory"
	@echo "  clean       - Remove all build files"
	@echo "  rebuild     - Clean and rebuild"
	@echo "  info        - Show build information"
	@echo "  help        - Show this help message"
	@echo ""
	@echo "$(BLUE)Build Options:$(RESET)"
	@echo "  BUILD={debug|release|profile} - Build configuration"
	@echo "  NO_COLOR=1 - Disable colored output"
	@echo ""
	@echo "$(BLUE)Usage Examples:$(RESET)"
	@echo "  make                    # Build debug version"
	@echo "  make BUILD=release      # Build release version"
	@echo "  make run                # Run with COM7"
	@echo "  make run-socket         # Run in socket mode"
	@echo "  make test               # Test with test-sender"
	@echo "  make clean rebuild      # Clean rebuild"
	@echo ""
	@echo "$(BLUE)Connection Examples:$(RESET)"
	@echo "  ./$(TARGET_EXE)                    # COM7 (default)"
	@echo "  ./$(TARGET_EXE) 3                  # COM3"
	@echo "  ./$(TARGET_EXE) -s                 # Socket 127.0.0.1:9001"
	@echo "  ./$(TARGET_EXE) -s 192.168.1.100   # Socket 192.168.1.100:9001"
	@echo "  ./$(TARGET_EXE) -s 192.168.1.100 8080  # Socket 192.168.1.100:8080"

# ====== 依赖管理 ======
-include $(DEPS)

.PHONY: test-com3 test-com7 install