# Aether OS top-level build.
#   make            build Aether binaries and tools        (build/bin)
#   make test       unit tests + integration tests
#   make busybox    build the pinned BusyBox                (build/bin/busybox)
#   make initramfs  build rootfs and initramfs image
#   make kernel     build the Linux kernel (needs KERNEL_SRC, see docs)
#   make qemu       boot the image in QEMU
#   make clean
CC      ?= gcc
WERROR  ?= -Werror
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes
CFLAGS  ?= -O2
CFLAGS  += -std=gnu11 $(WARN) $(WERROR)
LDFLAGS ?= -static

B  := build/bin
T  := build/test
C  := src/core

# Reproducible: no timestamps embedded by this build.
export SOURCE_DATE_EPOCH ?= 0

BINS := $(B)/aether-init $(B)/aetherctl $(B)/heartbeat $(B)/service-status \
        $(B)/mkinitramfs $(B)/aether-pkg $(B)/aether-net $(B)/aether-pm $(GUIBINS)

.PHONY: all test unit integration busybox rootfs initramfs kernel qemu \
        qemu-test pm-test gui gui-initramfs gui-qemu-test gui-interactive-test qemu-gui gui-headless-test compat-runtime compat-tests-build compat-initramfs \
        compat-chroot-test compat-qemu-test clean distclean
all: $(BINS)

$(B) $(T):
	mkdir -p $@

$(B)/aether-init: $(C)/init.c $(C)/service.c $(C)/service_order.c $(C)/service.h | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(C)/init.c $(C)/service.c $(C)/service_order.c
$(B)/aetherctl: $(C)/aetherctl.c $(C)/service.c $(C)/service_order.c $(C)/service.h | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(C)/aetherctl.c $(C)/service.c $(C)/service_order.c
$(B)/heartbeat: $(C)/heartbeat.c | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
$(B)/service-status: $(C)/service_status.c | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
$(B)/mkinitramfs: tools/mkinitramfs.c | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(B)/aether-pkg: src/pkg/aether-pkg.c src/pkg/sha256.c src/pkg/sha256.h | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -Isrc/pkg -o $@ src/pkg/aether-pkg.c src/pkg/sha256.c

# --- aether-pm: native .aether package manager (libsodium, pinned in config/build-deps.lock) ---
SODIUM  := build/deps/root/usr/lib/x86_64-linux-gnu/libsodium.a
PMSRC   := src/pm/util.c src/pm/version.c src/pm/verify.c src/pm/pkgfile.c src/pm/repo.c \
           src/pm/resolve.c src/pm/fsops.c src/pm/db.c
