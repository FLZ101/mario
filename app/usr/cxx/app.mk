.DEFAULT_GOAL := all

include $(ROOT)/usr/cxx/runtime.mk

CXXFLAGS := $(filter-out -std=gnu11,$(CFLAGS)) -std=c++17
-include Makefile.local

CXX_APP_BUILD := $(CXX_BUILD_DIR)/apps/$(patsubst $(ROOT)/%,%,$(CURDIR))
CXX_APP_RULES := $(ROOT)/Makefile.in $(CXX_DIR)/app.mk $(wildcard Makefile.local)
CXX_APP_EXE := $(CXX_APP_BUILD)/$(EXE)
CXX_APP_OBJS := $(addprefix $(CXX_APP_BUILD)/,$(addsuffix .o,$(wildcard *.c *.cpp)))
CXX_APP_OBJS += $(patsubst $(ROOT)/lib/%.c,$(CXX_APP_BUILD)/lib/%.c.o,$(wildcard $(ROOT)/lib/*.c))

all: $(CXX_APP_EXE)

clean:
	$(RM) -r "$(CXX_APP_BUILD)"

$(CXX_APP_EXE): $(CXX_APP_OBJS) $(CXX_LINK_DEPS) $(CXX_RUNTIME_STAMP) $(CXX_APP_RULES)
	cd "$(CXX_APP_BUILD)" && $(MARIO_CXX_CMD) -o "$@" $(CXX_APP_OBJS) $(LDFLAGS)

$(CXX_APP_BUILD)/%.cpp.o: %.cpp $(CXX_APP_RULES) $(MARIO_CXX) $(CXX_RUNTIME_STAMP)
	mkdir -p "$(@D)"
	cd "$(CXX_APP_BUILD)" && $(MARIO_CXX_CMD) $(CXXFLAGS) -o "$@" "$(abspath $<)"

$(CXX_APP_BUILD)/%.c.o: %.c $(CXX_APP_RULES)
	mkdir -p "$(@D)"
	cd "$(CXX_APP_BUILD)" && $(CC) $(CFLAGS) -o "$@" "$(abspath $<)"

$(CXX_APP_BUILD)/lib/%.c.o: $(ROOT)/lib/%.c $(CXX_APP_RULES)
	mkdir -p "$(@D)"
	cd "$(CXX_APP_BUILD)" && $(CC) $(CFLAGS) -o "$@" "$<"

-include $(CXX_APP_OBJS:.o=.d)
