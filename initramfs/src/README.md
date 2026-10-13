# abinit - the initramfs /init for AutoBleem

Source of a new `/init` for the PlayStation Classic kernel. It is not in use yet: the kernel still embeds the
2020 binary `initramfs/init`. Switching to this one is a separate step after a test on a console.

## What it does

With an AutoBleem stick present the console runs with **nothing written to the eMMC**: the AutoBleem system
layer is a squashfs read from the stick and copied into RAM. Without a stick the console boots like a stock
PSC.

In order:

1. Mounts devtmpfs on `/dev`, proc and sysfs; opens `/dev/console` for stdio. Every step is logged to
   `/dev/kmsg` with the prefix `abinit: ` (there is no serial console: read it with `dmesg | grep abinit`).
2. Mounts the stock root `/dev/mmcblk0p7` read-only (ext4) at `/newroot` (waits up to 5 s for the node).
3. Rear port: only when `/sys/module/musb_hdrc/parameters/charger_info` reads `3` (an OTG adapter, the test
   `start_pman` uses) it writes `idle`, waits 200 ms, then `host` to `/sys/devices/platform/mt_usb/swmode`. It never
   writes `device`: the driver treats device and host as exclusive (`drivers/misc/mediatek/usb20/mt8167/usb20.c`,
   `mt_usb_store_swmode`). Without an adapter the port is left alone.
4. Polls `/dev` for USB mass storage (`sd[a-z]` and their partitions; a disk without partitions is tried whole)
   for up to `ab.wait` seconds. When the first device shows up it waits 1 s more for its partitions.
5. Mounts each candidate read-only as vfat, then ext4, at `/stick` and looks for the image and its checksum.
6. Copies the image into a tmpfs at `/abimage` (size = image + 1 MiB; refused when free RAM is less than the
   image + 64 MiB), checks the sha256 of the copy against the `.sha256` file, unmounts the stick.
7. Attaches a loop device (`/dev/loop-control`, `LOOP_CTL_GET_FREE`, `LOOP_SET_FD`, read-only, autoclear) and
   mounts the squashfs at `/squashfs`.
8. Mounts a tmpfs at `/abrw` for `upper` and `work` and the overlay at `/root`:
   `lowerdir=/squashfs:/newroot,upperdir=/abrw/upper,workdir=/abrw/work` (the squashfs wins over the stock root).
9. Mounts `/dev/mmcblk0p10` read-only with `noload` (no journal replay) at `/root/data`; when that fails the boot
   goes on without it.
10. Moves `/dev`, `/proc`, `/sys` into `/root`, moves `/root` onto `/`, chroots, deletes the initramfs files
    (as busybox `switch_root` does) and execs `/sbin/init`.

## Stick layout

FAT32 (vfat) or ext4: the kernel has no exFAT or NTFS driver. The image is looked for on every partition.

```
System/Kernel/abrootfs.squashfs          squashfs (xz or lz4; the kernel has no zstd)
System/Kernel/abrootfs.squashfs.sha256   sha256sum output: "<64 hex digits>  abrootfs.squashfs"
```

Make the checksum file with `sha256sum abrootfs.squashfs > abrootfs.squashfs.sha256` in that folder. Only the
first 64 hex digits are read; the file name after them is not checked. The image may be at most 512 MiB.

## Kernel command line

| Option | Meaning |
|---|---|
| `ab.wait=<seconds>` | how long to look for a stick, 0..300; default 20. `ab.wait=0` does not look for a stick at all. A bad value is logged and ignored. |

## Fallback order

Nothing in the init panics or returns; a failed step undoes the mounts of its path before the next path is
tried.

1. **Stick**: as above. Any failure (no stick in time, no image, no or bad checksum, image too big for RAM, a
   mount, loop or switch_root failure) falls through.
2. **Legacy eMMC overlay**, when `/data/autobleem/rootfs` exists on `mmcblk0p10` (checked with a read-only
   `noload` mount): the 2020 layout, so current installs keep working. `mmcblk0p10` is mounted read-write at
   `/data`, `/data/autobleem/workdir` is created when missing, and the overlay is
   `lowerdir=/newroot,upperdir=/data/autobleem/rootfs,workdir=/data/autobleem/workdir`. When
   `/data/autobleem/rootfs.squashfs` exists it is loop-mounted (straight from the eMMC, read-only) and becomes the
   top lower layer, `lowerdir=/squashfs:/newroot`, as the 2020 binary's strings suggest. `/data` is then moved to
   `/root/data` and a tmpfs mounted on `/root/tmp`.
3. **Stock**: switch_root to `/newroot` (`mmcblk0p7`, read-only) and exec its `/sbin/init`; `mmcblk0p10` is not
   touched and is left to the stock system (writable, as from the factory). Tried twice.
4. Last resort: `execv("/sbin/init")` twice, then sleep forever (PID 1 must not exit).

## Building

The console binary is static armhf, built with the console's gcc-6 toolchain:

```
make                                  # uses CROSS_COMPILE=/opt/psc/bin/armv8-sony-linux-gnueabihf-
make CROSS_COMPILE=<prefix>           # another toolchain
```

Flags: `-static -Os -march=armv8-a -mfpu=neon-vfpv4 -mfloat-abi=hard`. No dynamic loader, no busybox, no other
file in the initramfs is needed. To use it, the binary replaces `initramfs/init` (a later step, after a test on a
console).

## Host test

`make host ROOT=<dir>` builds `abinit-host` with `-DABINIT_TEST -DABINIT_ROOT="<dir>"`: every path is taken below
`<dir>`, mounts, the loop device and switch_root are only logged, and the chosen path is printed as
`ABINIT_RESULT=stick|legacy|stock|halt`. A block device is faked by a directory `<dir>/fake/<node>.<vfat|ext4>`
(what mounting it shows); `<dir>/fake/<type>.fail` makes every mount of that type fail.

`./test.sh` (or `make test`) builds it for each case and runs it against fake trees: a good stick (vfat, ext4,
whole disk, second partition, with an OTG adapter), a bad checksum, a missing checksum, no stick, a legacy
`/data/autobleem` (with and without `rootfs.squashfs`), `ab.wait=0`, bad `ab.wait` values, a failing overlay mount
and no stock root. The test images have sizes that hit the sha256 padding cases, and their checksums come from
coreutils `sha256sum`. `./test.sh -k` keeps the trees and logs.
