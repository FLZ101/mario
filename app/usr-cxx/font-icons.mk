FONT_ICONS_DIR := $(ROOT)/usr-cxx/font-icons
FONT_ICONS_OBJECT := $(BUILD_DIR)/font-icons.o
RULES += $(lastword $(MAKEFILE_LIST))
CXXFLAGS += -I$(FONT_ICONS_DIR) -I$(ROOT)/gui/libmario-ui/include
LDFLAGS += $(FONT_ICONS_OBJECT)

include $(ROOT)/usr-cxx/fltk-app.mk

$(EXE): $(FONT_ICONS_OBJECT)

$(FONT_ICONS_OBJECT): $(FONT_ICONS_DIR)/font-icons.cpp $(RULES) $(TOOLCHAIN_STAMP)
	mkdir -p "$(@D)"
	cd "$(BUILD_DIR)" && $(CXX) $(CXXFLAGS) -o "$@" "$<"

-include $(FONT_ICONS_OBJECT:.o=.d)
