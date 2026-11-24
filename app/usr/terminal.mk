TERMINAL_EXE := terminal/terminal.exe

.PHONY build:: build-terminal
build-terminal: $(TERMINAL_EXE)

.PHONY clean:: clean-terminal
clean-terminal:
	$(RM) $(TERMINAL_EXE) terminal/*.o terminal/*.d

$(TERMINAL_EXE): terminal/main.c terminal/config.c terminal/config.h terminal/vt.c \
		../include/app/gui-limits.h $(MARIO_UI_LIB)
	$(CC) -o $@ terminal/main.c terminal/config.c terminal/vt.c -I$(ROOT)/../include \
		-I$(MARIO_UI_DIR)/include \
		-ggdb $(OPTIMIZE) -fno-omit-frame-pointer \
		-L$(MARIO_UI_DIR) -L$(FREETYPE_INSTALL_DIR)/lib -static \
		-lmario-ui -lfreetype
