// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * a8-gpu-drm: replay a Mesa A8 recording through powervr's real
 * DRM interface (render node, CREATE_VM_CONTEXT, CREATE_BO, VM_MAP,
 * CREATE_CONTEXT, CREATE_FREE_LIST, CREATE_HWRT_DATASET, SUBMIT_JOBS, a
 * syncobj wait) and check what the GPU wrote. Static, no libc: raw aarch64
 * system calls, for the iPod's busybox initramfs.
 *
 *   a8-gpu-drm compute FILE.rpl   k_store_lid: 32 dwords 0x12345678 in the output BO
 *   a8-gpu-drm triangle FILE.rpl  tri-twiddled: the 64x64 target, checked the way the
 *                                 bring-up module did (unflipped, twiddled layout)
 *   a8-gpu-drm info               the device's GPU ID and heaps
 *   a8-gpu-drm kms W H eDP|DSI [nogpu|bypass]
 *                                 the display pipe (adbe) through KMS: mode, link, modeset,
 *                                 page flips, a powervr buffer scanned out (kms() below);
 *                                 one RESULT line per check
 *   a8-gpu-drm dry compute|triangle FILE.rpl
 *                                 walk the recording without a device (every ioctl succeeds,
 *                                 BOs are anonymous memory): the check then fails
 *
 * The .rpl files come from tools/gen-replay.py. One line starts "RESULT" (several for kms).
 * Exit status 0 is a pass.
 */
#include <linux/types.h>
#include <drm/drm.h>
#include <drm/pvr_drm.h>

typedef unsigned long uintptr_t;	/* for DRM_PVR_OBJ_ARRAY() */

typedef __u8 u8;
typedef __u32 u32;
typedef __s32 s32;
typedef __u64 u64;
typedef long ssize;
typedef unsigned long usize;

/* ---- system calls (aarch64) ---- */

#define SYS_ioctl	29
#define SYS_openat	56
#define SYS_close	57
#define SYS_read	63
#define SYS_write	64
#define SYS_exit_group	94
#define SYS_clock_gettime 113
#define SYS_munmap	215
#define SYS_mmap	222

#define AT_FDCWD	-100
#define O_RDONLY	0
#define O_RDWR		2
#define O_CLOEXEC	02000000
#define PROT_READ	1
#define PROT_WRITE	2
#define MAP_SHARED	1
#define MAP_PRIVATE	2
#define MAP_ANONYMOUS	0x20
#define CLOCK_MONOTONIC	1

static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;

	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
			 : "memory", "cc");
	return x0;
}

#define sys3(n, a, b, c) sys6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)

static __attribute__((noreturn)) void sys_exit(int code)
{
	for (;;)
		sys6(SYS_exit_group, code, 0, 0, 0, 0, 0);
}

static void *sys_mmap(void *addr, usize len, int prot, int flags, int fd, u64 off)
{
	return (void *)sys6(SYS_mmap, (long)addr, (long)len, prot, flags, fd, (long)off);
}

static int is_err(long v)
{
	return v < 0 && v > -4096;
}

/* The compiler may emit calls to these */
void *memset(void *d, int c, usize n)
{
	u8 *p = d;

	while (n--)
		*p++ = (u8)c;
	return d;
}

void *memcpy(void *d, const void *s, usize n)
{
	u8 *p = d;
	const u8 *q = s;

	while (n--)
		*p++ = *q++;
	return d;
}

int memcmp(const void *a, const void *b, usize n)
{
	const u8 *p = a, *q = b;

	for (; n--; p++, q++)
		if (*p != *q)
			return *p < *q ? -1 : 1;
	return 0;
}

/* ---- output ---- */

static char obuf[512];
static usize olen;

static void flush(void)
{
	if (olen)
		sys3(SYS_write, 1, obuf, olen);
	olen = 0;
}

static void putch(char c)
{
	if (olen == sizeof(obuf))
		flush();
	obuf[olen++] = c;
	if (c == '\n')
		flush();
}

static void puts_(const char *s)
{
	while (*s)
		putch(*s++);
}