PMHDR   := $(wildcard src/pm/*.h)
PMINC   := -Isrc/pm -Ibuild/deps/root/usr/include

$(SODIUM):
	tools/fetch-build-deps.sh

$(B)/aether-pm: $(PMSRC) src/pm/main.c $(PMHDR) $(SODIUM) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) $(PMINC) -o $@ $(PMSRC) src/pm/main.c $(SODIUM)

# --- Aether GUI (compositor, shell, settings, notifications) ---
GUISRC_COMMON := src/gui/gfx.c src/gui/font8x16.c src/gui/proto.c src/gui/font.c src/gui/fontdata.c src/gui/icons.c
GUIHDR := $(wildcard src/gui/*.h)
GUIBINS := $(B)/aether-compositor $(B)/aether-shell $(B)/aether-settings $(B)/aether-notify $(B)/aether-terminal $(B)/aether-files
UICORE := $(GUISRC_COMMON) src/gui/ui.c

$(B)/aether-compositor: src/gui/compositor.c src/gui/wm.c src/gui/display.c src/gui/input.c $(GUISRC_COMMON) $(GUIHDR) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -Isrc/gui -o $@ src/gui/compositor.c src/gui/wm.c src/gui/display.c src/gui/input.c $(GUISRC_COMMON) -lm
$(B)/aether-shell: src/gui/shell.c src/gui/appmeta.c src/gui/wallpaper.c $(UICORE) $(GUIHDR) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -Isrc/gui -o $@ src/gui/shell.c src/gui/appmeta.c src/gui/wallpaper.c $(UICORE) -lm
$(B)/aether-settings: src/gui/settings.c $(UICORE) $(GUIHDR) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -Isrc/gui -o $@ src/gui/settings.c $(UICORE) -lm
$(B)/aether-notify: src/gui/notify.c $(UICORE) $(GUIHDR) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -Isrc/gui -o $@ src/gui/notify.c $(UICORE) -lm

$(B)/aether-terminal: src/gui/terminal.c src/gui/term_vt.c src/gui/input.c $(UICORE) $(GUIHDR) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -Isrc/gui -o $@ src/gui/terminal.c src/gui/term_vt.c src/gui/input.c $(UICORE) -lm
$(B)/aether-files: src/gui/files.c src/gui/fm_core.c src/gui/input.c $(UICORE) $(GUIHDR) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -Isrc/gui -o $@ src/gui/files.c src/gui/fm_core.c src/gui/input.c $(UICORE) -lm

gui: $(GUIBINS)

gui-initramfs: all gui compat-runtime compat-tests-build
	tools/build-compat-repo.sh >/dev/null
	COMPAT=1 GUI=1 ROOT=$(CURDIR)/build/rootfs-gui IMAGE=$(CURDIR)/build/aether-gui-initramfs.cpio.gz tools/build-rootfs.sh

gui-qemu-test: gui-initramfs
	tests/gui/qemu-gui-test.sh

# Interactive-desktop test in QEMU: real pointer devices (PS/2 mouse, usb-tablet, virtio-tablet), launcher, windows, terminal, files.
gui-interactive-test: gui-initramfs
	python3 tests/gui/qemu_interactive_test.py

# Boot the desktop in a QEMU window (needs a display): tools/run-qemu-gui.sh
qemu-gui: gui-initramfs
	tools/run-qemu-gui.sh

# Desktop end-to-end test without QEMU: real compositor (software display) + real clients, pixel-checked. Needs only python3.
gui-headless-test: gui
	python3 tests/gui/headless_gui_test.py $(B) --shots build/gui-shots-headless

$(B)/aether-net: src/net/aether-net.c | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

# Test-only init: no mounts, reboot(2) replaced by a message, short grace.
$(T)/aether-init-test: $(C)/init.c $(C)/service.c $(C)/service_order.c $(C)/service.h | $(T)
	$(CC) $(CFLAGS) $(LDFLAGS) -DAETHER_TEST -DAETHER_GRACE_MS=700 -o $@ \
	    $(C)/init.c $(C)/service.c $(C)/service_order.c

$(T)/test_service_order: tests/test_service_order.c $(C)/service_order.c $(C)/service.h | $(T)
	$(CC) $(CFLAGS) -o $@ tests/test_service_order.c $(C)/service_order.c
$(T)/test_service_load: tests/test_service_load.c $(C)/service.c $(C)/service_order.c $(C)/service.h | $(T)
	$(CC) $(CFLAGS) -o $@ tests/test_service_load.c $(C)/service.c $(C)/service_order.c

unit: $(T)/test_service_order $(T)/test_service_load $(T)/test_pm_unit $(T)/test_gui_unit
	$(T)/test_service_order
	$(T)/test_service_load
	$(T)/test_pm_unit
	$(T)/test_gui_unit

GUITEST_SRC := src/gui/gfx.c src/gui/font8x16.c src/gui/font.c src/gui/fontdata.c src/gui/icons.c src/gui/wallpaper.c src/gui/appmeta.c src/gui/proto.c src/gui/wm.c src/gui/input.c src/gui/term_vt.c src/gui/fm_core.c
$(T)/test_gui_unit: tests/test_gui_unit.c $(GUITEST_SRC) $(GUIHDR) | $(T)
	$(CC) -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-result -Wno-misleading-indentation -Isrc/gui -fsanitize=address,undefined -o $@ tests/test_gui_unit.c $(GUITEST_SRC) -lm

$(T)/test_pm_unit: tests/test_pm_unit.c $(PMSRC) $(PMHDR) $(SODIUM) | $(T)
	$(CC) -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-result -Wno-misleading-indentation -Wno-format-truncation $(PMINC) -fsanitize=address,undefined -o $@ tests/test_pm_unit.c $(PMSRC) $(SODIUM)

pm-test: $(B)/aether-pm
	tests/test_pm.sh

integration: all $(T)/aether-init-test
	tests/test_mkinitramfs.sh
	tests/test_pkg.sh
	tests/test_net.sh
	tests/integration-init.sh

test: unit integration pm-test

busybox:
	tools/build-busybox.sh

rootfs initramfs: all
	tools/build-rootfs.sh

kernel:
	tools/build-kernel.sh

qemu: initramfs
	tools/run-qemu.sh

qemu-test: initramfs
	tests/qemu-boot-test.sh

compat-runtime:
	tools/fetch-compat-runtime.sh

compat-tests-build:
	tests/compat/build.sh

compat-initramfs: all compat-runtime compat-tests-build
	tools/build-compat-repo.sh >/dev/null
	COMPAT=1 ROOT=$(CURDIR)/build/rootfs-compat IMAGE=$(CURDIR)/build/aether-compat-initramfs.cpio.gz tools/build-rootfs.sh

compat-chroot-test: compat-initramfs
	tests/compat/chroot-test.sh

compat-qemu-test: compat-initramfs
	tests/compat/qemu-test.sh

# clean keeps the slow-to-build BusyBox (build/bin/busybox, build/busybox-obj).
clean:
	rm -f $(BINS)
	rm -rf build/test build/rootfs build/rootfs-compat build/rootfs-gui build/aether-gui-initramfs.cpio.gz build/aether-initramfs.cpio.gz build/aether-compat-initramfs.cpio.gz build/compat-tests

distclean: clean
	rm -rf build/bin build/busybox-obj build/kernel build/kernel-obj \
	       build/kernel-src build/logs
