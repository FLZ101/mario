PODMAN ?= podman
BUILD_IMAGE ?=
BUILD_ENV_FILE := .build-env

ifeq ($(BUILD_IMAGE),)
$(error BUILD_IMAGE is required, e.g. make -f Makefile.container.mk BUILD_IMAGE=fedora:39)
endif

BUILD_IMAGE_TAG := localhost/mario-build:$(subst :,-,$(BUILD_IMAGE))
BUILD_CONTAINERFILE := .github/ci/Containerfile.$(subst :,-,$(BUILD_IMAGE))
BUILD_IMAGE_STAMP := .build-image-$(subst /,-,$(subst :,-,$(BUILD_IMAGE_TAG)))

.PHONY: all container clean-container

all: container

container:
	$(PODMAN) build -t "$(BUILD_IMAGE_TAG)" -f "$(BUILD_CONTAINERFILE)" .
	@touch $(BUILD_IMAGE_STAMP)
	@printf '%s\n' '$(BUILD_IMAGE)' > $(BUILD_ENV_FILE)

clean-container:
	$(PODMAN) image rm "$(BUILD_IMAGE_TAG)" || true
	$(RM) $(BUILD_IMAGE_STAMP) $(BUILD_ENV_FILE)
