SHELL := /bin/bash

ROOT := $(shell pwd)

COMPILER_RT_SRC_DIR := src/lib/compiler-rt
COMPILER_RT_INC_DIR := src/include
COMPILER_RT_OBJ_DIR := util/compiler-rt
COMPILER_RT_SRCS := $(wildcard $(COMPILER_RT_SRC_DIR)/*.S)
COMPILER_RT_OBJS := $(patsubst $(COMPILER_RT_SRC_DIR)/%.S,$(COMPILER_RT_OBJ_DIR)/%.o,$(COMPILER_RT_SRCS))
COMPILER_RT_LIB := libcompiler_rt.a

MESSAGE := $(shell $(CC) --mario-verify 2>&1)
ifneq ($(MESSAGE),)
  $(error Not a compiler for mario)
endif
MESSAGE := $(shell $(LD) --mario-verify 2>&1)
ifneq ($(MESSAGE),)
  $(error Not a linker for mario)
endif

.PHONY: all clean

all: $(COMPILER_RT_LIB)

$(COMPILER_RT_LIB): $(COMPILER_RT_OBJS)
	ar rcs $@ $^

$(COMPILER_RT_OBJ_DIR)/%.o: $(COMPILER_RT_SRC_DIR)/%.S
	@mkdir -p $(COMPILER_RT_OBJ_DIR)
	$(CC) -c -D__KERNEL__ -I$(ROOT)/$(COMPILER_RT_INC_DIR) \
		-std=gnu11 -Wall -Werror \
		-ggdb -O0 -fno-omit-frame-pointer \
		-MMD -MP -o $@ $<

clean:
	rm -f $(COMPILER_RT_LIB) $(COMPILER_RT_OBJS) $(COMPILER_RT_OBJS:.o=.d)
