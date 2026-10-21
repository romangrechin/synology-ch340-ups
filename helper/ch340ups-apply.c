/*
 * Setuid-root helper: installs blazer_ser and patches DSM's UPS tables.
 * DSM 7 denies third-party packages root, so this is the only route, and
 * /dev/ttyUSB0 (0600 root:root) cannot be reached without it.
 *
 * The package directory is writable by the unprivileged package account, so
 * nothing from it is trusted: the driver is pinned by a build-time SHA-256,
 * hashed and copied through one fd, and no path comes from argv or the env.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TARGET_DIR   "/var/packages/CH340UPS/target"
#define DRIVER_SRC   TARGET_DIR "/blazer_ser"
#define DRIVER_DST   "/usr/bin/blazer_ser"
#define DRIVER_TMP   "/usr/bin/.ch340ups-blazer_ser.tmp"
#define UPS_TABLE    "/lib/udev/devicetable/usb.nut-hid"
#define SCAN_ETC     "/etc/ups/nutscan-usb.h"
#define SCAN_DEF     "/etc.defaults/ups/nutscan-usb.h"
#define DRV_CACHE    "/tmp/ups/ups_drv.list"
#define USB_DEVICES  "/proc/bus/usb/devices"
#define DRIVER_SOCK  "/run/ups_state/blazer_ser-ups"
#define LOG_PATH     "/var/log/ch340ups.log"

#define COMMENT_LINE "#  Megatec UPS on a CH340/CH341 serial bridge"
#define SCAN_ANCHOR  "static usb_device_id_t usb_device_table[] = {"

/* QinHeng serial bridges. Listing one does not make DSM trust the device:
 * the driver still accepts a port only after it answers the Megatec query. */
static const struct {
	const char	*vid;
	const char	*pid;
} bridges[] = {
	{ "1a86", "7523" },	/* CH340 */
	{ "1a86", "5523" },	/* CH341 */
	{ "1a86", "7522" },	/* CH340K / CH340E */
	{ "4348", "5523" }	/* CH341, older ID */
};

#define NBRIDGES (sizeof(bridges) / sizeof(bridges[0]))

/* ------------------------------------------------------------------- log */

static void logmsg(const char *fmt, ...)
{
	va_list		ap;
	FILE		*fh;
	char		stamp[32];
	time_t		now = time(NULL);
	struct tm	tm;

	localtime_r(&now, &tm);
	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);

	fh = fopen(LOG_PATH, "a");

	if (fh) {
		va_start(ap, fmt);
		fprintf(fh, "%s  ", stamp);
		vfprintf(fh, fmt, ap);
		fputc('\n', fh);
		va_end(ap);
		fclose(fh);
	}

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* ---------------------------------------------------------------- sha256 */

