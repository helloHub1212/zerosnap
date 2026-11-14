# ZeroSnap - top level convenience targets.
#
#   make            build the native UI for this machine
#   make test       run every host-side test
#   make install    install on the Pi (sudo)
#   make screenshots  render the README screenshots from the current build

NATIVE = native
PYTHON ?= python3

.PHONY: all build test install screenshots clean arm

all: build

build:
	$(MAKE) -C $(NATIVE)

test:
	$(MAKE) -C $(NATIVE) test PYTHON=$(PYTHON)

screenshots:
	$(MAKE) -C $(NATIVE) screenshots

install:
	bash install/install.sh

# Cross-compile for the Pi from a PC with the SD card mounted:
#   sudo make arm ROOT=/mnt/card
arm:
	bash $(NATIVE)/build-arm.sh --root $(ROOT)

clean:
	$(MAKE) -C $(NATIVE) clean
