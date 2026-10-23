/*
 * hidkbd - USB HID keyboard over FunctionFS (freestanding, no libc, aarch64 Linux)
 *
 * usage: hidkbd <ffs_mount_dir> <ctl_fifo> <status_file> [abort_input_dev]
 *
 * Writes HID descriptors to <ffs>/ep0, serves control requests, and runs
 * typing jobs requested through <ctl_fifo> (one command per line):
 *   type <jp|us> <normal|invert|auto> <key_delay_ms> <hold_ms> <file>
 *     hold_ms: minimum time each key state (shift down / key down / released)
 *     stays visible; needed by hosts that sample the keyboard per frame.
 *   abort
 * Progress is written to <status_file> as "<state> <pos> <total>".
 * Volume-down on <abort_input_dev> also aborts the running job.
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef long ssize_t;
typedef unsigned long size_t;

/* ---------------------------------------------------------------- syscalls */
#define SYS_openat 56
#define SYS_close 57
#define SYS_lseek 62
#define SYS_read 63
#define SYS_write 64
#define SYS_ppoll 73
#define SYS_exit_group 94
#define SYS_nanosleep 101

static long sc6(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	__asm__ volatile("svc 0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
			 : "memory", "cc");
	return x0;
}
#define sc(n, a, b, c) sc6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)

#define AT_FDCWD -100
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_TRUNC 01000
#define O_NONBLOCK 04000
#define POLLIN 1
#define EINTR 4
#define EAGAIN 11

struct pollfd { int fd; short events, revents; };
struct timespec { long sec, nsec; };

static int sys_open(const char *p, int fl) { return (int)sc6(SYS_openat, AT_FDCWD, (long)p, fl, 0644, 0, 0); }
static void sys_close(int fd) { sc(SYS_close, fd, 0, 0); }
static long sys_lseek(int fd, long off, int whence) { return sc(SYS_lseek, fd, off, whence); }
static ssize_t sys_read(int fd, void *b, size_t n) { return sc(SYS_read, fd, b, n); }
static ssize_t sys_write(int fd, const void *b, size_t n) { return sc(SYS_write, fd, b, n); }
static int sys_ppoll(struct pollfd *f, int n, struct timespec *t) { return (int)sc6(SYS_ppoll, (long)f, n, (long)t, 0, 8, 0); }
static void sys_exit(int c) { sc(SYS_exit_group, c, 0, 0); for (;;) ; }
static void msleep(int ms)
{
	struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
	sc(SYS_nanosleep, &t, 0, 0);
}

/* clang may emit these */
void *memset(void *d, int c, size_t n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }

static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static void out(const char *s) { sys_write(1, s, slen(s)); }
static char *fmtn(char *b, long v) /* writes decimal into b, returns end */
{
	char t[24]; int i = 0;
	if (v < 0) { *b++ = '-'; v = -v; }
	do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (i) *b++ = t[--i];
	*b = 0;
	return b;
}
static void outn(long v) { char b[24]; fmtn(b, v); out(b); }
static void outx(unsigned v, int digits)
{
	char b[9]; int i;
	for (i = digits - 1; i >= 0; i--) { b[i] = "0123456789abcdef"[v & 15]; v >>= 4; }
	b[digits] = 0;
	out(b);
}
static int atoi_(const char *s) { int v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }

/* ------------------------------------------------------------- descriptors */
static const u8 report_desc[] = {
	0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,             /* Generic Desktop / Keyboard / Application */
	0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,             /* modifiers E0..E7 */
	0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x95, 0x01, 0x75, 0x08, 0x81, 0x01,             /* reserved byte */
	0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, /* LEDs */
	0x95, 0x01, 0x75, 0x03, 0x91, 0x01,             /* LED padding */
	0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xFF, 0x00,
	0x05, 0x07, 0x19, 0x00, 0x2A, 0xFF, 0x00, 0x81, 0x00, /* 6 keys, usages 0..255 */
	0xC0,
};

#define LE16(v) (u8)((v) & 0xff), (u8)(((v) >> 8) & 0xff)
#define LE32(v) (u8)((v) & 0xff), (u8)(((v) >> 8) & 0xff), (u8)(((v) >> 16) & 0xff), (u8)(((v) >> 24) & 0xff)