typedef struct {
	uint32_t	state[8];
	uint64_t	bits;
	uint8_t		buf[64];
	size_t		len;
} sha256_t;

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
	0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
	0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
	0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_t *c, const uint8_t *p)
{
	uint32_t	w[64], a, b, cc, d, e, f, g, h, t1, t2;
	int		i;

	for (i = 0; i < 16; i++) {
		w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
		       ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
	}

	for (i = 16; i < 64; i++) {
		uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	a = c->state[0]; b = c->state[1]; cc = c->state[2]; d = c->state[3];
	e = c->state[4]; f = c->state[5]; g = c->state[6]; h = c->state[7];

	for (i = 0; i < 64; i++) {
		uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
		uint32_t ch = (e & f) ^ ((~e) & g);
		uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
		uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);

		t1 = h + S1 + ch + K[i] + w[i];
		t2 = S0 + mj;

		h = g; g = f; f = e; e = d + t1;
		d = cc; cc = b; b = a; a = t1 + t2;
	}

	c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
	c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

static void sha256_init(sha256_t *c)
{
	static const uint32_t iv[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
	};

	memcpy(c->state, iv, sizeof(iv));
	c->bits = 0;
	c->len = 0;
}

static void sha256_update(sha256_t *c, const uint8_t *p, size_t n)
{
	c->bits += (uint64_t)n * 8;

	while (n > 0) {
		size_t take = 64 - c->len;

		if (take > n) {
			take = n;
		}

		memcpy(c->buf + c->len, p, take);
		c->len += take;
		p += take;
		n -= take;

		if (c->len == 64) {
			sha256_block(c, c->buf);
			c->len = 0;
		}
	}
}

static void sha256_hex(sha256_t *c, char *out)
{
	uint8_t		pad[72];
	size_t		padlen;
	uint64_t	bits = c->bits;
	int		i;

	memset(pad, 0, sizeof(pad));
	pad[0] = 0x80;
	padlen = (c->len < 56) ? (56 - c->len) : (120 - c->len);

	for (i = 0; i < 8; i++) {
		pad[padlen + i] = (uint8_t)(bits >> (56 - 8 * i));
	}

	sha256_update(c, pad, padlen + 8);

	for (i = 0; i < 8; i++) {
		sprintf(out + i * 8, "%08x", c->state[i]);
	}
}

static int hash_fd(int fd, char *hex)
{
	sha256_t	c;
	uint8_t		buf[65536];
	ssize_t		n;

	if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
		return -1;
	}

	sha256_init(&c);

	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		sha256_update(&c, buf, (size_t)n);
	}

	if (n < 0) {
		return -1;
	}

	sha256_hex(&c, hex);
	return 0;
}

/* ------------------------------------------------------------- id tables */

static void table_match(char *out, size_t n, size_t i)
{
	snprintf(out, n, "libhidups      0x0003      0x%s   0x%s",
		bridges[i].vid, bridges[i].pid);
}

static void table_line(char *out, size_t n, size_t i)
{
	char	m[128];

	table_match(m, sizeof(m), i);
	snprintf(out, n, "%s    0x0000       0x0000       0x00         0x00"
		"            0x00            0x00            0x00"
		"               0x00               0x00000000", m);
}

static void scan_match(char *out, size_t n, size_t i)
{
	snprintf(out, n, "{ 0x%s, 0x%s,", bridges[i].vid, bridges[i].pid);
}

static void scan_line(char *out, size_t n, size_t i)
{
	snprintf(out, n, "\t{ 0x%s, 0x%s, \"blazer_ser\" },",
		bridges[i].vid, bridges[i].pid);
}

static void usb_match(char *out, size_t n, size_t i)
{
	snprintf(out, n, "Vendor=%s ProdID=%s", bridges[i].vid, bridges[i].pid);
}

/* ----------------------------------------------------------------- files */

static int file_contains(const char *path, const char *needle)
{
	FILE	*fh = fopen(path, "r");
	char	line[1024];
	int	found = 0;

	if (!fh) {
		return 0;
	}

	while (fgets(line, sizeof(line), fh)) {
		if (strstr(line, needle)) {
			found = 1;
			break;
		}
	}

	fclose(fh);
	return found;
}

