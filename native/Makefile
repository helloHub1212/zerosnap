# Native camera UI for the Pi Zero.
#
# Nothing here links against SDL, X11, freetype or libjpeg: the renderer talks
# to /dev/fb1 directly and all text is pre-rendered at build time.
#
#   make            build zerosnap-ui and zerosnap-bench
#   make fonts      regenerate the pre-rendered text atlas
#   make test       run the host unit tests
#   make screenshots  render the README images (needs Pillow)
#   make install    copy the binaries to /opt/zerosnap (needs root)

CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE
# 32-bit ARM: without LFS, stat() fails with EOVERFLOW on large inode numbers
# and the gallery silently finds no media at all.
CFLAGS += -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE
CFLAGS += -Isrc
LDLIBS ?= -lm -lrt -lpthread

PREFIX ?= /opt/zerosnap

CORE = src/fb.c src/gfx.c src/text.c src/text_data.c src/input.c src/ipc.c \
       src/media.c src/touchcal.c src/ui.c
HEADERS = src/cam.h src/fb.h src/gfx.h src/text.h src/text_data.h src/input.h \
          src/ipc.h src/media.h src/touchcal.h src/ui.h

all: zerosnap-ui zerosnap-bench

zerosnap-ui: $(CORE) src/main.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(CORE) src/main.c $(LDLIBS)

zerosnap-bench: src/bench.c src/fb.c src/gfx.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ src/bench.c src/fb.c src/gfx.c $(LDLIBS)

fonts:
	python3 tools/gen_font.py

test: all
	bash tests/run_text_test.sh
	bash tests/run_input_test.sh
	bash tests/run_ipc_test.sh
	bash tests/run_media_test.sh
	bash tests/run_touchcal_test.sh
	bash tests/run_cmd_test.sh
	bash tests/run_ui_e2e.sh
	@echo "ALL TESTS PASSED"

screenshot: zerosnap-ui
	./zerosnap-ui --fb /tmp/zerosnap-fb.img --screenshot /tmp/zerosnap-ui.ppm

screenshots: zerosnap-ui
	python3 tools/make_screenshots.py

install: zerosnap-ui zerosnap-bench
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 zerosnap-ui $(DESTDIR)$(PREFIX)/bin/zerosnap-ui
	install -m 0755 zerosnap-bench $(DESTDIR)$(PREFIX)/bin/zerosnap-bench

clean:
	rm -f zerosnap-ui zerosnap-bench
	rm -rf build

.PHONY: all fonts test screenshot screenshots install clean
