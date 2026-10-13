#!/bin/sh
# test.sh - run the host build of abinit against fake device trees and check the boot path it picks.
#
# Each case gets its own tree (see the comment above sim_mount in abinit.c):
#   <root>/proc/cmdline, <root>/sys/...           what the kernel would show
#   <root>/dev/<node>                             the device node exists
#   <root>/fake/<node>.<vfat|ext4>/               what mounting that node shows
#   <root>/fake/<type>.fail                       every mount of that type fails
# Usage: ./test.sh [-k]   (-k keeps the trees)

set -u
SRC="$(cd "$(dirname "$0")" && pwd)"
BASE="$(mktemp -d "${TMPDIR:-/tmp}/abinit-test.XXXXXX")" || exit 1
KEEP=0
[ "${1:-}" = "-k" ] && KEEP=1
PASS=0
FAIL=0

# images whose sizes hit the sha256 padding cases: one block boundary, 55 and 56 bytes into a block
head -c 1048589 /dev/urandom >"$BASE/img1"
head -c 196663 /dev/urandom >"$BASE/img2"
head -c 120 /dev/urandom >"$BASE/img3"

# tree <case> <cmdline> <charger_info>: the stock console, no stick
tree() {
	r="$BASE/$1"
	mkdir -p "$r/proc" "$r/sys/module/musb_hdrc/parameters" "$r/sys/devices/platform/mt_usb" \
		"$r/dev" "$r/fake/mmcblk0p7.ext4/sbin" "$r/fake/mmcblk0p7.ext4/data" "$r/fake/mmcblk0p10.ext4"
	echo "console=tty0 root=/dev/ram gpt $2" >"$r/proc/cmdline"
	echo "$3" >"$r/sys/module/musb_hdrc/parameters/charger_info"
	: >"$r/sys/devices/platform/mt_usb/swmode"
	: >"$r/dev/mmcblk0p7"
	: >"$r/dev/mmcblk0p10"
	: >"$r/dev/loop-control"
	printf '#!/bin/sh\n' >"$r/fake/mmcblk0p7.ext4/sbin/init"
	chmod 755 "$r/fake/mmcblk0p7.ext4/sbin/init"
}

# stick <case> <node> <fstype> <image>: a stick with a good image
stick() {
	r="$BASE/$1"
	d="$r/fake/$2.$3/System/Kernel"
	: >"$r/dev/$2"
	mkdir -p "$d"
	cp "$BASE/$4" "$d/abrootfs.squashfs"
	(cd "$d" && sha256sum abrootfs.squashfs >abrootfs.squashfs.sha256)
}

legacy() {
	mkdir -p "$BASE/$1/fake/mmcblk0p10.ext4/autobleem/rootfs"
}

run() {
	make -s -C "$SRC" host ROOT="$BASE/$1" HOSTBIN="$BASE/$1.bin" || return 1
	"$BASE/$1.bin" >"$BASE/$1.log" 2>&1
	RESULT="$(sed -n 's/^ABINIT_RESULT=//p' "$BASE/$1.log")"
}

# check <case> <expected path> [grep -F patterns that must be in the log...]; !pattern must not be
check() {
	c="$1"
	want="$2"
	shift 2
	ok=1
	why=""
	if [ "$RESULT" != "$want" ]; then
		ok=0
		why="path '$RESULT', expected '$want'"
	fi
	for p in "$@"; do
		case "$p" in
		!*)
			if grep -qF -- "${p#!}" "$BASE/$c.log"; then
				ok=0
				why="$why; log has '${p#!}'"
			fi
			;;
		*)
			if ! grep -qF -- "$p" "$BASE/$c.log"; then
				ok=0
				why="$why; log lacks '$p'"
			fi
			;;
		esac
	done
	if [ $ok = 1 ]; then
		PASS=$((PASS + 1))
		echo "PASS $c -> $RESULT"
	else
		FAIL=$((FAIL + 1))
		echo "FAIL $c: $why"
		sed 's/^/    /' "$BASE/$c.log"
	fi
}

c=good_stick
tree $c "ab.wait=3" 0
stick $c sda1 vfat img1
run $c
check $c stick "sha256 ok" "mounted /dev/sda1 on /stick (vfat, ro)" "unmounted /stick" \
	"lowerdir=/squashfs:/newroot,upperdir=/abrw/upper,workdir=/abrw/work" \
	"mounted /dev/mmcblk0p10 on /root/data (ext4, ro, noload)" "rear port left alone" "!swmode"
[ -s "$BASE/$c/sys/devices/platform/mt_usb/swmode" ] && { FAIL=$((FAIL + 1)); echo "FAIL $c: swmode written"; }

c=bad_sha
tree $c "ab.wait=3" 0
stick $c sda1 vfat img1
echo "0000000000000000000000000000000000000000000000000000000000000000  abrootfs.squashfs" \
	>"$BASE/$c/fake/sda1.vfat/System/Kernel/abrootfs.squashfs.sha256"
