PKG_CONFIG ?= pkg-config
LIBUSB_CFLAGS = $(subst -I,-isystem ,$(shell $(PKG_CONFIG) --cflags libusb-1.0))
LIBUSB_LIBS = $(shell $(PKG_CONFIG) --libs libusb-1.0)
LIBUSB_DYLIB = $(shell $(PKG_CONFIG) --variable=libdir libusb-1.0)/libusb-1.0.dylib
FFMPEG_CFLAGS = $(subst -I,-isystem ,$(shell $(PKG_CONFIG) --cflags libavformat libavcodec libavutil libavfilter))
FFMPEG_LIBS = $(shell $(PKG_CONFIG) --libs libavformat libavcodec libavutil libavfilter)
CFLAGS ?= -O2
# Diagnose our code without reporting warnings from third-party inline headers.
WARNING_FLAGS ?= -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow -Wconversion \
	-Wstrict-prototypes -Wmissing-prototypes -Wundef -Wvla -Wimplicit-fallthrough \
	-Werror=implicit-function-declaration -Werror=return-type
CAPTURE_INCLUDES = -Isrc/capture
RAW_SOURCES = $(addprefix src/capture/,cx231xx-raw.c cx231xx-video.c cx231xx-writer.c cx231xx-record.c cx231xx-view.c)
RAW_HEADERS = $(addprefix src/capture/,cx231xx-raw.h cx231xx-video.h cx231xx-writer.h cx231xx-record.h cx231xx-view.h)
CAPTURE_SOURCES = $(addprefix src/capture/,cx231xx-init.c cx231xx-picture.c cx231xx-usb.c cx231xx-session.c) $(RAW_SOURCES)
CAPTURE_HEADERS = $(addprefix src/capture/,cx231xx-init.h cx231xx-monitor.h cx231xx-picture.h cx231xx-usb.h cx231xx-session.h) $(RAW_HEADERS)
CLI_SOURCE = src/cli/usblive2-probe.c
LINUX_FIRMWARE = firmware/v4l-cx231xx-avcore-01.fw firmware/LICENSE.conexant
MONITOR_APP = dist/USB-Live2 Monitor.app
CAPTURE_APP_EXECUTABLE = $(MONITOR_APP)/Contents/MacOS/USB-Live2 Monitor
DIST_NAME = usblive2-macos-source
DIST_FILES = Makefile .gitignore README.md COPYING assets/usblive2.svg scripts/build-icons.swift \
	$(LINUX_FIRMWARE) firmware/README.md \
	scripts/verify-mkv.py scripts/export-vhs-hd.sh scripts/vhs-qtgmc.vpy \
	$(CLI_SOURCE) $(CAPTURE_SOURCES) $(CAPTURE_HEADERS) tests/cx231xx-raw-test.c tests/cx231xx-picture-test.c \
	tests/cx231xx-firmware-test.c tests/scripts-test.py src/monitor/Monitor.m src/monitor/Info.plist \
	src/camera/Installer.h src/camera/Installer.m src/camera/Extension.swift src/camera/Publisher.c src/camera/build.sh \
	scripts/bundle-dylibs.py \
	src/camera/Capture.c src/camera/Capture.h src/camera/Fields.c src/camera/Fields.h

usblive2-probe: $(CLI_SOURCE) $(CAPTURE_SOURCES) $(CAPTURE_HEADERS) Makefile
	$(CC) $(CPPFLAGS) $(CAPTURE_INCLUDES) $(LIBUSB_CFLAGS) $(FFMPEG_CFLAGS) $(CFLAGS) -std=c11 $(WARNING_FLAGS) $(LDFLAGS) -o $@ $(CLI_SOURCE) $(CAPTURE_SOURCES) $(LIBUSB_LIBS) $(FFMPEG_LIBS) $(LDLIBS)
	codesign --force --sign - $@

build:
	mkdir -p $@