#define IFACE_DESC 9, 4, 0, 0, 1, 3, 1, 1, 1 /* 1 EP, HID, boot, keyboard, iInterface=1 */
#define HID_DESC 9, 0x21, LE16(0x0111), 0, 1, 0x22, LE16(sizeof(report_desc))
#define EP_DESC(interval) 7, 5, 0x81, 3, LE16(8), interval

#define DESCS_LEN (12 + 8 + 2 * (9 + 9 + 7))
static const u8 descs[DESCS_LEN] = {
	LE32(3),         /* FUNCTIONFS_DESCRIPTORS_MAGIC_V2 */
	LE32(DESCS_LEN),
	LE32(1 | 2),     /* HAS_FS_DESC | HAS_HS_DESC */
	LE32(3), LE32(3), /* fs_count, hs_count */
	IFACE_DESC, HID_DESC, EP_DESC(1),  /* full speed: 1 ms */
	IFACE_DESC, HID_DESC, EP_DESC(4),  /* high speed: 2^(4-1)*125us = 1 ms */
};

#define STR "Android HID Keyboard"
#define STRS_LEN (16 + 2 + sizeof(STR))
static const u8 strs[STRS_LEN] = {
	LE32(2), LE32(STRS_LEN), LE32(1), LE32(1), /* STRINGS_MAGIC, len, str_count, lang_count */
	LE16(0x0409),
	'A','n','d','r','o','i','d',' ','H','I','D',' ','K','e','y','b','o','a','r','d', 0,
};

/* ------------------------------------------------------------------ ep0 */
struct ctrlreq { u8 bRequestType, bRequest; u16 wValue, wIndex, wLength; };
struct ffs_event { struct ctrlreq setup; u8 type; u8 pad[3]; };
enum { EV_BIND, EV_UNBIND, EV_ENABLE, EV_DISABLE, EV_SETUP, EV_SUSPEND, EV_RESUME };

static int ep0 = -1, ep1 = -1, ctl = -1, kfd = -1;
static int enabled = 0;
static u8 protocol = 1, idle_rate = 0, leds = 0;

static void ep0_reply(const void *data, int len, int wlen)
{
	if (len > wlen) len = wlen;
	sys_write(ep0, data, len);
}
static void ep0_ack(void) { u8 b[1]; sys_read(ep0, b, 0); }
static void ep0_stall(const struct ctrlreq *r)
{
	u8 b[1];
	if (r->bRequestType & 0x80) sys_read(ep0, b, 0); else sys_write(ep0, b, 0);
}

static void set_leds(u8 v)
{
	if (v != leds) { out("leds "); outx(v, 2); out(v & 2 ? " (caps on)\n" : "\n"); }
	leds = v;
}

static void handle_setup(const struct ctrlreq *r)
{
	u8 hid_desc[] = { HID_DESC };
	u8 rep[8] = { 0 };
	u8 buf[64];

	if (r->bRequestType == 0x81 && r->bRequest == 6) {          /* GET_DESCRIPTOR (interface) */
		if ((r->wValue >> 8) == 0x22) { ep0_reply(report_desc, sizeof(report_desc), r->wLength); return; }
		if ((r->wValue >> 8) == 0x21) { ep0_reply(hid_desc, sizeof(hid_desc), r->wLength); return; }
	} else if (r->bRequestType == 0xA1) {                         /* class IN */
		switch (r->bRequest) {
		case 0x01: ep0_reply(rep, 8, r->wLength); return;         /* GET_REPORT */
		case 0x02: ep0_reply(&idle_rate, 1, r->wLength); return;  /* GET_IDLE */
		case 0x03: ep0_reply(&protocol, 1, r->wLength); return;   /* GET_PROTOCOL */
		}
	} else if (r->bRequestType == 0x21) {                         /* class OUT */
		switch (r->bRequest) {
		case 0x09:                                                /* SET_REPORT (LEDs) */
			if (r->wLength) {
				ssize_t n = sys_read(ep0, buf, r->wLength < sizeof(buf) ? r->wLength : sizeof(buf));
				if (n > 0) set_leds(buf[0]);
			} else ep0_ack();
			return;
		case 0x0A: idle_rate = (u8)(r->wValue >> 8); ep0_ack(); return; /* SET_IDLE */
		case 0x0B: protocol = (u8)r->wValue; ep0_ack(); return;   /* SET_PROTOCOL */
		}
	}
	out("unhandled setup "); outx(r->bRequestType, 2); out(" "); outx(r->bRequest, 2);
	out(" "); outx(r->wValue, 4); out("\n");
	ep0_stall(r);
}

