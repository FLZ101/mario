PODMAN ?= podman
BUILD_IMAGE ?=
BUILD_ENV_FILE := .build-env
CURRENT_BUILD_IMAGE := $(if $(wildcard $(BUILD_ENV_FILE)),$(shell cat $(BUILD_ENV_FILE)))

BUILD_IMAGE_TAG := localhost/mario-build:$(subst :,-,$(BUILD_IMAGE))
BUILD_CONTAINERFILE := .github/ci/Containerfile.$(subst :,-,$(BUILD_IMAGE))
BUILD_IMAGE_STAMP := .build-image-$(subst /,-,$(subst :,-,$(BUILD_IMAGE_TAG)))
BUILD_IMAGE_STAMPS := .build-image-*

.PHONY: all container clean-container clean-container-env clean-containers

all: container

container:
	@test -n '$(BUILD_IMAGE)' || { echo 'BUILD_IMAGE is required, e.g. make -f Makefile.container.mk BUILD_IMAGE=fedora:39' >&2; exit 1; }
	$(PODMAN) build -t "$(BUILD_IMAGE_TAG)" -f "$(BUILD_CONTAINERFILE)" .
	@touch $(BUILD_IMAGE_STAMP)
	@printf '%s\n' '$(BUILD_IMAGE)' > $(BUILD_ENV_FILE)

clean-container:
	@if [ -n '$(BUILD_IMAGE)' ] && [ '$(BUILD_IMAGE)' != '$(CURRENT_BUILD_IMAGE)' ]; then \
		$(PODMAN) image rm "$(BUILD_IMAGE_TAG)" || true; \
		$(RM) $(BUILD_IMAGE_STAMP); \
	elif [ -n '$(BUILD_IMAGE)' ] && [ '$(BUILD_IMAGE)' = '$(CURRENT_BUILD_IMAGE)' ]; then \
		echo "not removing in-use image $(BUILD_IMAGE)"; \
	fi

clean-container-env:
	$(RM) $(BUILD_ENV_FILE)

clean-containers:
	$(PODMAN) images --format '{{.Repository}}:{{.Tag}}' \
		| awk '/^localhost\/mario-build:/ { print }' \
		| xargs -r $(PODMAN) image rm
	$(RM) $(BUILD_IMAGE_STAMPS) $(BUILD_ENV_FILE)
