FLTK_DIR := $(ROOT)/usr-cxx/fltk-1.4.5-install
FLTK_LINK_LIBS := $(FLTK_DIR)/lib/libfltk.a \
	$(ROOT)/gui/libmario-ui/libmario-ui.a \
	$(ROOT)/usr/freetype-2.13.3-install/lib/libfreetype.a
CXXFLAGS += -I$(FLTK_DIR)/include
LDFLAGS += $(FLTK_LINK_LIBS)
RULES += $(lastword $(MAKEFILE_LIST))
$(EXE): $(FLTK_LINK_LIBS)
