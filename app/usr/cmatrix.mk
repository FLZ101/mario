
CMATRIX_VER		:=v2.0
CMATRIX_SRC_DIR	:=cmatrix
CMATRIX_INSTALL_DIR	:=$(ROOT)/$(CMATRIX_SRC_DIR)-install

.PHONY build:: build-cmatrix
build-cmatrix:
	CC=$(CC) CMATRIX_SRC_DIR=$(CMATRIX_SRC_DIR) CMATRIX_INSTALL_DIR=$(CMATRIX_INSTALL_DIR) ./build-cmatrix.sh

.PHONY clean:: clean-cmatrix
clean-cmatrix:
	$(RM) -r $(CMATRIX_INSTALL_DIR)
