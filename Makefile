MOD   := it5570_fan
KDIR  ?= /lib/modules/$(shell uname -r)/build
BUILD := $(CURDIR)/build

# CachyOS and other Clang-based kernel builds pass CC/LD on the command line.
# When building against a GCC kernel the variables are simply not set and
# the kernel Kbuild picks up its own defaults, so this works everywhere.
KBUILD_ARGS := $(if $(CC),CC=$(CC)) $(if $(LD),LD=$(LD)) $(if $(LLVM),LLVM=$(LLVM))

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
DKMS_VER   = $(shell git describe --tags --abbrev=0 2>/dev/null || echo "0.1.0")
DKMS_SRC   = /usr/src/$(DKMS_NAME)-$(DKMS_VER)

dkms-install: ## register with DKMS, install module and autoload config
	sudo mkdir -p $(DKMS_SRC)
	sudo cp $(MOD).c Kbuild Makefile dkms.conf $(DKMS_SRC)/
	sudo sed -i "s/@PKGVER@/$(DKMS_VER)/g" $(DKMS_SRC)/dkms.conf
	sudo dkms add $(DKMS_NAME)/$(DKMS_VER)
	sudo dkms build $(DKMS_NAME)/$(DKMS_VER)
	sudo dkms install $(DKMS_NAME)/$(DKMS_VER)
	sudo install -Dm644 $(MOD).conf /etc/modules-load.d/$(MOD).conf

dkms-remove: ## remove the module from DKMS and delete autoload config
	sudo dkms remove $(DKMS_NAME)/$(DKMS_VER) --all
	sudo rm -rf $(DKMS_SRC)
	sudo rm -f /etc/modules-load.d/$(MOD).conf

.PHONY: help all clean insmod rmmod dkms-install dkms-remove