static const char *ev_name[] = { "BIND", "UNBIND", "ENABLE", "DISABLE", "SETUP", "SUSPEND", "RESUME" };

static void handle_ep0(void)
{
	struct ffs_event ev[4];
	ssize_t n = sys_read(ep0, ev, sizeof(ev));
	int i;
	if (n < 0) {
		if (n == -EINTR || n == -EAGAIN) return;
		out("ep0 read error "); outn(n); out("\n");
		sys_exit(1);
	}
	for (i = 0; i < (int)(n / sizeof(ev[0])); i++) {
		u8 t = ev[i].type;
		if (t != EV_SETUP) { out("event "); out(t <= EV_RESUME ? ev_name[t] : "?"); out("\n"); }
		if (t == EV_ENABLE) enabled = 1;
		else if (t == EV_DISABLE || t == EV_UNBIND) enabled = 0;
		else if (t == EV_SETUP) handle_setup(&ev[i].setup);
	}
}

/* --------------------------------------------------------------- keymaps */
#define S 0x80 /* shift flag in keymap entries (high byte) */
/* entry: (shift<<8) | usage ; 0 = unsupported */
static u16 km_us[128], km_jp[128];

static void km_common(u16 *k)
{
	int c;
	for (c = 'a'; c <= 'z'; c++) { k[c] = (u16)(4 + c - 'a'); k[c - 32] = (u16)((S << 8) | (4 + c - 'a')); }
	for (c = '1'; c <= '9'; c++) k[c] = (u16)(30 + c - '1');
	k['0'] = 39; k['\n'] = 40; k['\b'] = 42; k['\t'] = 43; k[' '] = 44;
	k[','] = 54; k['<'] = (S << 8) | 54; k['.'] = 55; k['>'] = (S << 8) | 55;
	k['/'] = 56; k['?'] = (S << 8) | 56; k['-'] = 45; k[';'] = 51;
	k['!'] = (S << 8) | 30; k['#'] = (S << 8) | 32; k['$'] = (S << 8) | 33; k['%'] = (S << 8) | 34;
	/* ESC (0x1b) is intentionally not typable: Esc on the PC is the abort key */
}

static void km_init(void)
{
	u16 *k = km_us;
	km_common(k);
	k['@'] = (S << 8) | 31; k['^'] = (S << 8) | 35; k['&'] = (S << 8) | 36; k['*'] = (S << 8) | 37;
	k['('] = (S << 8) | 38; k[')'] = (S << 8) | 39; k['_'] = (S << 8) | 45; k['='] = 46; k['+'] = (S << 8) | 46;
	k['['] = 47; k['{'] = (S << 8) | 47; k[']'] = 48; k['}'] = (S << 8) | 48; k['\\'] = 49; k['|'] = (S << 8) | 49;
	k[':'] = (S << 8) | 51; k['\''] = 52; k['"'] = (S << 8) | 52; k['`'] = 53; k['~'] = (S << 8) | 53;

	k = km_jp; /* Japanese 106/109 layout */
	km_common(k);
	k['"'] = (S << 8) | 31; k['&'] = (S << 8) | 35; k['\''] = (S << 8) | 36; k['('] = (S << 8) | 37;
	k[')'] = (S << 8) | 38; k['='] = (S << 8) | 45; k['^'] = 46; k['~'] = (S << 8) | 46;
	k['@'] = 47; k['`'] = (S << 8) | 47; k['['] = 48; k['{'] = (S << 8) | 48;
	k[']'] = 50; k['}'] = (S << 8) | 50; k['+'] = (S << 8) | 51; k[':'] = 52; k['*'] = (S << 8) | 52;
	k['\\'] = 0x87; k['_'] = (S << 8) | 0x87; k['|'] = (S << 8) | 0x89;
}

