MOD   := it5570_fan
KDIR  ?= /lib/modules/$(shell uname -r)/build
BUILD := $(CURDIR)/build

# Kernels built with clang (e.g. CachyOS) need the LLVM toolchain for module
# builds. Detect that from the kernel's .config; on GCC kernels nothing extra
# is passed. Explicit CC=/LD=/LLVM= on the command line still override.
LLVM ?= $(shell grep -qs '^CONFIG_CC_IS_CLANG=y' $(KDIR)/.config && echo 1)
# make has built-in defaults CC=cc and LD=ld; forward them only when the
# user actually set them (command line or environment), or they would
# override the kernel Makefile's own toolchain choice under LLVM=1.
KBUILD_ARGS := $(if $(filter-out default undefined,$(origin CC)),CC=$(CC)) \
               $(if $(filter-out default undefined,$(origin LD)),LD=$(LD)) \
               $(if $(LLVM),LLVM=$(LLVM))

.DEFAULT_GOAL := help

help: ## show this help
	@echo "it5570-fan - fan control driver for the LattePanda Sigma"
	@echo ""
	@echo "Targets:"
	@awk -F':.*## ' '/^[a-z-]+:.*## /{printf "  make %-14s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

# Kbuild puts its artifacts into the M= directory, so sources and the Kbuild
# file are symlinked into build/ to keep the repo root clean.
all: $(BUILD)/Kbuild $(BUILD)/$(MOD).c ## build the module into build/
	$(MAKE) -C $(KDIR) M=$(BUILD) $(KBUILD_ARGS) modules

$(BUILD)/%: % | $(BUILD)
	ln -sf $(CURDIR)/$< $@

$(BUILD):
	mkdir -p $@

clean: ## remove the build/ directory
	rm -rf $(BUILD)

# Quick local install (not for DKMS - use 'make dkms-install' for that)
insmod: all ## build and load the module (quick local test)
	sudo insmod $(BUILD)/$(MOD).ko

rmmod: ## unload the module
	sudo rmmod $(MOD)

# DKMS helpers (run from repo checkout). DKMS_VER expands lazily so
# `git describe` only runs when a dkms target actually uses it.
DKMS_NAME := it5570-fan
DKMS_VER   = $(shell { git describe --tags --abbrev=0 2>/dev/null || echo "v0.1.0"; } | sed 's/^v//')
DKMS_SRC   = /usr/src/$(DKMS_NAME)-$(DKMS_VER)
CURVE_CONF := /etc/modprobe.d/$(MOD)-curve.conf

dkms-install: ## register with DKMS, install module and autoload config
	sudo mkdir -p $(DKMS_SRC)
	sudo cp $(MOD).c Kbuild Makefile dkms.conf $(DKMS_SRC)/
	sudo sed -i "s/@PKGVER@/$(DKMS_VER)/g" $(DKMS_SRC)/dkms.conf
	sudo dkms add $(DKMS_NAME)/$(DKMS_VER)
	sudo dkms build $(DKMS_NAME)/$(DKMS_VER)
	sudo dkms install $(DKMS_NAME)/$(DKMS_VER)
	sudo install -Dm644 $(MOD).conf /etc/modules-load.d/$(MOD).conf
	@if [ -e $(CURVE_CONF) ]; then \
		echo "Kept existing $(CURVE_CONF)"; \
	else \
		sudo install -Dm644 $(MOD)-curve.conf $(CURVE_CONF); \
	fi
	@echo "Edit $(CURVE_CONF) to persist a fan curve (uncomment its options line)."

dkms-remove: ## remove the module from DKMS and delete autoload config
	sudo dkms remove $(DKMS_NAME)/$(DKMS_VER) --all
	sudo rm -rf $(DKMS_SRC)
	sudo rm -f /etc/modules-load.d/$(MOD).conf
	@# cmp also keeps an unmodified copy of an OLDER template revision -
	@# acceptable: never delete a file we cannot prove we'd recreate.
	@if cmp -s $(MOD)-curve.conf $(CURVE_CONF); then \
		sudo rm -f $(CURVE_CONF); \
		echo "Removed $(CURVE_CONF)"; \
	elif [ -e $(CURVE_CONF) ]; then \
		echo "Kept $(CURVE_CONF) (modified - your curve settings survive)"; \
	fi

.PHONY: help all clean insmod rmmod dkms-install dkms-remove
