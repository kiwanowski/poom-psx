TARGET = poom
TYPE = ps-exe

SRCS = \
src/main.cpp \
src/render.cpp \
src/game.cpp \
src/hud.cpp \
src/pfont.cpp \
src/assets.cpp \
src/fixed.cpp \

CXXFLAGS = -std=c++20 -Isrc -O2 -fno-exceptions -fno-rtti

include third_party/nugget/psyqo/psyqo.mk

LIBRARIES += -lgcc

DATA = build/POOM.DAT

$(DATA): tools/build_assets.py tools/poom_unpack.py tools/p8_rom.py tools/poom_gfx.py
	cd tools && python3 build_assets.py

HOSTCXX ?= c++
HOSTSRC = test/harness.cpp src/fixed.cpp
HOSTDEPS = test/harness.cpp test/stub/psyqo/*.hh test/stub/psyqo/primitives/*.hh \
           src/game.cpp src/render.cpp src/fixed.cpp src/fixed.hh src/poom.hh src/data.hh \
           Makefile
TESTFLAGS = -std=c++20 -g -DPOOM_PROFILE -Isrc -Itest/stub

build/harness: $(HOSTDEPS)
	@mkdir -p build
	$(HOSTCXX) $(TESTFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-fno-omit-frame-pointer \
		-o $@ $(HOSTSRC)

build/profile: $(HOSTDEPS)
	@mkdir -p build
	$(HOSTCXX) $(TESTFLAGS) -O2 -o $@ $(HOSTSRC)

.PHONY: data iso clean-iso test profile
test: build/harness $(DATA)
	./build/harness build/POOM.DAT

profile: build/profile $(DATA)
	./build/profile --profile --worst build/POOM.DAT

data: $(DATA)

iso: all $(DATA)
	mkpsxiso -y -q -o build/poom.bin -c build/poom.cue poom.xml

clean-iso:
	rm -f build/poom.bin build/poom.cue $(DATA) build/harness build/profile