static int send_report(u8 mod, u8 key)
{
	u8 r[8] = { mod, 0, key, 0, 0, 0, 0, 0 };
	ssize_t n;
	do n = sys_write(ep1, r, 8); while (n == -EINTR);
	return n == 8 ? 0 : (int)n;
}

/* ------------------------------------------------------------------- jobs */
enum { CASE_NORMAL, CASE_INVERT, CASE_AUTO };
static const char *status_path;
static struct {
	int fd;                  /* -1 when idle */
	u16 *km;
	int case_mode, delay, hold;
	long pos, total, typed, skipped;
	u8 buf[4096];
	int blen, boff;
} job = { .fd = -1 };

static void write_status(const char *state)
{
	char b[80], *p = b;
	int fd;
	while (*state) *p++ = *state++;
	*p++ = ' '; p = fmtn(p, job.pos);
	*p++ = ' '; p = fmtn(p, job.total);
	*p++ = '\n';
	fd = sys_open(status_path, O_WRONLY | O_CREAT | O_TRUNC);
	if (fd >= 0) { sys_write(fd, b, (size_t)(p - b)); sys_close(fd); }
}

static void job_end(const char *state)
{
	send_report(0, 0);
	sys_close(job.fd);
	job.fd = -1;
	write_status(state);
	out(state); out(" at "); outn(job.pos); out("/"); outn(job.total);
	out(" (typed "); outn(job.typed); out(", skipped "); outn(job.skipped); out(")\n");
}

static void job_start(char *args)
{
	char *f[5];
	int n = 0, fd;
	while (n < 4 && *args) { /* layout, case, delay, hold, then the rest is the path */
		f[n++] = args;
		while (*args && *args != ' ') args++;
		if (*args) *args++ = 0;
	}
	f[4] = args;
	if (n < 4 || !*f[4]) { out("bad type command\n"); write_status("error"); return; }
	if (job.fd >= 0) job_end("aborted");
	fd = sys_open(f[4], O_RDONLY);
	if (fd < 0) { out("open failed "); out(f[4]); out("\n"); write_status("error"); return; }
	job.fd = fd;
	job.km = seq(f[0], "us") ? km_us : km_jp;
	job.case_mode = seq(f[1], "invert") ? CASE_INVERT : seq(f[1], "auto") ? CASE_AUTO : CASE_NORMAL;
	job.delay = atoi_(f[2]);
	job.hold = atoi_(f[3]);
	job.total = sys_lseek(fd, 0, 2);
	sys_lseek(fd, 0, 0);
	job.pos = job.typed = job.skipped = 0;
	job.blen = job.boff = 0;
	out("type "); out(f[0]); out(" "); out(f[1]); out(" delay="); out(f[2]); out(" hold="); out(f[3]); out(" "); out(f[4]);
	out(" ("); outn(job.total); out(" bytes)\n");
	write_status("typing");
}

static void handle_cmd(char *line)
{
	if (seq(line, "abort")) { if (job.fd >= 0) job_end("aborted"); }
	else if (line[0] == 't' && line[1] == 'y' && line[2] == 'p' && line[3] == 'e' && line[4] == ' ') job_start(line + 5);
	else if (line[0]) { out("unknown command: "); out(line); out("\n"); }
}

static char cbuf[1024];
static int clen = 0;

static void handle_ctl(void)
{
	ssize_t n = sys_read(ctl, cbuf + clen, sizeof(cbuf) - 1 - clen);
	int i, s = 0;
	if (n <= 0) return;
	clen += (int)n;
	for (i = 0; i < clen; i++) {
		if (cbuf[i] == '\n') {
			cbuf[i] = 0;
			handle_cmd(cbuf + s);
			s = i + 1;
		}
	}
	memcpy(cbuf, cbuf + s, (size_t)(clen - s));
	clen -= s;
	if (clen == sizeof(cbuf) - 1) clen = 0; /* overlong line: drop */
}

struct input_event { long sec, usec; u16 type, code; int value; };

