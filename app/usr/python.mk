PYTHON_VER		:=3.13.13
PYTHON_SRC_DIR	:=Python-$(PYTHON_VER)
PYTHON_SRC_FILE	:=$(PYTHON_SRC_DIR).tgz
PYTHON_SRC_URL	:=https://www.python.org/ftp/python/$(PYTHON_VER)/$(PYTHON_SRC_FILE)
PYTHON_INSTALL_DIR	:=$(ROOT)/$(PYTHON_SRC_DIR)-install

.PHONY build:: build-python
build-python: src-python
	CC=$(CC) PYTHON_SRC_DIR=$(PYTHON_SRC_DIR) PYTHON_INSTALL_DIR=$(PYTHON_INSTALL_DIR) ./build-python.sh

src-python: $(PYTHON_SRC_DIR)/configure

$(PYTHON_SRC_DIR)/configure: | $(PYTHON_SRC_FILE)
	tar xf $(PYTHON_SRC_FILE)

$(PYTHON_SRC_FILE):
	wget -c --no-check-certificate $(PYTHON_SRC_URL) -O $(PYTHON_SRC_FILE)

.PHONY clean:: clean-python
clean-python:
	$(RM) -r $(PYTHON_INSTALL_DIR) $(PYTHON_SRC_DIR)

.PHONY clean-src:: clean-src-python
clean-src-python:
	$(RM) $(PYTHON_SRC_FILE)