build/cx231xx-raw-test: tests/cx231xx-raw-test.c $(RAW_SOURCES) $(CAPTURE_HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CAPTURE_INCLUDES) $(FFMPEG_CFLAGS) $(CFLAGS) -UNDEBUG -std=c11 $(WARNING_FLAGS) $(LDFLAGS) -o $@ tests/cx231xx-raw-test.c $(RAW_SOURCES) $(FFMPEG_LIBS) $(LDLIBS)

build/cx231xx-picture-test: tests/cx231xx-picture-test.c src/capture/cx231xx-picture.c src/capture/cx231xx-picture.h Makefile | build
	$(CC) $(CPPFLAGS) $(CAPTURE_INCLUDES) $(CFLAGS) -UNDEBUG -std=c11 $(WARNING_FLAGS) $(LDFLAGS) -o $@ tests/cx231xx-picture-test.c src/capture/cx231xx-picture.c $(LDLIBS)

build/cx231xx-firmware-test: tests/cx231xx-firmware-test.c src/capture/cx231xx-init.c src/capture/cx231xx-picture.c $(CAPTURE_HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CAPTURE_INCLUDES) $(LIBUSB_CFLAGS) $(CFLAGS) -UNDEBUG -std=c11 $(WARNING_FLAGS) $(LDFLAGS) -o $@ tests/cx231xx-firmware-test.c src/capture/cx231xx-init.c src/capture/cx231xx-picture.c $(LIBUSB_LIBS) $(LDLIBS)

.PHONY: check clean dist camera monitor capture-app-executable print-warning-flags icons
print-warning-flags:
	@printf '%s\n' '$(WARNING_FLAGS)'

check: usblive2-probe build/cx231xx-raw-test build/cx231xx-picture-test build/cx231xx-firmware-test
	./build/cx231xx-raw-test
	./build/cx231xx-picture-test
	./build/cx231xx-firmware-test
	python3 tests/scripts-test.py
	bash -n scripts/export-vhs-hd.sh
	sh -n src/camera/build.sh
	@./usblive2-probe --invalid >/dev/null 2>&1; test $$? -eq 2
	@./usblive2-probe --init /dev/null extra >/dev/null 2>&1; test $$? -eq 2
	@./usblive2-probe --help | grep -q -- '--firmware PATH'
	@./usblive2-probe --record svideo 0 unused >/dev/null 2>&1; test $$? -eq 2
	@./usblive2-probe --firmware /dev/null --record svideo 6 unused >/dev/null 2>&1; test $$? -eq 1
	@./usblive2-probe --stream /dev/null hdmi 5 >/dev/null 2>&1; test $$? -eq 2
	@./usblive2-probe --stream /dev/null composite 0 >/dev/null 2>&1; test $$? -eq 2
	@./usblive2-probe --stream /dev/null svideo 3601 >/dev/null 2>&1; test $$? -eq 2
	@./usblive2-probe --dump /dev/null composite 6 unused >/dev/null 2>&1; test $$? -eq 2
	@./usblive2-probe --record /dev/null composite 0 unused >/dev/null 2>&1; test $$? -eq 2
	@./usblive2-probe --record /dev/null composite 6 unused >/dev/null 2>&1; test $$? -eq 1
	@./usblive2-probe --preview --camera --record /dev/null svideo 6 unused >/dev/null 2>&1; test $$? -eq 1
	@./usblive2-probe --camera --view /dev/null svideo >/dev/null 2>&1; test $$? -eq 1
	@./usblive2-probe --codec prores --record /dev/null svideo 6 unused >/dev/null 2>&1; test $$? -eq 1
	@./usblive2-probe --codec raw --record /dev/null svideo 6 unused >/dev/null 2>&1; test $$? -eq 1
	@./usblive2-probe --codec h264 --stream /dev/null svideo >/dev/null 2>&1; test $$? -eq 2
	@./usblive2-probe --codec unknown --record /dev/null svideo unused >/dev/null 2>&1; test $$? -eq 2
	@./usblive2-probe --view /dev/null svideo >/dev/null 2>&1; test $$? -eq 2
	@output=$$(./usblive2-probe --preview --live-peaking --view /dev/null svideo </dev/null 2>&1); result=$$?; \
	  test $$result -eq 2 && case "$$output" in *"requires --preview --view and an interactive terminal."*) true;; *) false;; esac
	@./usblive2-probe --picture hue=128 --record /dev/null svideo unused >/dev/null 2>&1; test $$? -eq 2
	@./usblive2-probe --picture peaking=4 --picture brightness=-128 --record /dev/null svideo unused >/dev/null 2>&1; test $$? -eq 1
	@./usblive2-probe --obs --view /dev/null svideo >/dev/null 2>&1; test $$? -eq 2
	@output=$$(./usblive2-probe --init /dev/null 2>&1); result=$$?; \
	  test $$result -eq 1 && case "$$output" in *"expected exactly 16382 readable firmware bytes."*) true;; *) false;; esac