/* Temp file carrying the original's owner and mode, renamed into place. */
static int open_rewrite(const char *path, FILE **out, char *tmp, size_t tmplen)
{
	struct stat	st;
	int		fd;

	if (stat(path, &st) != 0) {
		return -1;
	}

	snprintf(tmp, tmplen, "%s.ch340ups.tmp", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	if (fd < 0) {
		return -1;
	}

	if (fchown(fd, st.st_uid, st.st_gid) != 0 || fchmod(fd, st.st_mode & 07777) != 0) {
		close(fd);
		unlink(tmp);
		return -1;
	}

	*out = fdopen(fd, "w");

	if (!*out) {
		close(fd);
		unlink(tmp);
		return -1;
	}

	return 0;
}

static int append_line(const char *path, const char *line)
{
	FILE	*fh = fopen(path, "a");

	if (!fh) {
		return -1;
	}

	fprintf(fh, "%s\n", line);
	return fclose(fh) == 0 ? 0 : -1;
}

static int insert_after(const char *path, const char *anchor, const char *line)
{
	FILE	*in, *out;
	char	tmp[512], buf[1024];
	int	done = 0;

	in = fopen(path, "r");

	if (!in) {
		return -1;
	}

	if (open_rewrite(path, &out, tmp, sizeof(tmp)) != 0) {
		fclose(in);
		return -1;
	}

	while (fgets(buf, sizeof(buf), in)) {
		fputs(buf, out);

		if (!done && strstr(buf, anchor)) {
			fprintf(out, "%s\n", line);
			done = 1;
		}
	}

	fclose(in);

	if (fclose(out) != 0 || !done || rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}

	return 0;
}

static int drop_lines(const char *path, const char *needle)
{
	FILE	*in, *out;
	char	tmp[512], buf[1024];

	in = fopen(path, "r");

	if (!in) {
		return 0;
	}

	if (open_rewrite(path, &out, tmp, sizeof(tmp)) != 0) {
		fclose(in);
		return -1;
	}

	while (fgets(buf, sizeof(buf), in)) {
		if (!strstr(buf, needle)) {
			fputs(buf, out);
		}
	}

	fclose(in);

	if (fclose(out) != 0 || rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}

	return 0;
}

static int copy_driver(int fd)
{
	uint8_t	buf[65536];
	ssize_t	n;
	int	dst;

	if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
		return -1;
	}

	dst = open(DRIVER_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0700);

	if (dst < 0) {
		return -1;
	}

	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		ssize_t off = 0;

		while (off < n) {
			ssize_t w = write(dst, buf + off, (size_t)(n - off));

			if (w <= 0) {
				close(dst);
				unlink(DRIVER_TMP);
				return -1;
			}

			off += w;
		}
	}

	if (n < 0 || fchown(dst, 0, 0) != 0 || fchmod(dst, 0755) != 0) {
		close(dst);
		unlink(DRIVER_TMP);
		return -1;
	}

	if (close(dst) != 0 || rename(DRIVER_TMP, DRIVER_DST) != 0) {
		unlink(DRIVER_TMP);
		return -1;
	}

	return 0;
}

/* --------------------------------------------------------------- actions */

static void restart_ups(void)
{
	static char *const av[] = { "systemctl", "restart", "ups-usb", NULL };
	static char *const ev[] = { "PATH=/usr/bin:/bin:/usr/sbin:/sbin", NULL };
	pid_t	pid = fork();
	int	status;

	if (pid < 0) {
		logmsg("ERROR: fork failed: %s", strerror(errno));
		return;
	}

	if (pid == 0) {
		execve("/usr/bin/systemctl", av, ev);
		_exit(127);
	}

	if (waitpid(pid, &status, 0) < 0) {
		logmsg("ERROR: waitpid failed: %s", strerror(errno));
		return;
	}

	logmsg("systemctl restart ups-usb -> exit %d", WEXITSTATUS(status));
}

static int install_driver(int *changed)
{
	const char	*want = DRIVER_SHA256;
	char		got[65], cur_hex[65];
	int		fd, cur, needed = 0;

	fd = open(DRIVER_SRC, O_RDONLY);

	if (fd < 0) {
		logmsg("ERROR: cannot open %s: %s", DRIVER_SRC, strerror(errno));
		return -1;
	}

	if (hash_fd(fd, got) != 0) {
		logmsg("ERROR: cannot hash %s", DRIVER_SRC);
		close(fd);
		return -1;
	}

	if (strcmp(want, got) != 0) {
		logmsg("REFUSING: %s has SHA-256 %s, expected %s", DRIVER_SRC, got, want);
		close(fd);
		return -1;
	}

	cur = open(DRIVER_DST, O_RDONLY);

	if (cur < 0) {
		needed = 1;
	} else {
		if (hash_fd(cur, cur_hex) != 0 || strcmp(cur_hex, want) != 0) {
			needed = 1;
		}

		close(cur);
	}

	/* Copied from the same fd that was hashed, so no swap can slip in. */
	if (needed && copy_driver(fd) != 0) {
		logmsg("ERROR: cannot install %s: %s", DRIVER_DST, strerror(errno));
		close(fd);
		return -1;
	}

	if (needed) {
		*changed = 1;
		logmsg("installed %s", DRIVER_DST);
	}

	close(fd);
	return 0;
}

