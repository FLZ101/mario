IMAGEVIEWER_EXE := imageviewer/imageviewer.exe

# imageviewer follows the global OPTIMIZE level. Keep the variable overridable
# so the optimized path can be exercised directly with IMAGEVIEWER_OPTIMIZE=-O3.
IMAGEVIEWER_OPTIMIZE ?= $(OPTIMIZE)

.PHONY build:: build-imageviewer
build-imageviewer: $(IMAGEVIEWER_EXE)

.PHONY clean:: clean-imageviewer
clean-imageviewer:
	$(RM) $(IMAGEVIEWER_EXE) imageviewer/*.o imageviewer/*.d

$(IMAGEVIEWER_EXE): imageviewer/main.c ../include/app/gui-limits.h $(MARIO_UI_LIB)
	$(CC) -o $@ imageviewer/main.c -I$(ROOT)/../include \
		-I$(MARIO_UI_DIR)/include \
		-ggdb $(IMAGEVIEWER_OPTIMIZE) -fno-omit-frame-pointer \
		-L$(MARIO_UI_DIR) -L$(LIBPNG_INSTALL_DIR)/lib \
		-L$(ZLIB_INSTALL_DIR)/lib -static -lmario-ui -lpng16 -lz
