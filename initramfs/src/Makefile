# abinit - the initramfs /init (see README.md)
#
#   make                       console binary, static armhf (gcc-6 console toolchain)
#   make CROSS_COMPILE=<pfx>   another cross toolchain
#   make host ROOT=<dir>       host test build: paths below <dir>, mounts only logged
#   make test                  build for the host and run test.sh

CROSS_COMPILE ?= /opt/psc/bin/armv8-sony-linux-gnueabihf-
CC = $(CROSS_COMPILE)gcc
STRIP = $(CROSS_COMPILE)strip
HOSTCC ?= gcc

WARN = -Wall -Wextra -Wno-unused-parameter
CFLAGS = -static -Os $(WARN) -march=armv8-a -mfpu=neon-vfpv4 -mfloat-abi=hard
HOSTCFLAGS = -O2 -g $(WARN)

SRCS = abinit.c sha256.c
HDRS = sha256.h

ROOT ?= $(CURDIR)/test-root
HOSTBIN ?= abinit-host

all: abinit

abinit: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)
	$(STRIP) $@

host: $(SRCS) $(HDRS)
	$(HOSTCC) $(HOSTCFLAGS) -DABINIT_TEST -DABINIT_ROOT='"$(ROOT)"' -o $(HOSTBIN) $(SRCS)

test:
	./test.sh

clean:
	rm -f abinit abinit-host

.PHONY: all host test clean
