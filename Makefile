# webOS toolchain: any buildroot-nc4 SDK (same compiler name and layout).
# The launcher shipped with the nc4 build is built with the nc4 SDK from
# build/nc4/build-sdk.sh, inside the ffbuild container:
#   podman exec -u builder ffbuild make -C /src -B TC=/work/nc4/out/host app/geckotv
# The default below is an older macOS-hosted SDK (GCC 12.2, glibc 2.12.2)
# for building on the Mac; its output needs the same glibc.
TC ?= /Users/aicoder/toolchains/arm-webos-linux-gnueabi_sdk-buildroot
CC := $(TC)/bin/arm-webos-linux-gnueabi-gcc
SYSROOT := $(TC)/arm-webos-linux-gnueabi/sysroot

CFLAGS := --sysroot=$(SYSROOT) -O2 -Wall -Wextra -std=c11
LDFLAGS := --sysroot=$(SYSROOT) -lwayland-webos-client -lwayland-client -lwayland-egl -lEGL -lGLESv2 -lm -ldl
# The SDK's own wayland-scanner generates code that calls only functions the
# TV's libwayland-client (1.11 on webOS 4) has. Newer scanners emit
# wl_proxy_marshal_flags and those libraries lack it.
SCANNER := $(TC)/bin/wayland-scanner
GEN := build/gen

.PHONY: all package clean icons

all: app/smoke app/geckotv

app/smoke: src/smoke.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	chmod 755 $@

$(GEN)/wayland-webos-shell-client-protocol.h: src/webos-shell.xml
	mkdir -p $(GEN)
	$(SCANNER) client-header $< $@
$(GEN)/webos-shell-protocol.c: src/webos-shell.xml
	mkdir -p $(GEN)
	$(SCANNER) private-code $< $@
$(GEN)/webos-foreign-client-protocol.h: src/webos-foreign.xml
	mkdir -p $(GEN)
	$(SCANNER) client-header $< $@
$(GEN)/webos-foreign-protocol.c: src/webos-foreign.xml
	mkdir -p $(GEN)
	$(SCANNER) private-code $< $@

# The launcher is C. Its media helper mode (src/media-helper.cc) is C++
# with libstdc++ linked in statically and hidden so that LG's media
# library keeps binding to the TV's own libstdc++. The dynamic list exports
# the PmLog entry points the helper defines so that LG's libraries log into
# geckotv.log.
CXX := $(TC)/bin/arm-webos-linux-gnueabi-g++
CXXFLAGS := --sysroot=$(SYSROOT) -O2 -Wall -Wextra -std=gnu++17 -fvisibility=hidden
GECKOTV_OBJ := $(GEN)/geckotv.o $(GEN)/media-helper.o

$(GEN)/geckotv.o: src/geckotv.c src/media-helper.h
	mkdir -p $(GEN)
	$(CC) $(CFLAGS) -std=gnu11 -Isrc -c -o $@ $<
$(GEN)/media-helper.o: src/media-helper.cc src/media-helper.h src/media-protocol.h
	mkdir -p $(GEN)
	$(CXX) $(CXXFLAGS) -Isrc -c -o $@ $<
app/geckotv: $(GECKOTV_OBJ) src/geckotv.dynlist
	$(CXX) --sysroot=$(SYSROOT) -o $@ $(GECKOTV_OBJ) -static-libstdc++ \
		-Wl,--exclude-libs,libstdc++.a:libsupc++.a -Wl,--dynamic-list=src/geckotv.dynlist \
		-lpthread -ldl
	chmod 755 $@

icons:
	python3 scripts/make-icons.py

package: all
	python3 scripts/pack-ipk.py

clean:
	rm -f app/smoke app/geckotv dist/*.ipk
	rm -rf $(GEN)
