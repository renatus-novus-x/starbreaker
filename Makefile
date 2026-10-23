# STAR BREAKER for X68000 (elf2x68k, C++)
#
#   make  ->  breakout.sys (bootable system, no Human68k)
#             + breakout.x (Human68k .X executable)
#
# Toolchain: PATH must include the elf2x68k m68k-xelf/bin directory.
# A ready-to-boot disk image is shipped in dist/starbreaker.xdf.

CROSS = m68k-xelf-
CXX = $(CROSS)g++
CXXFLAGS = -m68000 -O2 -g -Wall

TARGETS = breakout.sys breakout.x

all: $(TARGETS)

# standalone system binary (hellosys-style link, no DOS)
breakout.sys: breakout_sys.o
	$(CXX) -Ttext=0x6800 -s -specs=x68knodos.specs -o $@ $^

breakout_sys.o: breakout.cpp
	$(CXX) $(CXXFLAGS) -DSTANDALONE -c $< -o $@

# Human68k executable
breakout.x: breakout_x.o
	$(CXX) -o $@ $^

breakout_x.o: breakout.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	-rm -f *.o *.elf* $(TARGETS)

.PHONY: all clean
