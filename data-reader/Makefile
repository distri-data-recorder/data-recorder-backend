# ====== Config ======
TARGET     := serialread
SRC        := serialread.c protocol/protocol.c protocol/io_buffer.c shared_memory.c
INC_DIRS   := protocol
CC         := gcc

# Build type: make 或 make BUILD=release/debug
BUILD      ?= debug

# ====== Flags ======
CFLAGS_COMMON := -std=c11 -Wall -Wextra -Wno-unused-parameter -I$(INC_DIRS) -MMD -MP
ifeq ($(BUILD),release)
    CFLAGS := $(CFLAGS_COMMON) -O2
else
    CFLAGS := $(CFLAGS_COMMON) -O0 -g
endif

LDFLAGS    :=
LDLIBS     := -lkernel32 -luser32

# ====== Derived ======
OBJ        := $(SRC:.c=.o)
DEP        := $(OBJ:.o=.d)
EXE        := $(TARGET).exe

# ====== Rules ======
.PHONY: all run clean rebuild

all: $(EXE)

$(EXE): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(EXE)
	./$(EXE)

clean:
ifeq ($(OS),Windows_NT)
	-del /Q $(subst /,\,$(OBJ)) $(subst /,\,$(DEP)) $(EXE) 2>nul
else
	$(RM) $(OBJ) $(DEP) $(EXE)
endif

rebuild: clean all

# 自动依赖
-include $(DEP)