static void putu(u64 v)
{
	char b[24];
	int i = 0;

	do {
		b[i++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (i--)
		putch(b[i]);
}

static void puth(u64 v)
{
	static const char hex[] = "0123456789abcdef";
	char b[18];
	int i = 0;

	do {
		b[i++] = hex[v & 15];
		v >>= 4;
	} while (v);
	puts_("0x");
	while (i--)
		putch(b[i]);
}

static void puti(long v)
{
	if (v < 0) {
		putch('-');
		v = -v;
	}
	putu((u64)v);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

/* ---- time ---- */

struct ts {
	long sec, nsec;
};

static u64 now_us(void)
{
	struct ts t;

	sys3(SYS_clock_gettime, CLOCK_MONOTONIC, &t, 0);
	return (u64)t.sec * 1000000 + t.nsec / 1000;
}

/* ---- the replay file (tools/gen-replay.py) ---- */

#define RPL_MAGIC	"A8RPLAY1"
enum {
	RPL_BO = 1,		/* u32 index, u32 kind (0 CPU, 1 PM protected), u64 size */
	RPL_MAP,		/* u32 index, u32 0, u64 va, u64 size */
	RPL_DATA,		/* u32 index, u32 0, u64 offset, bytes */
	RPL_FREELIST,		/* u32 index, u32 0, u64 va, u32 initial, max, grow, threshold */
	RPL_HWRT,		/* u32 free list 0, u32 free list 1, create_hwrt_dataset args */
	RPL_CONTEXT,		/* u32 DRM_PVR_CTX_TYPE_* */
	RPL_JOB,		/* u32 type, u32 flags, u32 data index, u32 0, stream */
	RPL_CHECK_STORE,	/* u64 va, u32 count, u32 value */
	RPL_CHECK_TRI,		/* u64 va */
	RPL_NAME,		/* text */
};

#define MAX_BOS		64
#define MAX_FREELISTS	4
#define MAX_JOBS	4
#define KIND_PM		1

static struct bo {
	u32 handle;
	u32 kind;
	u64 size;
	u8 *map;
	u64 va;		/* its first mapping, for the checks */
} bos[MAX_BOS];
static u32 nbos;
static u32 freelists[MAX_FREELISTS];
static int fd = -1;
static u32 vm, hwrt, ctx;

static int dry;		/* no device: every ioctl succeeds */
static u32 dry_handle;

static int ioc(unsigned long req, void *arg, const char *what)
{
	long r;

	if (dry) {
		/* every handle-returning argument struct of ours has its handle where the kernel puts it */
		if (req == DRM_IOCTL_PVR_CREATE_BO)
			((struct drm_pvr_ioctl_create_bo_args *)arg)->handle = ++dry_handle;
		else if (req == DRM_IOCTL_PVR_CREATE_VM_CONTEXT)
			((struct drm_pvr_ioctl_create_vm_context_args *)arg)->handle = ++dry_handle;
		else if (req == DRM_IOCTL_PVR_CREATE_FREE_LIST)
			((struct drm_pvr_ioctl_create_free_list_args *)arg)->handle = ++dry_handle;
		else if (req == DRM_IOCTL_PVR_CREATE_HWRT_DATASET)
			((struct drm_pvr_ioctl_create_hwrt_dataset_args *)arg)->handle = ++dry_handle;
		else if (req == DRM_IOCTL_PVR_CREATE_CONTEXT)
			((struct drm_pvr_ioctl_create_context_args *)arg)->handle = ++dry_handle;
		else if (req == DRM_IOCTL_SYNCOBJ_CREATE)
			((struct drm_syncobj_create *)arg)->handle = ++dry_handle;
		puts_("dry: ");
		puts_(what);
		putch('\n');
		return 0;
	}
	r = sys3(SYS_ioctl, fd, req, arg);

	if (r < 0) {
		puts_("a8-gpu-drm: ");
		puts_(what);
		puts_(" failed: ");
		puti(r);
		putch('\n');
	}
	return (int)r;
}

static const u8 *rpl;
static u64 rpl_size;

static u32 rd32(const u8 *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

static u64 rd64(const u8 *p)
{
	return rd32(p) | (u64)rd32(p + 4) << 32;
}

static int load(const char *path)
{
	int f = (int)sys6(SYS_openat, AT_FDCWD, (long)path, O_RDONLY | O_CLOEXEC, 0, 0, 0);
	u64 cap = 1 << 20, n = 0;
	u8 *buf;

	if (f < 0)
		return f;
	buf = sys_mmap(0, 64 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (is_err((long)buf))
		return -12;
	for (;;) {
		long r = sys3(SYS_read, f, buf + n, cap);

		if (r < 0)
			return (int)r;
		if (!r)
			break;
		n += r;
		if (n + cap > (64 << 20))
			return -27;
	}
	sys3(SYS_close, f, 0, 0);
	rpl = buf;
	rpl_size = n;
	return n >= 12 && !memcmp(buf, RPL_MAGIC, 8) ? 0 : -22;
}

/* ---- the triangle check (as the bring-up module checked it) ---- */

#define TRI_DIM		64
#define TRI_CLEAR_PX	0xff000033u
#define TRI_COLOUR_PX	0xffff4000u
enum { TL_TWID_YLO, TL_TWID_XLO, TL_LINEAR, TL_KINDS };
#define TL_FLIP		TL_KINDS
#define TL_COUNT	(2 * TL_KINDS)

static const char *const tl_name[TL_COUNT] = {
	"twiddled-y0", "twiddled-x0", "linear", "twiddled-y0-flipped", "twiddled-x0-flipped",
	"linear-flipped",
};

static u32 tri_index(unsigned int layout, u32 x, u32 y)
{
	u32 i, idx = 0;

	if (layout >= TL_FLIP)
		y = TRI_DIM - 1 - y;
	switch (layout % TL_KINDS) {
	case TL_LINEAR:
		return y * TRI_DIM + x;
	case TL_TWID_YLO:
		for (i = 0; i < 6; i++)
			idx |= ((y >> i) & 1) << (2 * i) | ((x >> i) & 1) << (2 * i + 1);
		return idx;
	default:
		for (i = 0; i < 6; i++)
			idx |= ((x >> i) & 1) << (2 * i) | ((y >> i) & 1) << (2 * i + 1);
		return idx;
	}
}

/* 0: clear, 1: triangle, 2: either (pixel centre within 0.75 px of an edge) */
static int tri_expect(u32 x, u32 y)
{
	static const int v[3][2] = { { 32, 32 }, { 96, 32 }, { 64, 96 } };
	long long px = 2 * x + 1, py = 2 * y + 1;
	int i, inside = 1, near = 0;

	for (i = 0; i < 3; i++) {
		long long ax = v[i][0], ay = v[i][1], bx = v[(i + 1) % 3][0], by = v[(i + 1) % 3][1];
		long long e = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
		long long len2 = (bx - ax) * (bx - ax) + (by - ay) * (by - ay);
		int close = 4 * e * e < 9 * len2;

		if (e <= 0 && !close)
			return 0;
		if (e <= 0)
			inside = 0;
		near |= close;
	}
	return near ? 2 : inside;
}

static int px_near(u32 a, u32 b)
{
	int i;

	for (i = 0; i < 32; i += 8) {
		int d = (int)((a >> i) & 0xff) - (int)((b >> i) & 0xff);

		if (d < -2 || d > 2)
			return 0;
	}
	return 1;
}

static int px_class(u32 v)
{
	return px_near(v, TRI_CLEAR_PX) ? 0 : px_near(v, TRI_COLOUR_PX) ? 1 : 2;
}

struct tri_result {
	u32 clear, colour, edge, other, bad;
};

static int check_triangle(const u8 *img)
{
	struct tri_result r[TL_COUNT];
	unsigned int l, best = 0;
	u32 x, y;
	int pass;

	for (l = 0; l < TL_COUNT; l++) {
		struct tri_result *t = &r[l];

		memset(t, 0, sizeof(*t));
		for (y = 0; y < TRI_DIM; y++)
			for (x = 0; x < TRI_DIM; x++) {
				u32 v = rd32(img + 4 * tri_index(l, x, y));
				int c = px_class(v), e = tri_expect(x, y);

				if (c == 2)
					t->other++;
				else if (e == 2)
					t->edge++;
				else if (c != e)
					t->bad++;
				else if (c)
					t->colour++;
				else
					t->clear++;
			}
		if (t->bad + t->other < r[best].bad + r[best].other)
			best = l;
		puts_("target as ");
		puts_(tl_name[l]);
		puts_(": clear ");
		putu(t->clear);
		puts_(" colour ");
		putu(t->colour);
		puts_(" edge ");
		putu(t->edge);
		puts_(" other ");
		putu(t->other);
		puts_(" bad ");
		putu(t->bad);
		putch('\n');
	}
	for (y = 0; y < 16; y++) {
		for (x = 0; x < 16; x++) {
			unsigned int n[3] = { 0, 0, 0 }, yy, xx;

			for (yy = 4 * y; yy < 4 * y + 4; yy++)
				for (xx = 4 * x; xx < 4 * x + 4; xx++)
					n[px_class(rd32(img + 4 * tri_index(best, xx, yy)))]++;
			putch(n[2] ? '?' : n[1] == 16 ? '#' : n[0] == 16 ? '.' : 'x');
		}
		putch('\n');
	}
	pass = best < TL_FLIP && best != TL_LINEAR && !r[best].bad && !r[best].other && r[best].colour;
	puts_("RESULT triangle ");
	puts_(pass ? "PASS" : "FAIL");
	puts_(" layout ");
	puts_(tl_name[best]);
	puts_(" colour ");
	putu(r[best].colour);
	puts_(" bad ");
	putu(r[best].bad);
	puts_(" other ");
	putu(r[best].other);
	puts_(" pixel(0,0) ");
	puth(rd32(img));
	puts_(" pixel(32,32) ");
	puth(rd32(img + 4 * tri_index(best, 32, 32)));
	putch('\n');
	return pass ? 0 : 1;
}

static struct bo *bo_at(u64 va, u64 len)
{
	u32 i;

	for (i = 0; i < nbos; i++)
		if (bos[i].va && va >= bos[i].va && va + len <= bos[i].va + bos[i].size)
			return &bos[i];
	return 0;
}

static int open_node(void)
{
	static const char *const nodes[] = { "/dev/dri/renderD128", "/dev/dri/renderD129",
					     "/dev/dri/renderD130" };
	unsigned int i;

	for (i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
		char name[16] = { 0 };
		struct drm_version v = { .name = name, .name_len = sizeof(name) - 1 };
		int f = (int)sys6(SYS_openat, AT_FDCWD, (long)nodes[i], O_RDWR | O_CLOEXEC, 0, 0, 0);

		if (f < 0)
			continue;
		if (!sys3(SYS_ioctl, f, DRM_IOCTL_VERSION, &v) && streq(name, "powervr"))
			return f;
		sys3(SYS_close, f, 0, 0);
	}
	return -19;
}

static int info(void)
{
	struct drm_pvr_dev_query_gpu_info gpu = { 0 };
	struct drm_pvr_heap heaps[8];
	struct drm_pvr_dev_query_heap_info hi = {
		.heaps = { .stride = sizeof(struct drm_pvr_heap), .count = 8, .array = (u64)heaps },
	};
	struct drm_pvr_ioctl_dev_query_args q = {
		.type = DRM_PVR_DEV_QUERY_GPU_INFO_GET, .pointer = (u64)&gpu, .size = sizeof(gpu),
	};
	u32 i;

	if (ioc(DRM_IOCTL_PVR_DEV_QUERY, &q, "DEV_QUERY gpu"))
		return 1;
	puts_("gpu id ");
	puth(gpu.gpu_id);
	puts_(" (BVNC ");
	putu(gpu.gpu_id >> 48);
	putch('.');
	putu((gpu.gpu_id >> 32) & 0xffff);
	putch('.');
	putu((gpu.gpu_id >> 16) & 0xffff);
	putch('.');
	putu(gpu.gpu_id & 0xffff);
	puts_(")\n");
	q = (struct drm_pvr_ioctl_dev_query_args) {
		.type = DRM_PVR_DEV_QUERY_HEAP_INFO_GET, .pointer = (u64)&hi, .size = sizeof(hi),
	};
	if (ioc(DRM_IOCTL_PVR_DEV_QUERY, &q, "DEV_QUERY heaps"))
		return 1;
	for (i = 0; i < hi.heaps.count && i < 8; i++) {
		puts_("heap ");
		putu(i);
		puts_(" base ");
		puth(heaps[i].base);
		puts_(" size ");
		puth(heaps[i].size);
		puts_(" page 2^");
		putu(heaps[i].page_size_log2);
		putch('\n');
	}
	puts_("RESULT info PASS\n");
	return 0;
}

static int replay(int triangle)
{
	struct drm_pvr_job jobs[MAX_JOBS];
	static u8 streams[MAX_JOBS][1024];
	struct drm_pvr_sync_op signal;
	struct drm_syncobj_create sync = { 0 };
	struct drm_pvr_ioctl_create_vm_context_args cvm = { 0 };
	u64 check_va = 0, start;
	u32 check_count = 0, check_value = 0, njobs = 0, nfl = 0, i;
	const u8 *p = rpl + 12, *end = rpl + rpl_size;
	s32 ctx_type = -1;
	int err;

	if (ioc(DRM_IOCTL_PVR_CREATE_VM_CONTEXT, &cvm, "CREATE_VM_CONTEXT"))
		return 2;
	vm = cvm.handle;
	while (p + 8 <= end) {
		u32 type = rd32(p), len = rd32(p + 4);
		const u8 *d = p + 8;

		if (d + len > end)
			return 2;
		p = d + len;
		switch (type) {
		case RPL_NAME:
			puts_("recording: ");
			for (i = 0; i < len; i++)
				putch(d[i]);
			putch('\n');
			break;
		case RPL_BO: {
			u32 idx = rd32(d);
			struct bo *b = &bos[idx];
			struct drm_pvr_ioctl_create_bo_args c = { .size = rd64(d + 8) };
			struct drm_pvr_ioctl_get_bo_mmap_offset_args o = { 0 };

			if (idx >= MAX_BOS)
				return 2;
			b->kind = rd32(d + 4);
			b->size = c.size;
			c.flags = b->kind == KIND_PM ? DRM_PVR_BO_PM_FW_PROTECT :
				  DRM_PVR_BO_BYPASS_DEVICE_CACHE | DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS;
			if (ioc(DRM_IOCTL_PVR_CREATE_BO, &c, "CREATE_BO"))
				return 2;
			b->handle = c.handle;
			if (b->kind != KIND_PM) {
				o.handle = b->handle;
				if (ioc(DRM_IOCTL_PVR_GET_BO_MMAP_OFFSET, &o, "GET_BO_MMAP_OFFSET"))
					return 2;
				b->map = dry ? sys_mmap(0, b->size, PROT_READ | PROT_WRITE,
							MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) :
					       sys_mmap(0, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
							o.offset);
				if (is_err((long)b->map)) {
					puts_("a8-gpu-drm: mmap failed\n");
					return 2;
				}
			}
			if (idx >= nbos)
				nbos = idx + 1;
			break;
		}
		case RPL_DATA: {
			struct bo *b = &bos[rd32(d)];
			u64 off = rd64(d + 8);

			if (!b->map || off + len - 16 > b->size)
				return 2;
			memcpy(b->map + off, d + 16, len - 16);
			break;
		}
		case RPL_MAP: {
			struct bo *b = &bos[rd32(d)];
			struct drm_pvr_ioctl_vm_map_args m = {
				.vm_context_handle = vm, .device_addr = rd64(d + 8), .handle = b->handle,
				.size = rd64(d + 16),
			};

			if (ioc(DRM_IOCTL_PVR_VM_MAP, &m, "VM_MAP"))
				return 2;
			if (!b->va)
				b->va = m.device_addr;
			break;
		}
		case RPL_FREELIST: {
			struct drm_pvr_ioctl_create_free_list_args f = {
				.free_list_gpu_addr = rd64(d + 8), .initial_num_pages = rd32(d + 16),
				.max_num_pages = rd32(d + 20), .grow_num_pages = rd32(d + 24),
				.grow_threshold = rd32(d + 28), .vm_context_handle = vm,
			};

			if (nfl >= MAX_FREELISTS || ioc(DRM_IOCTL_PVR_CREATE_FREE_LIST, &f, "CREATE_FREE_LIST"))
				return 2;
			freelists[rd32(d)] = f.handle;
			nfl++;
			break;
		}
		case RPL_HWRT: {
			struct drm_pvr_ioctl_create_hwrt_dataset_args h;

			if (len != 8 + sizeof(h))
				return 2;
			memcpy(&h, d + 8, sizeof(h));
			h.free_list_handles[0] = freelists[rd32(d)];
			h.free_list_handles[1] = freelists[rd32(d + 4)];
			h.handle = 0;
			if (ioc(DRM_IOCTL_PVR_CREATE_HWRT_DATASET, &h, "CREATE_HWRT_DATASET"))
				return 2;
			hwrt = h.handle;
			break;
		}
		case RPL_CONTEXT: {
			struct drm_pvr_ioctl_create_context_args c = {
				.type = rd32(d), .priority = DRM_PVR_CTX_PRIORITY_NORMAL,
				.vm_context_handle = vm,
			};

			if (ioc(DRM_IOCTL_PVR_CREATE_CONTEXT, &c, "CREATE_CONTEXT"))
				return 2;
			ctx = c.handle;
			ctx_type = (s32)c.type;
			break;
		}
		case RPL_JOB:
			if (njobs >= MAX_JOBS || len - 16 > sizeof(streams[0]))
				return 2;
			memcpy(streams[njobs], d + 16, len - 16);
			/*
			 * Every job's sync_ops carries the stride, even with no ops: the
			 * kernel checks the stride before the count (pvr_get_uobj_array),
			 * so a zeroed array is -EINVAL: the triangle's geometry
			 * job, the one job without the signal op.
			 */
			jobs[njobs] = (struct drm_pvr_job) {
				.type = rd32(d), .context_handle = ctx, .flags = rd32(d + 4),
				.cmd_stream_len = len - 16, .cmd_stream = (u64)streams[njobs],
				.sync_ops = DRM_PVR_OBJ_ARRAY(0, &signal),
			};
			if (jobs[njobs].type != DRM_PVR_JOB_TYPE_COMPUTE)
				jobs[njobs].hwrt = (struct drm_pvr_hwrt_data_ref) {
					.set_handle = hwrt, .data_index = rd32(d + 8),
				};
			njobs++;
			break;
		case RPL_CHECK_STORE:
			check_va = rd64(d);
			check_count = rd32(d + 8);
			check_value = rd32(d + 12);
			break;
		case RPL_CHECK_TRI:
			check_va = rd64(d);
			break;
		default:
			return 2;
		}
	}
	if (!njobs || ctx_type < 0 || !check_va || triangle != (check_count == 0))
		return 2;

	if (ioc(DRM_IOCTL_SYNCOBJ_CREATE, &sync, "SYNCOBJ_CREATE"))
		return 2;
	signal = (struct drm_pvr_sync_op) {
		.handle = sync.handle,
		.flags = DRM_PVR_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ | DRM_PVR_SYNC_OP_FLAG_SIGNAL,
	};
	jobs[njobs - 1].sync_ops = (struct drm_pvr_obj_array) DRM_PVR_OBJ_ARRAY(1, &signal);
	{
		struct drm_pvr_ioctl_submit_jobs_args s = {
			.jobs = DRM_PVR_OBJ_ARRAY(njobs, jobs),
		};
		struct drm_syncobj_wait w = {
			.handles = (u64)&sync.handle, .count_handles = 1,
		};
		struct ts t;

		puts_("submitting ");
		putu(njobs);
		puts_(triangle ? " render jobs\n" : " compute job\n");
		start = now_us();
		err = ioc(DRM_IOCTL_PVR_SUBMIT_JOBS, &s, "SUBMIT_JOBS");
		if (err) {
			puts_(triangle ? "RESULT triangle FAIL submit " : "RESULT compute FAIL submit ");
			puti(err);
			putch('\n');
			return 1;
		}
		sys3(SYS_clock_gettime, CLOCK_MONOTONIC, &t, 0);
		w.timeout_nsec = (t.sec + 10) * 1000000000ll + t.nsec;
		err = ioc(DRM_IOCTL_SYNCOBJ_WAIT, &w, "SYNCOBJ_WAIT");
		puts_("jobs ended after ");
		putu(now_us() - start);
		puts_(" us\n");
		if (err) {
			puts_(triangle ? "RESULT triangle FAIL wait " : "RESULT compute FAIL wait ");
			puti(err);
			putch('\n');
			return 1;
		}
	}

	if (triangle) {
		struct bo *b = bo_at(check_va, TRI_DIM * TRI_DIM * 4);

		if (!b || !b->map)
			return 2;
		return check_triangle(b->map + (check_va - b->va));
	}
	{
		struct bo *b = bo_at(check_va, 4ull * check_count);
		u32 good = 0, n;
		const u8 *out;

		if (!b || !b->map)
			return 2;
		out = b->map + (check_va - b->va);
		for (n = 0; n < check_count; n++)
			good += rd32(out + 4 * n) == check_value;
		puts_("out[0..3] ");
		for (n = 0; n < 4; n++) {
			puth(rd32(out + 4 * n));
			putch(' ');
		}
		putch('\n');
		puts_(good == check_count ? "RESULT compute PASS " : "RESULT compute FAIL ");
		putu(good);
		putch('/');
		putu(check_count);
		puts_(" dwords ");
		puth(check_value);
		puts_(" at ");
		puth(check_va);
		putch('\n');
		return good == check_count ? 0 : 1;
	}
}

/* ---- the display pipe (adbe) through KMS ---- */

#define SYS_ppoll	73
#define POLLIN		1
#define FMT_XRGB8888	(0x58 | 0x52 << 8 | 0x32 << 16 | 0x34 << 24)	/* 'XR24' */
#define KMS_FLIPS	30
#define KMS_WAIT_MS	500	/* one flip, or the pipe's interrupt is not arriving */

struct pollfd_ {
	int fd;
	short events, revents;
};

static int kfd = -1;
static u32 kms_minor;

static void result(const char *check, const char *verdict)
{
	puts_("RESULT display-");
	puts_(check);
	putch(' ');
	puts_(verdict);
}

static int kioc(unsigned long req, void *arg)
{
	long r = sys3(SYS_ioctl, kfd, req, arg);

	return r < 0 ? (int)r : 0;
}

/* The card the adbe driver registered; its minor is the debugfs directory. */
static int open_card(void)
{
	char path[] = "/dev/dri/card0";
	u32 i;

	for (i = 0; i < 4; i++) {
		char name[16] = { 0 };
		struct drm_version v = { .name = name, .name_len = sizeof(name) - 1 };
		int f;

		path[sizeof(path) - 2] = '0' + i;
		f = (int)sys6(SYS_openat, AT_FDCWD, (long)path, O_RDWR | O_CLOEXEC, 0, 0, 0);
		if (f < 0)
			continue;
		if (!sys3(SYS_ioctl, f, DRM_IOCTL_VERSION, &v) && streq(name, "adbe")) {
			kms_minor = i;
			return f;
		}
		sys3(SYS_close, f, 0, 0);
	}
	return -19;
}

struct kfb {
	u32 handle, pitch, fb;
	u64 size;
	u32 *map;
};

/* Vertical colour bars inside a white 8-pixel border; inverted: the bars swap order. */
static void pattern(u32 *px, u32 pitch, u32 w, u32 h, int inverted)
{
	static const u32 bars[8] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
				     0xff00ff, 0xff0000, 0x0000ff, 0x000000 };
	u32 x, y;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			u32 bar = x * 8 / w;
			u32 v = bars[inverted ? 7 - bar : bar];

			if (x < 8 || y < 8 || x >= w - 8 || y >= h - 8)
				v = 0xffffff;
			px[y * (pitch / 4) + x] = v;
		}
}

static int dumb_fb(struct kfb *f, u32 w, u32 h)
{
	struct drm_mode_create_dumb c = { .width = w, .height = h, .bpp = 32 };
	struct drm_mode_map_dumb m = { 0 };
	struct drm_mode_fb_cmd2 fb = { .width = w, .height = h, .pixel_format = FMT_XRGB8888 };
	int r;

	r = kioc(DRM_IOCTL_MODE_CREATE_DUMB, &c);
	if (r)
		return r;
	f->handle = c.handle;
	f->pitch = c.pitch;
	f->size = c.size;
	m.handle = c.handle;
	r = kioc(DRM_IOCTL_MODE_MAP_DUMB, &m);
	if (r)
		return r;
	f->map = sys_mmap(0, f->size, PROT_READ | PROT_WRITE, MAP_SHARED, kfd, m.offset);
	if (is_err((long)f->map))
		return (int)(long)f->map;
	fb.handles[0] = f->handle;
	fb.pitches[0] = f->pitch;
	r = kioc(DRM_IOCTL_MODE_ADDFB2, &fb);
	f->fb = fb.fb_id;
	return r;
}

/* One page flip to FB and its completion event: the event's time in us, or <0. */
static long flip(u32 crtc, u32 fb)
{
	struct drm_mode_crtc_page_flip pf = {
		.crtc_id = crtc, .fb_id = fb, .flags = DRM_MODE_PAGE_FLIP_EVENT, .user_data = fb,
	};
	struct pollfd_ p = { .fd = kfd, .events = POLLIN };
	struct ts t = { 0, KMS_WAIT_MS * 1000000L };
	struct drm_event_vblank ev;
	int r;

	r = kioc(DRM_IOCTL_MODE_PAGE_FLIP, &pf);
	if (r)
		return r;
	r = (int)sys6(SYS_ppoll, (long)&p, 1, (long)&t, 0, 0, 0);
	if (r <= 0)
		return r ? r : -62;	/* ETIME */
	r = (int)sys3(SYS_read, kfd, &ev, sizeof(ev));
	if (r != sizeof(ev) || ev.base.type != DRM_EVENT_FLIP_COMPLETE || ev.user_data != fb)
		return -71;	/* EPROTO */
	return (long)ev.tv_sec * 1000000 + ev.tv_usec;
}

static usize slurp(const char *path, char *buf, usize cap)
{
	int f = (int)sys6(SYS_openat, AT_FDCWD, (long)path, O_RDONLY | O_CLOEXEC, 0, 0, 0);
	usize n = 0;

	if (f < 0)
		return 0;
	while (n < cap - 1) {
		long r = sys3(SYS_read, f, buf + n, cap - 1 - n);

		if (r <= 0)
			break;
		n += r;
	}
	sys3(SYS_close, f, 0, 0);
	buf[n] = 0;
	return n;
}

static const char *find(const char *hay, const char *needle)
{
	for (; *hay; hay++) {
		const char *h = hay, *n = needle;

		while (*n && *h == *n)
			h++, n++;
		if (!*n)
			return hay;
	}
	return 0;
}

static void fmt_u(char *b, const char *prefix, u32 v)
{
	char d[12];
	int i = 0;

	while (*prefix)
		*b++ = *prefix++;
	do {
		d[i++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (i--)
		*b++ = d[i];
	*b = 0;
}

/*
 * debugfs: is FB on the primary plane (state), and is it an imported
 * buffer (framebuffer)? 1 yes, 0 no, -1 debugfs unreadable.
 */
static char dbg[32768];
static int fb_scanned_imported(u32 fb)
{
	char path[48], key[32];
	const char *at, *next;
	usize n;

	if (kms_minor > 9)
		return -1;
	fmt_u(path, "/sys/kernel/debug/dri/", kms_minor);	/* 22 + 1 characters */
	memcpy(path + 23, "/state", 7);
	if (!slurp(path, dbg, sizeof(dbg)))
		return -1;
	fmt_u(key, "fb=", fb);
	for (n = 0; key[n]; n++)
		;
	key[n] = '\n';
	key[n + 1] = 0;
	if (!find(dbg, key))
		return 0;
	memcpy(path + 23, "/framebuffer", 13);
	if (!slurp(path, dbg, sizeof(dbg)))
		return -1;
	fmt_u(key, "framebuffer[", fb);
	for (n = 0; key[n]; n++)
		;
	key[n] = ']';
	key[n + 1] = 0;
	at = find(dbg, key);
	if (!at)
		return 0;
	next = find(at + 1, "framebuffer[");
	at = find(at, "imported=yes");
	return at && (!next || at < next);
}

/*
 * a8-gpu-drm kms WIDTH HEIGHT eDP|DSI [nogpu|bypass]: on the adbe card, the
 * panel's mode and link, a modeset of a dumb buffer with a test pattern, timed
 * page flips, and a powervr buffer imported and scanned out through the
 * display DART (skipped with nogpu or bypass). One RESULT line per check;
 * exit 0 when all pass.
 */
static int kms(u32 want_w, u32 want_h, const char *want_link, const char *skip)
{
	u32 crtcs[4], conns[4], encs[4];
	static struct drm_mode_modeinfo modes[64];
	struct drm_mode_card_res res = { 0 };
	struct drm_mode_get_connector gc = { 0 };
	struct drm_mode_crtc sc = { 0 };
	struct kfb a = { 0 }, b = { 0 }, imp = { 0 };
	u32 i, w, h, conn, fails = 0, slow = 0;
	long t0, t, prev, worst = 0;
	const char *link;
	int r;

	kfd = open_card();
	if (kfd < 0) {
		result("card", "FAIL no adbe card in /dev/dri\n");
		return 1;
	}
	puts_("display card: /dev/dri/card");
	putu(kms_minor);
	putch('\n');
	sys3(SYS_ioctl, kfd, DRM_IOCTL_SET_MASTER, 0);

	res.crtc_id_ptr = (u64)crtcs;
	res.connector_id_ptr = (u64)conns;
	res.encoder_id_ptr = (u64)encs;
	res.count_crtcs = res.count_connectors = res.count_encoders = 4;
	if (kioc(DRM_IOCTL_MODE_GETRESOURCES, &res) || res.count_crtcs != 1 ||
	    res.count_connectors != 1) {
		result("mode", "FAIL GETRESOURCES: not one CRTC and one connector\n");
		return 1;
	}
	conn = conns[0];
	/* the first call, with no mode array, probes the connector */
	gc.connector_id = conn;
	r = kioc(DRM_IOCTL_MODE_GETCONNECTOR, &gc);
	if (!r && gc.count_modes >= 1 && gc.count_modes <= 64) {
		gc = (struct drm_mode_get_connector) {
			.connector_id = conn, .modes_ptr = (u64)modes, .count_modes = 64,
		};
		r = kioc(DRM_IOCTL_MODE_GETCONNECTOR, &gc);
	}
	if (r || gc.count_modes < 1 || gc.count_modes > 64) {
		result("mode", "FAIL GETCONNECTOR: no mode\n");
		return 1;
	}
	/* adbe offers the one mode iBoot left running */
	for (i = 0; i < gc.count_modes - 1; i++)
		if (modes[i].type & DRM_MODE_TYPE_PREFERRED)
			break;
	modes[0] = modes[i];
	w = modes[0].hdisplay;
	h = modes[0].vdisplay;
	link = gc.connector_type == DRM_MODE_CONNECTOR_eDP ? "eDP" :
	       gc.connector_type == DRM_MODE_CONNECTOR_DSI ? "DSI" : "other";

	result("mode", w == want_w && h == want_h && gc.connection == 1 ? "PASS " : "FAIL ");
	putu(w);
	putch('x');
	putu(h);
	puts_("@");
	putu(modes[0].vrefresh);
	puts_(" ");
	putu(gc.mm_width);
	putch('x');
	putu(gc.mm_height);
	puts_(" mm, expected ");
	putu(want_w);
	putch('x');
	putu(want_h);
	puts_(gc.connection == 1 ? ", connected\n" : ", NOT connected\n");
	fails += !(w == want_w && h == want_h && gc.connection == 1);

	result("link", streq(link, want_link) ? "PASS " : "FAIL ");
	puts_(link);
	puts_("-1, expected ");
	puts_(want_link);
	putch('\n');
	fails += !streq(link, want_link);

	/* Modeset: buffer A with the bars */
	r = dumb_fb(&a, w, h);
	if (r) {
		result("modeset", "FAIL dumb buffer: ");
		puti(r);
		putch('\n');
		return 1;
	}
	pattern(a.map, a.pitch, w, h, 0);
	sc.crtc_id = crtcs[0];
	sc.fb_id = a.fb;
	sc.set_connectors_ptr = (u64)&conn;
	sc.count_connectors = 1;
	sc.mode = modes[0];
	sc.mode_valid = 1;
	r = kioc(DRM_IOCTL_MODE_SETCRTC, &sc);
	result("modeset", r ? "FAIL SETCRTC: " : "PASS ");
	if (r) {
		puti(r);
		putch('\n');
		return 1;
	}
	puts_("dumb XR24 ");
	putu(w);
	putch('x');
	putu(h);
	puts_(" pitch ");
	putu(a.pitch);
	puts_(": colour bars in a white border\n");

	/* Flips: A and B (bars inverted); in bypass B may not fit in CMA: flip A to A */
	if (dumb_fb(&b, w, h)) {
		puts_("second dumb buffer refused; flipping the first to itself\n");
		b = a;
	} else {
		pattern(b.map, b.pitch, w, h, 1);
	}
	prev = t0 = flip(crtcs[0], b.fb);
	for (i = 1; i <= KMS_FLIPS && t0 >= 0; i++) {
		t = flip(crtcs[0], i & 1 ? a.fb : b.fb);
		if (t < 0) {
			t0 = t;
			break;
		}
		if (t - prev > worst)
			worst = t - prev;
		/* the driver completes a flip after 100 ms without an interrupt */
		slow += t - prev > 90000;
		prev = t;
	}
	if (t0 < 0) {
		result("flip", "FAIL no flip event: ");
		puti(t0);
		putch('\n');
		return 1;
	}
	t = (prev - t0) / KMS_FLIPS;
	result("flip", !slow && t < 40000 ? "PASS " : "FAIL ");
	putu(KMS_FLIPS);
	puts_(" flips, ");
	putu(t / 1000);
	putch('.');
	putu(t / 100 % 10);
	puts_(" ms each, slowest ");
	putu(worst / 1000);
	puts_(" ms");
	puts_(slow ? " (flips completed by the 100 ms timeout: no vblank interrupt)\n" : "\n");
	fails += !(!slow && t < 40000);

	/* A powervr buffer, filled by the CPU, imported and scanned out */
	if (skip && streq(skip, "nogpu")) {
		result("import", "SKIPPED (powervr is not loaded: run a8-gpu-drm-test probe first)\n");
	} else if (skip) {
		result("import", "SKIPPED (the display DART is in bypass: a scattered buffer cannot be scanned out)\n");
	} else {
		struct drm_pvr_ioctl_create_bo_args c = {
			.size = (u64)a.pitch * h,
			.flags = DRM_PVR_BO_BYPASS_DEVICE_CACHE | DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS,
		};
		struct drm_pvr_ioctl_get_bo_mmap_offset_args o = { 0 };
		struct drm_prime_handle ex = { .flags = DRM_CLOEXEC | DRM_RDWR, .fd = -1 };
		struct drm_prime_handle im = { .fd = -1 };
		struct drm_mode_fb_cmd2 fb = { .width = w, .height = h, .pixel_format = FMT_XRGB8888 };
		const char *why = 0;
		int seen;

		c.size = (c.size + 0x3fff) & ~0x3fffull;
		fd = open_node();
		if (fd < 0)
			why = "no powervr render node";
		else if (sys3(SYS_ioctl, fd, DRM_IOCTL_PVR_CREATE_BO, &c))
			why = "powervr CREATE_BO";
		else if ((o.handle = c.handle, sys3(SYS_ioctl, fd, DRM_IOCTL_PVR_GET_BO_MMAP_OFFSET, &o)))
			why = "powervr GET_BO_MMAP_OFFSET";
		else if (is_err((long)(imp.map = sys_mmap(0, c.size, PROT_READ | PROT_WRITE,
							   MAP_SHARED, fd, o.offset))))
			why = "powervr mmap";
		if (!why) {
			/* checkerboard of 64-pixel squares, so it differs from both bar patterns */
			u32 x, y;

			for (y = 0; y < h; y++)
				for (x = 0; x < w; x++)
					imp.map[y * (a.pitch / 4) + x] =
						((x / 64) ^ (y / 64)) & 1 ? 0xff8000 : 0x0040ff;
			ex.handle = c.handle;
			if (sys3(SYS_ioctl, fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ex))
				why = "PRIME export from powervr";
			else if ((im.fd = ex.fd, kioc(DRM_IOCTL_PRIME_FD_TO_HANDLE, &im)))
				why = "PRIME import into adbe (a scattered buffer needs the display DART)";
		}
		if (!why) {
			fb.handles[0] = im.handle;
			fb.pitches[0] = a.pitch;
			if (kioc(DRM_IOCTL_MODE_ADDFB2, &fb))
				why = "ADDFB2 of the imported buffer";
			else if (flip(crtcs[0], fb.fb_id) < 0)
				why = "page flip to the imported buffer";
		}
		seen = why ? 0 : fb_scanned_imported(fb.fb_id);
		if (why) {
			result("import", "FAIL ");
			puts_(why);
			putch('\n');
			fails++;
		} else if (seen == 1) {
			result("import", "PASS powervr buffer on the primary plane, imported=yes in debugfs: "
			       "orange and blue checkerboard\n");
		} else {
			result("import", seen ? "PASS flipped (debugfs unreadable, not confirmed)\n" :
						"FAIL the imported framebuffer is not on the plane\n");
			fails += !seen;
		}
		if (!why) {
			struct ts d = { 2, 0 };

			flush();
			sys6(SYS_ppoll, 0, 0, (long)&d, 0, 0, 0);	/* 2 s for the eye */
			flip(crtcs[0], a.fb);
		}
	}
	flush();
	return fails ? 1 : 0;
}

static int main_(int argc, char **argv)
{
	int err;

	if (argc >= 5 && streq(argv[1], "kms")) {
		u32 wd = 0, ht = 0;
		const char *p;

		for (p = argv[2]; *p >= '0' && *p <= '9'; p++)
			wd = wd * 10 + (u32)(*p - '0');
		for (p = argv[3]; *p >= '0' && *p <= '9'; p++)
			ht = ht * 10 + (u32)(*p - '0');
		return kms(wd, ht, argv[4], argc >= 6 ? argv[5] : 0);
	}

	if (argc == 4 && streq(argv[1], "dry")) {
		dry = 1;
		argv++;
		argc--;
	}
	if (argc < 2 || (!streq(argv[1], "info") && argc < 3)) {
		puts_("usage: a8-gpu-drm compute|triangle FILE.rpl | info | kms W H eDP|DSI [nogpu|bypass] | dry compute|triangle FILE.rpl\n");
		return 2;
	}
	fd = dry ? -2 : open_node();
	if (!dry && fd < 0) {
		puts_("RESULT ");
		puts_(argv[1]);
		puts_(" FAIL no powervr render node\n");
		return 2;
	}
	if (streq(argv[1], "info"))
		return info();
	err = load(argv[2]);
	if (err) {
		puts_("RESULT ");
		puts_(argv[1]);
		puts_(" FAIL cannot read the recording: ");
		puti(err);
		putch('\n');
		return 2;
	}
	err = replay(streq(argv[1], "triangle"));
	if (err == 2) {
		puts_("RESULT ");
		puts_(argv[1]);
		puts_(" FAIL the recording does not replay (see above)\n");
	}
	return err;
}

__attribute__((used)) void start_c(long *sp)
{
	int argc = (int)sp[0];
	char **argv = (char **)(sp + 1);
	int r = main_(argc, argv);

	flush();
	sys_exit(r);
}

__asm__(".text\n"
	".global _start\n"
	"_start:\n"
	"	mov x0, sp\n"
	"	and sp, x0, #~15\n"
	"	bl start_c\n");