static int apply(void)
{
	const char	*scans[] = { SCAN_ETC, SCAN_DEF };
	char		m[192], l[384];
	int		changed = 0, present = 0;
	size_t		i, s;

	if (install_driver(&changed) != 0) {
		return 1;
	}

	/* Without these entries udev reports the cable as unsupported. */
	if (!file_contains(UPS_TABLE, COMMENT_LINE)) {
		append_line(UPS_TABLE, "");
		append_line(UPS_TABLE, COMMENT_LINE);
	}

	for (i = 0; i < NBRIDGES; i++) {
		table_match(m, sizeof(m), i);

		if (file_contains(UPS_TABLE, m)) {
			continue;
		}

		table_line(l, sizeof(l), i);

		if (append_line(UPS_TABLE, l) != 0) {
			logmsg("ERROR: cannot update %s", UPS_TABLE);
			return 1;
		}

		changed = 1;
		logmsg("added %s:%s to %s", bridges[i].vid, bridges[i].pid, UPS_TABLE);
	}

	/* /etc.defaults matters too: DSM restores /etc from it. */
	for (s = 0; s < sizeof(scans) / sizeof(scans[0]); s++) {
		if (access(scans[s], F_OK) != 0) {
			continue;
		}

		for (i = 0; i < NBRIDGES; i++) {
			scan_match(m, sizeof(m), i);

			if (file_contains(scans[s], m)) {
				continue;
			}

			scan_line(l, sizeof(l), i);

			if (insert_after(scans[s], SCAN_ANCHOR, l) != 0) {
				logmsg("ERROR: cannot update %s", scans[s]);
				return 1;
			}

			changed = 1;
			logmsg("added %s:%s to %s", bridges[i].vid, bridges[i].pid, scans[s]);
		}
	}

	unlink(DRV_CACHE);

	for (i = 0; i < NBRIDGES && !present; i++) {
		usb_match(m, sizeof(m), i);
		present = file_contains(USB_DEVICES, m);
	}

	if (!present) {
		logmsg("no CH340/CH341 device present - configuration applied only");
		return 0;
	}

	/* The state socket exists only while the driver is running. */
	if (changed || access(DRIVER_SOCK, F_OK) != 0) {
		restart_ups();
	} else {
		logmsg("driver already running, nothing to do");
	}

	return 0;
}

static int remove_all(void)
{
	char	m[192];
	size_t	i;

	if (unlink(DRIVER_DST) == 0) {
		logmsg("removed %s", DRIVER_DST);
	}

	for (i = 0; i < NBRIDGES; i++) {
		table_match(m, sizeof(m), i);
		drop_lines(UPS_TABLE, m);

		scan_match(m, sizeof(m), i);
		drop_lines(SCAN_ETC, m);
		drop_lines(SCAN_DEF, m);
	}

	drop_lines(UPS_TABLE, COMMENT_LINE);
	unlink(DRV_CACHE);

	logmsg("reverted DSM tables");
	restart_ups();
	return 0;
}

int main(int argc, char **argv)
{
	int	do_remove = 0;

	if (argc > 2) {
		fprintf(stderr, "usage: ch340ups-apply [--apply|--remove]\n");
		return 2;
	}

	if (argc == 2) {
		if (!strcmp(argv[1], "--remove")) {
			do_remove = 1;
		} else if (strcmp(argv[1], "--apply") != 0) {
			fprintf(stderr, "usage: ch340ups-apply [--apply|--remove]\n");
			return 2;
		}
	}

	if (geteuid() != 0) {
		fprintf(stderr, "ch340ups-apply must be installed setuid root\n");
		return 1;
	}

	if (setgid(0) != 0 || setuid(0) != 0) {
		fprintf(stderr, "cannot assume root: %s\n", strerror(errno));
		return 1;
	}

	umask(022);

	return do_remove ? remove_all() : apply();
}