run $c
check $c stock "sha256 mismatch" "unmounted /abimage" "unmounted /stick" "legacy /data/autobleem/rootfs not found" \
	"!mounted /dev/mmcblk0p10 on /data (ext4)"

c=bad_sha_legacy
tree $c "ab.wait=3" 0
stick $c sda1 vfat img2
legacy $c
head -c 10 /dev/zero >>"$BASE/$c/fake/sda1.vfat/System/Kernel/abrootfs.squashfs"
run $c
check $c legacy "sha256 mismatch" "!sha256 ok"

c=no_sha_file
tree $c "ab.wait=2" 0
stick $c sda1 vfat img3
rm "$BASE/$c/fake/sda1.vfat/System/Kernel/abrootfs.squashfs.sha256"
run $c
check $c stock "cannot read /stick/System/Kernel/abrootfs.squashfs.sha256" "unmounted /stick"

c=no_stick
tree $c "ab.wait=1" 0
run $c
check $c stock "looking for a stick for up to 1 s" "no stick with a good image after 1 s" "!trying"

c=legacy
tree $c "ab.wait=1" 0
legacy $c
run $c
check $c legacy "legacy /data/autobleem/rootfs found" "mounted /dev/mmcblk0p10 on /data (ext4)" \
	"lowerdir=/newroot,upperdir=/data/autobleem/rootfs,workdir=/data/autobleem/workdir" \
	"mounted /data on /root/data (move)" "mounted tmpfs on /root/tmp"
[ -d "$BASE/$c/fake/mmcblk0p10.ext4/autobleem/workdir" ] || { FAIL=$((FAIL + 1)); echo "FAIL $c: no workdir"; }

c=legacy_squashfs
tree $c "ab.wait=0" 0
legacy $c
cp "$BASE/img3" "$BASE/$c/fake/mmcblk0p10.ext4/autobleem/rootfs.squashfs"
run $c
check $c legacy "lowerdir=/squashfs:/newroot,upperdir=/data/autobleem/rootfs" "mounted /dev/loop0 on /squashfs"

c=wait0
tree $c "ab.wait=0" 0
stick $c sda1 vfat img1
run $c
check $c stock "ab.wait=0: not looking for a stick" "!trying"

c=ext4_stick
tree $c "ab.wait=3" 0
stick $c sda1 ext4 img2
run $c
check $c stick "mount /dev/sda1 on /stick (vfat)" "mounted /dev/sda1 on /stick (ext4, ro)" "sha256 ok"

c=whole_disk
tree $c "ab.wait=3" 0
stick $c sdb vfat img3
run $c
check $c stick "trying /dev/sdb" "sha256 ok"

c=second_partition
tree $c "ab.wait=3" 0
: >"$BASE/$c/dev/sda"
: >"$BASE/$c/dev/sda1"
stick $c sda2 vfat img3
run $c
check $c stick "trying /dev/sda1" "/dev/sda1: neither vfat nor ext4" "trying /dev/sda2"
grep -qx "abinit: trying /dev/sda" "$BASE/$c.log" && { FAIL=$((FAIL + 1)); echo "FAIL $c: tried the whole disk"; }

c=otg
tree $c "ab.wait=3" 3
stick $c sda1 vfat img1
run $c
check $c stick "charger_info 3: rear port to host mode"
if [ "$(cat "$BASE/$c/sys/devices/platform/mt_usb/swmode")" != "$(printf 'idle\nhost')" ]; then
	FAIL=$((FAIL + 1))
	echo "FAIL $c: swmode got '$(cat "$BASE/$c/sys/devices/platform/mt_usb/swmode")'"
fi

c=overlay_fail
tree $c "ab.wait=3" 0
stick $c sda1 vfat img1
: >"$BASE/$c/fake/overlay.fail"
run $c
check $c stock "stick path failed, cleaning up" "unmounted /abrw" "unmounted /squashfs" "unmounted /abimage"

c=overlay_fail_legacy
tree $c "ab.wait=3" 0
stick $c sda1 vfat img1
legacy $c
: >"$BASE/$c/fake/overlay.fail"
run $c
check $c stock "stick path failed" "legacy path failed" "unmounted /data" "booting stock"

c=bad_wait
tree $c "ab.wait=x ab.wait=-1 ab.wait=1" 0
run $c
check $c stock "ignoring bad ab.wait=x" "ignoring bad ab.wait=-1" "looking for a stick for up to 1 s"

c=no_root
tree $c "ab.wait=1" 0
rm "$BASE/$c/dev/mmcblk0p7"
run $c
check $c halt "no stock root, nothing to boot"

echo "passed $PASS, failed $FAIL"
if [ $KEEP = 1 ] || [ $FAIL != 0 ]; then
	echo "trees kept in $BASE"
else
	case "$BASE" in
	*/abinit-test.*) rm -rf -- "$BASE" ;;
	esac
fi
[ $FAIL = 0 ]
