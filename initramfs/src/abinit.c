/*
 * abinit.c - initramfs /init for the PlayStation Classic with AutoBleem.
 *
 * With an AutoBleem stick present the console runs with nothing written to
 * the eMMC: the system layer is a squashfs read from the stick, copied into
 * RAM and put over the stock root (mmcblk0p7) with a tmpfs upper layer.
 * Without a stick it boots the legacy eMMC overlay (/data/autobleem) when
 * one is installed, else the stock system. It never returns: PID 1 exiting
 * panics the kernel.
 *
 * Build with -DABINIT_TEST (and -DABINIT_ROOT="<dir>") for the host test:
 * every path is then taken below <dir>, mounts, loop devices and the
 * switch_root are only logged (block devices are faked by directories,
 * see test.sh) and the chosen boot path is printed.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysinfo.h>
#include <linux/loop.h>
#include <linux/magic.h>

#include "sha256.h"

#ifndef ABINIT_ROOT
#define ABINIT_ROOT ""
#endif
#ifdef ABINIT_TEST
#define TEST 1
#else
#define TEST 0
#endif

#define DEV_ROOT		"/dev/mmcblk0p7"
#define DEV_DATA		"/dev/mmcblk0p10"
#define CHARGER_INFO		"/sys/module/musb_hdrc/parameters/charger_info"
#define REAR_SWMODE		"/sys/devices/platform/mt_usb/swmode"

#define MNT_NEWROOT		"/newroot"	/* stock root, read-only */
#define MNT_STICK		"/stick"	/* the stick, read-only, only while copying */
#define MNT_IMAGE		"/abimage"	/* tmpfs with the copy of the squashfs */
#define MNT_SQUASHFS		"/squashfs"	/* the squashfs, lower layer */
#define MNT_RW			"/abrw"		/* tmpfs with upper and work */
#define MNT_ROOT		"/root"		/* the overlay, the new root */
#define MNT_DATA		"/data"		/* legacy path: mmcblk0p10 */

#define STICK_IMAGE		MNT_STICK "/System/Kernel/abrootfs.squashfs"
#define STICK_SHA256		STICK_IMAGE ".sha256"
#define RAM_IMAGE		MNT_IMAGE "/abrootfs.squashfs"
#define LEGACY_DIR		MNT_DATA "/autobleem"

#define WAIT_DEFAULT_S		20	/* ab.wait= default */
#define WAIT_MAX_S		300
#define ROOT_WAIT_MS		5000	/* mmcblk0p7 node */
#define DATA_WAIT_MS		3000	/* mmcblk0p10 node */
#define STICK_SETTLE_MS		1000	/* let the partitions of a new disk appear */
#define POLL_MS			250
#define IMAGE_MARGIN		(1024 * 1024)
#define IMAGE_MAX		(512LL * 1024 * 1024)
#define RAM_RESERVE		(64LL * 1024 * 1024)	/* left free after the copy */
#define MAX_MOUNTS		16
#define MAX_CANDIDATES		32
#define DELETE_MAX_DEPTH	16

static int kmsg_fd = -1;
static char **init_argv;

static void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void logmsg(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n, m;

	n = snprintf(buf, sizeof(buf), "<5>abinit: ");
	va_start(ap, fmt);
	m = vsnprintf(buf + n, sizeof(buf) - n - 1, fmt, ap);
	va_end(ap);
	if (m < 0)
		return;
	n += m;
	if (n > (int)sizeof(buf) - 2)
		n = sizeof(buf) - 2;
	buf[n++] = '\n';
	if (TEST) {
		if (fwrite(buf + 3, 1, n - 3, stdout) != (size_t)(n - 3))
			return;
		fflush(stdout);
	} else if (write(kmsg_fd >= 0 ? kmsg_fd : 2, buf, n) < 0) {
		return;
	}
}

static long long now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
		return 0;
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	int i;

	/* a signal can cut it short; a few retries, never an endless loop */
	for (i = 0; i < 8 && nanosleep(&ts, &ts) < 0 && errno == EINTR; i++)
		;
}

/* ---- paths: the test build maps them below ABINIT_ROOT and fake mounts ---- */

#if TEST
struct sim_mount {
	char target[PATH_MAX];
	char dir[PATH_MAX];
};
static struct sim_mount sim[MAX_MOUNTS];
static int nsim;
static int sim_tmpfs;
#endif

