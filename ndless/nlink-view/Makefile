# Build with the Ndless SDK on PATH:
#   https://github.com/ndless-nspire/Ndless/wiki/Ndless-SDK:-C-and-assembly-development-introduction
# Produces nlink-view.tns. The SDK is not part of this repository.
# Run the program once on the calculator. It stays resident and serves
# the screen over USB. It does not write a file.

CC = nspire-gcc
LD = nspire-ld
GENZEHN = genzehn

CFLAGS = -Wall -Wextra -Os -marm

.PHONY: all clean
all: nlink-view.tns

main.o: main.c
	$(CC) $(CFLAGS) -c $< -o $@

nlink-view.elf: main.o
	$(LD) $^ -o $@

nlink-view.tns: nlink-view.elf
	$(GENZEHN) --input $< --output $@.zehn --name "nlink-view" --compress \
		--240x320-support true --uses-lcd-blit false
	make-prg $@.zehn $@
	rm -f $@.zehn

clean:
	rm -f main.o nlink-view.elf nlink-view.tns nlink-view.tns.zehn