clean:
	rm -f usblive2-probe
	rm -rf build

build/build-icons: scripts/build-icons.swift Makefile | build
	xcrun swiftc -swift-version 5 -warnings-as-errors -module-cache-path build/swift-cache -O $< -o $@

icons: build/icons/.stamp

build/icons/.stamp: assets/usblive2.svg build/build-icons
	./build/build-icons $< build/icons
	touch $@

monitor: icons
	mkdir -p "$(MONITOR_APP)/Contents/MacOS" "$(MONITOR_APP)/Contents/Frameworks" "$(MONITOR_APP)/Contents/Resources/firmware"
	$(MAKE) capture-app-executable
	cp -L "$(LIBUSB_DYLIB)" "$(MONITOR_APP)/Contents/Frameworks/libusb-1.0.dylib"
	# Rewriting load commands breaks the linker's signature. The bundle is signed below.
	codesign --remove-signature "$(CAPTURE_APP_EXECUTABLE)"
	install_name_tool -change "$$(otool -D "$(LIBUSB_DYLIB)" | tail -1)" \
		@executable_path/../Frameworks/libusb-1.0.dylib "$(MONITOR_APP)/Contents/MacOS/USB-Live2 Monitor"
	codesign --force --sign - "$(MONITOR_APP)/Contents/Frameworks/libusb-1.0.dylib"
	cp src/monitor/Info.plist "$(MONITOR_APP)/Contents/Info.plist"
	cp build/icons/USB-Live2.icns "$(MONITOR_APP)/Contents/Resources/"
	cp $(LINUX_FIRMWARE) "$(MONITOR_APP)/Contents/Resources/firmware/"
	codesign --force --sign - "$(MONITOR_APP)"

capture-app-executable:
	$(CC) $(CPPFLAGS) $(CAPTURE_INCLUDES) $(LIBUSB_CFLAGS) $(FFMPEG_CFLAGS) $(CFLAGS) -std=c11 $(WARNING_FLAGS) \
		-fobjc-arc $(LDFLAGS) -Wl,-headerpad_max_install_names -o "$(CAPTURE_APP_EXECUTABLE)" \
		$(if $(CAMERA_INSTALLER),-DUSBLIVE2_CAMERA_APP -Isrc/camera src/camera/Installer.m -framework SystemExtensions) \
		src/monitor/Monitor.m $(CAPTURE_SOURCES) $(LIBUSB_LIBS) $(FFMPEG_LIBS) $(LDLIBS) \
		-framework AppKit -framework AVFoundation -framework CoreMedia -framework CoreVideo -framework QuartzCore

camera:
	PKG_CONFIG="$(PKG_CONFIG)" WARNING_FLAGS="$(WARNING_FLAGS)" ./src/camera/build.sh

dist: dist/$(DIST_NAME).tar.gz

dist/$(DIST_NAME).tar.gz: $(DIST_FILES)
	mkdir -p dist
	COPYFILE_DISABLE=1 tar -czf $@.tmp -s ',^,$(DIST_NAME)/,' $(DIST_FILES)
	mv $@.tmp $@