static const char *P(const char *path)
{
	static char bufs[4][PATH_MAX];
	static unsigned int next;
	char *b = bufs[next++ % 4];
	const char *base = ABINIT_ROOT;
	const char *rest = path;
	int n;

#if TEST
	size_t best = 0;
	int i;

	for (i = 0; i < nsim; i++) {
		size_t len = strlen(sim[i].target);

		if (len > best && !strncmp(path, sim[i].target, len) &&
		    (path[len] == '\0' || path[len] == '/')) {
			best = len;
			base = sim[i].dir;
			rest = path + len;
		}
	}
#endif
	n = snprintf(b, PATH_MAX, "%s%s", base, rest);
	if (n < 0 || n >= PATH_MAX)
		return "/nonexistent/path-too-long";
	return b;
}

static int is_dir(const char *path)
{
	struct stat st;

	return stat(P(path), &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_file(const char *path)
{
	struct stat st;

	return stat(P(path), &st) == 0 && S_ISREG(st.st_mode);
}

static int make_dir(const char *path, mode_t mode)
{
	if (mkdir(P(path), mode) < 0 && errno != EEXIST) {
		logmsg("mkdir %s: %s", path, strerror(errno));
		return -1;
	}
	return 0;
}

static int make_dirs(const char *path)
{
	char tmp[PATH_MAX];
	char *p;

	if (strlen(path) >= sizeof(tmp))
		return -1;
	strcpy(tmp, path);
	for (p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (make_dir(tmp, 0755) < 0)
			return -1;
		*p = '/';
	}
	return make_dir(tmp, 0755);
}

static ssize_t read_small(const char *path, char *buf, size_t size)
{
	ssize_t n;
	int fd;

	fd = open(P(path), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = read(fd, buf, size - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	return n;
}

static int write_str(const char *path, const char *s)
{
	size_t len = strlen(s);
	ssize_t n;
	int fd;

	/* the test build appends, so test.sh sees every write in order */
	fd = open(P(path), TEST ? O_WRONLY | O_APPEND | O_CLOEXEC : O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		logmsg("open %s: %s", path, strerror(errno));
		return -1;
	}
	n = write(fd, s, len);
	if (n != (ssize_t)len) {
		logmsg("write '%s' to %s: %s", s, path, n < 0 ? strerror(errno) : "short write");
		close(fd);
		return -1;
	}
	if (close(fd) < 0) {
		logmsg("close %s: %s", path, strerror(errno));
		return -1;
	}
	return 0;
}

/* wait until a device node exists, at most ms */
static int wait_for_node(const char *path, long ms)
{
	long long deadline = now_ms() + ms;

	for (;;) {
		if (access(P(path), F_OK) == 0)
			return 0;
		if (now_ms() >= deadline)
			break;
		sleep_ms(50);
	}
	logmsg("%s did not appear in %ld ms", path, ms);
	return -1;
}

/* ---- mounts: every one the init makes is on a stack, so a failed path can be undone ---- */

static char mstack[MAX_MOUNTS][64];
static int mdepth;

#if TEST
static int sim_add(const char *target, const char *dir)
{
	if (nsim >= MAX_MOUNTS || strlen(target) >= PATH_MAX || strlen(dir) >= PATH_MAX) {
		errno = ENOMEM;
		return -1;
	}
	strcpy(sim[nsim].target, target);
	strcpy(sim[nsim].dir, dir);
	nsim++;
	return 0;
}

static void sim_remove(const char *target)
{
	int i;

	for (i = nsim - 1; i >= 0; i--) {
		if (strcmp(sim[i].target, target))
			continue;
		memmove(&sim[i], &sim[i + 1], (nsim - i - 1) * sizeof(sim[0]));
		nsim--;
		return;
	}
}

/*
 * Fake mounts. A block device <dev> "holds" filesystem <type> when the
 * directory ABINIT_ROOT/fake/<dev>.<type> exists; its content is what the
 * mount shows. A tmpfs is a new directory below ABINIT_ROOT/tmpfs. The
 * overlay shows the stock root (/newroot). ABINIT_ROOT/fake/<type>.fail
 * makes every mount of that type fail.
 */
static int sim_mount(const char *src, const char *target, const char *type, unsigned long flags)
{
	char path[PATH_MAX];

	if (flags & (MS_MOVE | MS_REMOUNT))
		return 0;
	snprintf(path, sizeof(path), "%s/fake/%s.fail", ABINIT_ROOT, type);
	if (access(path, F_OK) == 0) {
		errno = EINVAL;
		return -1;
	}
	if (!strcmp(type, "vfat") || !strcmp(type, "ext4")) {
		const char *dev = strrchr(src, '/');
		struct stat st;

		snprintf(path, sizeof(path), "%s/fake/%s.%s", ABINIT_ROOT, dev ? dev + 1 : src, type);
		if (stat(path, &st) < 0 || !S_ISDIR(st.st_mode)) {
			errno = EINVAL;
			return -1;
		}
		return sim_add(target, path);
	}
	if (!strcmp(type, "tmpfs") && strncmp(target, "/dev/", 5)) {
		snprintf(path, sizeof(path), "%s/tmpfs", ABINIT_ROOT);
		if (mkdir(path, 0755) < 0 && errno != EEXIST)
			return -1;
		snprintf(path, sizeof(path), "%s/tmpfs/%d", ABINIT_ROOT, ++sim_tmpfs);
		if (mkdir(path, 0755) < 0)
			return -1;
		return sim_add(target, path);
	}
	if (!strcmp(type, "overlay"))
		return sim_add(target, P(MNT_NEWROOT));
	return 0;
}
#endif

static int do_mount(const char *src, const char *target, const char *type,
		    unsigned long flags, const char *data)
{
	int ret;

	if (!TEST && make_dirs(target) < 0)
		return -1;
#if TEST
	ret = sim_mount(src, target, type ? type : "", flags);
#else
	ret = mount(src, target, type, flags, data);
#endif
	if (ret < 0) {
		int err = errno;

		logmsg("mount %s on %s (%s%s%s): %s", src, target, type ? type : "move",
		       data ? ", " : "", data ? data : "", strerror(err));
		errno = err;
		return -1;
	}
	logmsg("mounted %s on %s (%s%s%s%s)", src, target, type ? type : "move",
	       flags & MS_RDONLY ? ", ro" : "", data ? ", " : "", data ? data : "");
	return 0;
}

static void do_umount(const char *target)
{
#if TEST
	sim_remove(target);
	logmsg("unmounted %s", target);
#else
	if (umount(target) == 0) {
		logmsg("unmounted %s", target);
		return;
	}
	logmsg("umount %s: %s, detaching", target, strerror(errno));
	if (umount2(target, MNT_DETACH) < 0)
		logmsg("umount -l %s: %s", target, strerror(errno));
#endif
}

static int push_mount(const char *src, const char *target, const char *type,
		      unsigned long flags, const char *data)
{
	if (mdepth >= MAX_MOUNTS || strlen(target) >= sizeof(mstack[0])) {
		logmsg("mount table full, not mounting %s", target);
		return -1;
	}
	if (do_mount(src, target, type, flags, data) < 0)
		return -1;
	strcpy(mstack[mdepth++], target);
	return 0;
}

/* unmount one entry of the stack, wherever it is */
static void pop_mount(const char *target)
{
	int i;

	for (i = mdepth - 1; i >= 0; i--) {
		if (strcmp(mstack[i], target))
			continue;
		do_umount(target);
		memmove(mstack[i], mstack[i + 1], (mdepth - i - 1) * sizeof(mstack[0]));
		mdepth--;
		return;
	}
}

/* undo every mount above depth, newest first */
static void unwind(int depth)
{
	while (mdepth > depth)
		do_umount(mstack[--mdepth]);
}

/* move a mount; on the stack it becomes the newest entry */
static int move_stacked(const char *from, const char *to)
{
	int i;

	if (do_mount(from, to, NULL, MS_MOVE, NULL) < 0)
		return -1;
	for (i = mdepth - 1; i >= 0; i--) {
		if (strcmp(mstack[i], from))
			continue;
		memmove(mstack[i], mstack[i + 1], (mdepth - i - 1) * sizeof(mstack[0]));
		mdepth--;
		break;
	}
	if (mdepth < MAX_MOUNTS && strlen(to) < sizeof(mstack[0]))
		strcpy(mstack[mdepth++], to);
	return 0;
}

/* ---- loop device + squashfs ---- */

#if !TEST
static int loop_attach(const char *file, char *dev, size_t devsz)
{
	int ffd, cfd, lfd = -1;
	int tries;

	ffd = open(file, O_RDONLY | O_CLOEXEC);
	if (ffd < 0) {
		logmsg("open %s: %s", file, strerror(errno));
		return -1;
	}
	cfd = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
	if (cfd < 0) {
		logmsg("open /dev/loop-control: %s", strerror(errno));
		close(ffd);
		return -1;
	}
	/* another opener can take the free device between the two ioctls: retry a few times */
	for (tries = 0; tries < 4; tries++) {
		struct loop_info64 info;
		int n;

		n = ioctl(cfd, LOOP_CTL_GET_FREE);
		if (n < 0) {
			logmsg("LOOP_CTL_GET_FREE: %s", strerror(errno));
			break;
		}
		snprintf(dev, devsz, "/dev/loop%d", n);
		if (wait_for_node(dev, 1000) < 0)
			break;
		lfd = open(dev, O_RDONLY | O_CLOEXEC);
		if (lfd < 0) {
			logmsg("open %s: %s", dev, strerror(errno));
			break;
		}
		if (ioctl(lfd, LOOP_SET_FD, ffd) < 0) {
			int err = errno;

			close(lfd);
			lfd = -1;
			if (err == EBUSY)
				continue;
			logmsg("LOOP_SET_FD %s: %s", dev, strerror(err));
			break;
		}
		/* the device frees itself when the squashfs is unmounted */
		memset(&info, 0, sizeof(info));
		info.lo_flags = LO_FLAGS_READ_ONLY | LO_FLAGS_AUTOCLEAR;
		strncpy((char *)info.lo_file_name, file, LO_NAME_SIZE - 1);
		if (ioctl(lfd, LOOP_SET_STATUS64, &info) < 0) {
			logmsg("LOOP_SET_STATUS64 %s: %s", dev, strerror(errno));
			if (ioctl(lfd, LOOP_CLR_FD, 0) < 0)
				logmsg("LOOP_CLR_FD %s: %s", dev, strerror(errno));
			close(lfd);
			lfd = -1;
			break;
		}
		logmsg("%s attached to %s (read-only)", file, dev);
		break;
	}
	close(cfd);
	close(ffd);
	return lfd;
}
#endif

static int mount_squashfs(const char *file, const char *target)
{
	char dev[32];

#if TEST
	if (!is_file(file)) {
		logmsg("no squashfs file %s", file);
		return -1;
	}
	snprintf(dev, sizeof(dev), "/dev/loop0");
	logmsg("%s attached to %s (read-only)", file, dev);
#else
	int lfd = loop_attach(file, dev, sizeof(dev));

	if (lfd < 0)
		return -1;
#endif
	if (push_mount(dev, target, "squashfs", MS_RDONLY | MS_NODEV | MS_NOSUID, NULL) < 0) {
#if !TEST
		if (ioctl(lfd, LOOP_CLR_FD, 0) < 0)
			logmsg("LOOP_CLR_FD %s: %s", dev, strerror(errno));
		close(lfd);
#endif
		return -1;
	}
#if !TEST
	close(lfd);
#endif
	return 0;
}

/* ---- switch_root ---- */

#if !TEST
/* free the initramfs: delete what is on the old root's device, not what is mounted on it */
static void delete_contents(int dirfd, dev_t rootdev, int depth)
{
	struct dirent *d;
	DIR *dir;
	int fd;

	if (depth > DELETE_MAX_DEPTH)
		return;
	fd = dup(dirfd);
	if (fd < 0)
		return;
	dir = fdopendir(fd);
	if (!dir) {
		close(fd);
		return;
	}
	while ((d = readdir(dir))) {
		struct stat st;

		if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
			continue;
		if (fstatat(dirfd, d->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0)
			continue;
		if (st.st_dev != rootdev)
			continue;
		if (S_ISDIR(st.st_mode)) {
			int sub = openat(dirfd, d->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

			if (sub >= 0) {
				delete_contents(sub, rootdev, depth + 1);
				close(sub);
			}
			unlinkat(dirfd, d->d_name, AT_REMOVEDIR);
		} else {
			unlinkat(dirfd, d->d_name, 0);
		}
	}
	closedir(dir);
}
#endif

static const char *const moved[] = { "/dev", "/proc", "/sys" };

/*
 * Move /dev, /proc, /sys into newroot, make it / and exec its /sbin/init.
 * Returns -1 when nothing is changed (the caller can try another path),
 * -2 when the root was already switched and init could not be started.
 */
static int switch_root(const char *newroot, const char *label)
{
	char path[PATH_MAX];
	int i;

	snprintf(path, sizeof(path), "%s/sbin/init", newroot);
	if (access(P(path), X_OK) < 0) {
		logmsg("%s: %s", path, strerror(errno));
		return -1;
	}
#if TEST
	for (i = 0; i < 3; i++)
		logmsg("would move %s to %s%s", moved[i], newroot, moved[i]);
	logmsg("would switch_root %s and exec /sbin/init", newroot);
	printf("ABINIT_RESULT=%s\n", label);
	exit(0);
#else
	{
		struct stat st;
		struct statfs sfs;
		int oldfd, can_delete;

		oldfd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		can_delete = oldfd >= 0 && fstat(oldfd, &st) == 0 && statfs("/", &sfs) == 0 &&
			     ((unsigned long)sfs.f_type == RAMFS_MAGIC ||
			      (unsigned long)sfs.f_type == TMPFS_MAGIC);

		for (i = 0; i < 3; i++) {
			snprintf(path, sizeof(path), "%s%s", newroot, moved[i]);
			if (mount(moved[i], path, NULL, MS_MOVE, NULL) < 0) {
				logmsg("move %s to %s: %s", moved[i], path, strerror(errno));
				break;
			}
		}
		if (i == 3 && chdir(newroot) < 0)
			logmsg("chdir %s: %s", newroot, strerror(errno));
		else if (i == 3 && mount(".", "/", NULL, MS_MOVE, NULL) < 0)
			logmsg("move %s to /: %s", newroot, strerror(errno));
		else if (i == 3)
			goto moved;

		/* undo: put back what was moved, the caller tries the next path */
		while (i-- > 0) {
			snprintf(path, sizeof(path), "%s%s", newroot, moved[i]);
			if (mount(path, moved[i], NULL, MS_MOVE, NULL) < 0)
				logmsg("move %s back: %s", path, strerror(errno));
		}
		if (chdir("/") < 0)
			logmsg("chdir /: %s", strerror(errno));
		if (oldfd >= 0)
			close(oldfd);
		return -1;

moved:
		if (chroot(".") < 0)
			logmsg("chroot %s: %s", newroot, strerror(errno));
		if (chdir("/") < 0)
			logmsg("chdir /: %s", strerror(errno));
		logmsg("switched root to %s (%s), exec /sbin/init", newroot, label);
		if (can_delete)
			delete_contents(oldfd, st.st_dev, 0);
		if (oldfd >= 0)
			close(oldfd);
		init_argv[0] = (char *)"/sbin/init";
		execv("/sbin/init", init_argv);
		logmsg("exec /sbin/init: %s", strerror(errno));
		return -2;
	}
#endif
}

/* ---- the steps ---- */

static void early_mounts(void)
{
	make_dir("/dev", 0755);
	make_dir("/proc", 0555);
	make_dir("/sys", 0555);
	if (do_mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755") < 0 && errno != EBUSY)
		logmsg("no devtmpfs: device nodes will be missing");
#if !TEST
	{
		int fd;

		kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
		fd = open("/dev/console", O_RDWR | O_NOCTTY);
		if (fd >= 0) {
			dup2(fd, 0);
			dup2(fd, 1);
			dup2(fd, 2);
			if (fd > 2)
				close(fd);
		}
	}
#endif
	do_mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	do_mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
}

/* ab.wait=<seconds> on the kernel cmdline */
static int stick_wait_seconds(void)
{
	char buf[4096];
	char *tok, *save = NULL;
	int wait = WAIT_DEFAULT_S;

	if (read_small("/proc/cmdline", buf, sizeof(buf)) < 0) {
		logmsg("cannot read /proc/cmdline: %s", strerror(errno));
		return wait;
	}
	for (tok = strtok_r(buf, " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save)) {
		char *end;
		long v;

		if (strncmp(tok, "ab.wait=", 8))
			continue;
		errno = 0;
		v = strtol(tok + 8, &end, 10);
		if (errno || end == tok + 8 || *end || v < 0 || v > WAIT_MAX_S) {
			logmsg("ignoring bad %s (0..%d)", tok, WAIT_MAX_S);
			continue;
		}
		wait = (int)v;
	}
	return wait;
}

/* the rear port is in host mode only when an OTG adapter is plugged (as start_pman does) */
static void rear_port(void)
{
	char buf[32];
	unsigned long v;
	char *end;

	if (read_small(CHARGER_INFO, buf, sizeof(buf)) < 0) {
		logmsg("cannot read %s: %s, rear port left alone", CHARGER_INFO, strerror(errno));
		return;
	}
	v = strtoul(buf, &end, 10);
	if (end == buf || v != 3) {
		logmsg("charger_info %lu: no OTG adapter, rear port left alone", end == buf ? 0UL : v);
		return;
	}
	/* host is accepted only from idle; never write device (usb20.c: device and host exclude each other) */
	logmsg("charger_info 3: rear port to host mode");
	if (write_str(REAR_SWMODE, "idle\n") < 0)
		return;
	sleep_ms(200);
	write_str(REAR_SWMODE, "host\n");
}

/* sd[a-z] or sd[a-z]<n> */
static int sd_name(const char *n)
{
	const char *p;

	if (strncmp(n, "sd", 2) || n[2] < 'a' || n[2] > 'z')
		return 0;
	for (p = n + 3; *p; p++)
		if (*p < '0' || *p > '9')
			return 0;
	return strlen(n) < 16;
}

static int cmp_candidates(const void *a, const void *b)
{
	const char *x = a, *y = b;

	if (x[2] != y[2])
		return x[2] - y[2];
	return atoi(x + 3) - atoi(y + 3);
}

/* the stick's block devices: partitions, or the whole disk when it has none */
static int list_candidates(char names[][16], int max)
{
	char all[MAX_CANDIDATES][16];
	struct dirent *d;
	DIR *dir;
	int n = 0, out = 0, i, j;

	dir = opendir(P("/dev"));
	if (!dir) {
		logmsg("opendir /dev: %s", strerror(errno));
		return 0;
	}
	while ((d = readdir(dir)) && n < MAX_CANDIDATES) {
		if (sd_name(d->d_name))
			strcpy(all[n++], d->d_name);
	}
	closedir(dir);

	for (i = 0; i < n && out < max; i++) {
		int has_parts = 0;

		if (all[i][3] == '\0') {
			for (j = 0; j < n; j++)
				if (all[j][2] == all[i][2] && all[j][3] != '\0')
					has_parts = 1;
			if (has_parts)
				continue;
		}
		strcpy(names[out++], all[i]);
	}
	qsort(names, out, sizeof(names[0]), cmp_candidates);
	return out;
}

static int parse_sha256(const char *path, uint8_t out[SHA256_DIGEST_SIZE])
{
	char buf[256];
	const char *p;
	int i;

	if (read_small(path, buf, sizeof(buf)) < 0) {
		logmsg("cannot read %s: %s", path, strerror(errno));
		return -1;
	}
	for (p = buf; *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'; p++)
		;
	for (i = 0; i < 2 * SHA256_DIGEST_SIZE; i++) {
		char c = p[i];
		int v;

		if (c >= '0' && c <= '9')
			v = c - '0';
		else if (c >= 'a' && c <= 'f')
			v = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			v = c - 'A' + 10;
		else
			goto bad;
		if (i % 2)
			out[i / 2] |= v;
		else
			out[i / 2] = v << 4;
	}
	if (p[i] == '\0' || p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')
		return 0;
bad:
	logmsg("%s: not in sha256sum format", path);
	return -1;
}

static void hex(const uint8_t *d, char *out)
{
	static const char digits[] = "0123456789abcdef";
	int i;

	for (i = 0; i < SHA256_DIGEST_SIZE; i++) {
		out[2 * i] = digits[d[i] >> 4];
		out[2 * i + 1] = digits[d[i] & 15];
	}
	out[2 * SHA256_DIGEST_SIZE] = '\0';
}

static unsigned char iobuf[64 * 1024];

static int copy_file(const char *from, const char *to, long long size)
{
	long long total = 0;
	int in, out, ret = -1;

	in = open(P(from), O_RDONLY | O_CLOEXEC);
	if (in < 0) {
		logmsg("open %s: %s", from, strerror(errno));
		return -1;
	}
	out = open(P(to), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
	if (out < 0) {
		logmsg("create %s: %s", to, strerror(errno));
		close(in);
		return -1;
	}
	/* each read moves forward or ends the loop: bounded by the file size */
	for (;;) {
		ssize_t n = read(in, iobuf, sizeof(iobuf)), done = 0;

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			logmsg("read %s: %s", from, strerror(errno));
			goto out;
		}
		if (n == 0)
			break;
		total += n;
		if (total > size) {
			logmsg("%s grew while copying", from);
			goto out;
		}
		while (done < n) {
			ssize_t w = write(out, iobuf + done, n - done);

			if (w < 0 && errno == EINTR)
				continue;
			if (w <= 0) {
				logmsg("write %s: %s", to, w < 0 ? strerror(errno) : "no progress");
				goto out;
			}
			done += w;
		}
	}
	if (total != size) {
		logmsg("copied %lld of %lld bytes from %s", total, size, from);
		goto out;
	}
	ret = 0;
out:
	if (close(out) < 0 && ret == 0) {
		logmsg("close %s: %s", to, strerror(errno));
		ret = -1;
	}
	close(in);
	return ret;
}

static int hash_file(const char *path, uint8_t digest[SHA256_DIGEST_SIZE])
{
	struct sha256_ctx ctx;
	int fd;

	fd = open(P(path), O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		logmsg("open %s: %s", path, strerror(errno));
		return -1;
	}
	sha256_init(&ctx);
	for (;;) {
		ssize_t n = read(fd, iobuf, sizeof(iobuf));

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			logmsg("read %s: %s", path, strerror(errno));
			close(fd);
			return -1;
		}
		if (n == 0)
			break;
		sha256_update(&ctx, iobuf, n);
	}
	close(fd);
	sha256_final(&ctx, digest);
	return 0;
}

/* mount one candidate, copy and check its image into RAM; on success only the RAM copy stays mounted */
static int try_stick(const char *name)
{
	static const char *const types[] = { "vfat", "ext4" };
	uint8_t want[SHA256_DIGEST_SIZE], got[SHA256_DIGEST_SIZE];
	char dev[32], opts[64], w[65], g[65];
	struct sysinfo si;
	struct stat st;
	long long size;
	int base = mdepth;
	int i;

	snprintf(dev, sizeof(dev), "/dev/%.15s", name);
	for (i = 0; i < 2; i++)
		if (push_mount(dev, MNT_STICK, types[i],
			       MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) == 0)
			break;
	if (i == 2) {
		logmsg("%s: neither vfat nor ext4", dev);
		return -1;
	}
	if (stat(P(STICK_IMAGE), &st) < 0 || !S_ISREG(st.st_mode)) {
		logmsg("%s: no System/Kernel/abrootfs.squashfs", dev);
		goto fail;
	}
	if (parse_sha256(STICK_SHA256, want) < 0)
		goto fail;
	size = st.st_size;
	if (size <= 0 || size > IMAGE_MAX) {
		logmsg("%s: image size %lld out of range", dev, size);
		goto fail;
	}
	if (sysinfo(&si) < 0) {
		logmsg("sysinfo: %s", strerror(errno));
		goto fail;
	}
	if ((long long)si.freeram * si.mem_unit < size + RAM_RESERVE) {
		logmsg("image %lld bytes does not fit in free RAM %lld", size,
		       (long long)si.freeram * si.mem_unit);
		goto fail;
	}
	snprintf(opts, sizeof(opts), "size=%lld,mode=0700",
		 (size + IMAGE_MARGIN + 4095) / 4096 * 4096);
	if (push_mount("abimage", MNT_IMAGE, "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, opts) < 0)
		goto fail;
	logmsg("copying %s (%lld bytes) to RAM", STICK_IMAGE, size);
	if (copy_file(STICK_IMAGE, RAM_IMAGE, size) < 0)
		goto fail;
	if (hash_file(RAM_IMAGE, got) < 0)
		goto fail;
	hex(want, w);
	hex(got, g);
	if (memcmp(want, got, sizeof(got))) {
		logmsg("sha256 mismatch: file says %s, copy is %s", w, g);
		goto fail;
	}
	logmsg("sha256 ok: %s", g);
	pop_mount(MNT_STICK);
	return 0;
fail:
	unwind(base);
	return -1;
}

/* poll for USB mass storage until one holds a good image or the time is up */
static int find_stick(int wait_s)
{
	char tried[MAX_CANDIDATES][16];
	char names[MAX_CANDIDATES][16];
	long long deadline = now_ms() + wait_s * 1000LL;
	int ntried = 0, settled = 0;

	logmsg("looking for a stick for up to %d s", wait_s);
	for (;;) {
		int n = list_candidates(names, MAX_CANDIDATES), i, j;

		if (n > 0 && !settled) {
			long left = (long)(deadline - now_ms());

			settled = 1;
			sleep_ms(left < STICK_SETTLE_MS ? (left > 0 ? left : 0) : STICK_SETTLE_MS);
			continue;
		}
		for (i = 0; i < n; i++) {
			for (j = 0; j < ntried; j++)
				if (!strcmp(tried[j], names[i]))
					break;
			if (j < ntried)
				continue;
			if (ntried < MAX_CANDIDATES)
				strcpy(tried[ntried++], names[i]);
			logmsg("trying /dev/%s", names[i]);
			if (try_stick(names[i]) == 0)
				return 0;
		}
		if (now_ms() >= deadline)
			break;
		sleep_ms(POLL_MS);
	}
	logmsg("no stick with a good image after %d s", wait_s);
	return -1;
}

/* stick: squashfs in RAM over the stock root, upper in RAM, /data read-only */
static int boot_stick(int wait_s)
{
	int base = mdepth, ret;

	if (find_stick(wait_s) < 0)
		return -1;
	if (mount_squashfs(RAM_IMAGE, MNT_SQUASHFS) < 0)
		goto fail;
	if (push_mount("abrw", MNT_RW, "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755") < 0)
		goto fail;
	if (make_dir(MNT_RW "/upper", 0755) < 0 || make_dir(MNT_RW "/work", 0755) < 0)
		goto fail;
	if (push_mount("overlay", MNT_ROOT, "overlay", 0,
		       "lowerdir=" MNT_SQUASHFS ":" MNT_NEWROOT ",upperdir=" MNT_RW "/upper,workdir=" MNT_RW "/work") < 0)
		goto fail;
	/* never written: read-only, and noload so not even a journal replay touches it */
	if (wait_for_node(DEV_DATA, DATA_WAIT_MS) < 0 ||
	    push_mount(DEV_DATA, MNT_ROOT MNT_DATA, "ext4",
		       MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOATIME, "noload") < 0)
		logmsg("continuing without %s", MNT_DATA);
	ret = switch_root(MNT_ROOT, "stick");
	if (ret == -2)
		return -2;
fail:
	logmsg("stick path failed, cleaning up");
	unwind(base);
	return -1;
}

/* is there a legacy eMMC overlay (/data/autobleem/rootfs)? looked at read-only */
static int legacy_present(void)
{
	int present;

	if (wait_for_node(DEV_DATA, DATA_WAIT_MS) < 0)
		return 0;
	if (push_mount(DEV_DATA, MNT_DATA, "ext4", MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOATIME,
		       "noload") < 0)
		return 0;
	present = is_dir(LEGACY_DIR "/rootfs");
	logmsg("legacy %s/rootfs %s", LEGACY_DIR, present ? "found" : "not found");
	pop_mount(MNT_DATA);
	return present;
}

/* legacy: the 2020 init's layout, upper and work on /data/autobleem (eMMC, read-write) */
static int boot_legacy(void)
{
	const char *lower = "lowerdir=" MNT_NEWROOT;
	char opts[256];
	int base = mdepth, ret;

	if (push_mount(DEV_DATA, MNT_DATA, "ext4", MS_NOATIME | MS_NODIRATIME, NULL) < 0)
		goto fail;
	if (make_dir(LEGACY_DIR "/workdir", 0755) < 0)
		goto fail;
	if (is_file(LEGACY_DIR "/rootfs.squashfs")) {
		if (mount_squashfs(LEGACY_DIR "/rootfs.squashfs", MNT_SQUASHFS) == 0)
			lower = "lowerdir=" MNT_SQUASHFS ":" MNT_NEWROOT;
		else
			logmsg("ignoring %s/rootfs.squashfs", LEGACY_DIR);
	}
	snprintf(opts, sizeof(opts), "%s,upperdir=%s/rootfs,workdir=%s/workdir",
		 lower, LEGACY_DIR, LEGACY_DIR);
	if (push_mount("overlay", MNT_ROOT, "overlay", 0, opts) < 0)
		goto fail;
	if (move_stacked(MNT_DATA, MNT_ROOT MNT_DATA) < 0)
		goto fail;
	if (push_mount("tmpfs", MNT_ROOT "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777") < 0)
		logmsg("continuing without a tmpfs on /tmp");
	ret = switch_root(MNT_ROOT, "legacy");
	if (ret == -2)
		return -2;
fail:
	logmsg("legacy path failed, cleaning up");
	unwind(base);
	return -1;
}

int main(int argc, char **argv)
{
	int wait_s, i;

	(void)argc;
	init_argv = argv;
	early_mounts();
	logmsg("start");

	wait_s = stick_wait_seconds();
	if (wait_for_node(DEV_ROOT, ROOT_WAIT_MS) < 0 ||
	    push_mount(DEV_ROOT, MNT_NEWROOT, "ext4", MS_RDONLY | MS_NOATIME | MS_NODIRATIME, NULL) < 0) {
		logmsg("no stock root, nothing to boot");
		goto last_resort;
	}
	rear_port();

	if (wait_s == 0)
		logmsg("ab.wait=0: not looking for a stick");
	else if (boot_stick(wait_s) == -2)
		goto last_resort;

	if (legacy_present() && boot_legacy() == -2)
		goto last_resort;

	/* stock: the root as it is, /data left to the stock system */
	for (i = 0; i < 2; i++) {
		logmsg("booting stock");
		if (switch_root(MNT_NEWROOT, "stock") == -2)
			break;
	}

last_resort:
	init_argv[0] = (char *)"/sbin/init";
	for (i = 0; i < 2; i++) {
		if (!TEST)
			execv("/sbin/init", init_argv);
		logmsg("exec /sbin/init: %s", TEST ? "skipped in test" : strerror(errno));
	}
	if (TEST) {
		printf("ABINIT_RESULT=halt\n");
		return 0;
	}
	/* PID 1 must never exit */
	logmsg("nothing left to boot, halting");
	for (;;)
		sleep(60);
}