static void handle_key(void)
{
	struct input_event ev[16];
	ssize_t n = sys_read(kfd, ev, sizeof(ev));
	int i;
	for (i = 0; i < (int)(n / (ssize_t)sizeof(ev[0])); i++)
		if (ev[i].type == 1 && ev[i].code == 114 && ev[i].value == 1 && job.fd >= 0) { /* KEY_VOLUMEDOWN */
			out("volume-down: ");
			job_end("aborted");
		}
}

static void type_next(void)
{
	u8 c;
	u16 e;
	int shift, rc, inv;

	if (job.boff >= job.blen) {
		ssize_t n = sys_read(job.fd, job.buf, sizeof(job.buf));
		if (n <= 0) { job_end(n < 0 ? "error" : "done"); return; }
		job.blen = (int)n;
		job.boff = 0;
	}
	c = job.buf[job.boff++];
	job.pos++;
	e = c < 128 ? job.km[c] : 0;
	if (c == '\r') return;
	if (!e) { job.skipped++; return; }

	shift = (e >> 8) != 0;
	inv = job.case_mode == CASE_INVERT || (job.case_mode == CASE_AUTO && (leds & 2));
	if (inv && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) shift = !shift;

	rc = 0;
	if (job.hold && shift) { rc = send_report(0x02, 0); msleep(job.hold); } /* shift first */
	if (!rc) rc = send_report(shift ? 0x02 : 0, (u8)(e & 0xff));
	if (!rc && job.hold) msleep(job.hold);
	if (!rc) rc = send_report(0, 0);
	if (!rc && job.hold) msleep(job.hold);
	if (rc) { out("ep1 write error "); outn(rc); out("\n"); job_end("error"); return; }
	job.typed++;
	if ((job.pos & 31) == 0) write_status("typing");
	if (job.delay) msleep(job.delay);
}

/* ------------------------------------------------------------------ main */
static int main(int argc, char **argv)
{
	char path[256];
	size_t l;

	if (argc < 4) { out("usage: hidkbd <ffs_dir> <ctl_fifo> <status_file> [abort_input_dev]\n"); return 2; }
	km_init();
	status_path = argv[3];

	l = slen(argv[1]);
	if (l > 200) return 2;
	memcpy(path, argv[1], l);
	memcpy(path + l, "/ep0", 5);
	ep0 = sys_open(path, O_RDWR);
	if (ep0 < 0) { out("open ep0 failed "); outn(ep0); out("\n"); return 1; }
	if (sys_write(ep0, descs, sizeof(descs)) != (ssize_t)sizeof(descs)) { out("write descriptors failed\n"); return 1; }
	if (sys_write(ep0, strs, sizeof(strs)) != (ssize_t)sizeof(strs)) { out("write strings failed\n"); return 1; }
	out("descriptors written\n");

	memcpy(path + l, "/ep1", 5);
	ep1 = sys_open(path, O_RDWR);
	if (ep1 < 0) { out("open ep1 failed "); outn(ep1); out("\n"); return 1; }

	ctl = sys_open(argv[2], O_RDWR); /* RDWR: never sees EOF when writers close */
	if (ctl < 0) { out("open ctl fifo failed "); outn(ctl); out("\n"); return 1; }
	if (argc > 4) {
		kfd = sys_open(argv[4], O_RDONLY | O_NONBLOCK);
		if (kfd < 0) { out("warning: cannot open "); out(argv[4]); out("\n"); }
	}
	write_status("idle");
	out("ready\n");

	for (;;) {
		struct pollfd p[3] = { { ep0, POLLIN, 0 }, { ctl, POLLIN, 0 }, { kfd, POLLIN, 0 } };
		struct timespec zero = { 0, 0 };
		int busy = job.fd >= 0 && enabled;
		if (sys_ppoll(p, kfd >= 0 ? 3 : 2, busy ? &zero : 0) < 0) continue;
		if (p[0].revents & POLLIN) handle_ep0();
		if (p[1].revents & POLLIN) handle_ctl();
		if (kfd >= 0 && (p[2].revents & POLLIN)) handle_key();
		if (job.fd >= 0 && enabled) type_next();
	}
}

__asm__(".global _start\n_start:\n mov x0, sp\n bl cstart\n");
void cstart(long *sp) { sys_exit(main((int)sp[0], (char **)(sp + 1))); }
