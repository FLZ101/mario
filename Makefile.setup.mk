MAKEOVERRIDES :=

SHELL := /bin/bash
export SHELL

RM	:= rm -f

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

PYTHON ?= $(ROOT)/.venv/bin/python
VENV_STAMP := $(ROOT)/.venv/.stamp

.PHONY: all venv precommit-install precommit precommit-all \
	clean clean-venv clean-hooks

all: precommit-install

$(VENV_STAMP): $(ROOT)/requirements.txt
	python3 -m venv --system-site-packages $(ROOT)/.venv
	$(PYTHON) -m pip install -r $(ROOT)/requirements.txt
	@touch $@

venv: $(VENV_STAMP)

precommit-install: venv
	$(PYTHON) -m pre_commit install

precommit: precommit-install
	$(PYTHON) -m pre_commit run

precommit-all: precommit-install
	$(PYTHON) -m pre_commit run --all-files

clean: clean-hooks
	$(RM) -r $(ROOT)/.venv

clean-hooks:
	-$(PYTHON) -m pre_commit uninstall
	$(RM) $(ROOT)/.git/hooks/pre-commit

clean-venv:
	$(RM) -r $(ROOT)/.venv
