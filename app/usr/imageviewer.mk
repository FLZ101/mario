IMAGEVIEWER_EXE := imageviewer/imageviewer.exe

# imageviewer must stay unoptimized for now. With -O1 or higher its startup
# sequence changes enough to expose a kernel bug: mario-wm's poll() spins on a
# corrupted AF_UNIX socket pair (a dangling `pair` pointer) and the whole guest
# stalls before imageviewer reaches "window ready". Keep this at -O0 until the
# socket lifetime bug is fixed in the kernel.
IMAGEVIEWER_OPTIMIZE := -O0

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
