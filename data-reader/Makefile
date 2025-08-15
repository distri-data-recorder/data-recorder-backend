# ====== Config ======
TARGET     := serialread
SRC        := serialread.c protocol.c io_buffer.c
INC_DIRS   := .
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
	$(RM) $(OBJ) $(DEP) $(EXE)

rebuild: clean all

# 自动依赖
-include $(DEP)