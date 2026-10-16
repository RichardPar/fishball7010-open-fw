# Optimised classifier. Cross build for the Pluto-class board (static: independent of the target's libc).
CROSS ?= arm-linux-gnueabihf-
CC     = $(CROSS)gcc
CFLAGS = -O2 -Wall -Wextra -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard

all: classifier

classifier: classifier.c
	$(CC) $(CFLAGS) -static -o $@ $< -lm
	$(CROSS)strip $@

host-test: classifier.c
	gcc -O2 -Wall -Wextra -o test/classifier_x86 classifier.c -lm

clean:
	rm -f classifier test/classifier_x86 test/bench_orig test/bench_new
