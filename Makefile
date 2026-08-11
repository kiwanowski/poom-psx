TARGET = poom
TYPE = ps-exe

SRCS = \
src/main.cpp \
src/render.cpp \
src/game.cpp \
src/hud.cpp \
src/assets.cpp \
src/fixed.cpp \

CXXFLAGS = -std=c++20 -Isrc -O2 -fno-exceptions -fno-rtti

include third_party/nugget/psyqo/psyqo.mk

LIBRARIES += -lgcc

DATA = build/POOM.DAT

$(DATA): tools/build_assets.py tools/poom_unpack.py tools/p8_rom.py tools/poom_gfx.py
	cd tools && python3 build_assets.py

.PHONY: data iso clean-iso
data: $(DATA)

iso: all $(DATA)
	mkpsxiso -y -q -o build/poom.bin -c build/poom.cue poom.xml

clean-iso:
	rm -f build/poom.bin build/poom.cue $(DATA)
