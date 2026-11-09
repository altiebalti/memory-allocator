/* Instrumentation wrappers for verifying the allocator against the reference
 * ltrace traces, without needing ltrace itself.
 *
 * Linked with -Wl,--wrap=os_malloc,os_calloc,os_realloc,os_free,sbrk,mmap,munmap
 * Logging uses write(2) + snprintf only (no stdio/malloc) so it does not perturb
 * the program break or issue stray syscalls.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

extern void *__real_sbrk(intptr_t increment);
extern void *__real_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
extern int   __real_munmap(void *addr, size_t length);
extern void *__real_os_malloc(size_t size);
extern void *__real_os_calloc(size_t nmemb, size_t size);
extern void *__real_os_realloc(void *ptr, size_t size);
extern void  __real_os_free(void *ptr);

static char *g_base;
static int   g_base_set;

struct region { char *base; size_t len; int id; int active; };
static struct region g_regions[256];
static int g_region_cnt;
static int g_map_id;

static void emit(const char *s)
{
	ssize_t n = (ssize_t)strlen(s);
	ssize_t off = 0;

	while (off < n) {
		ssize_t w = write(2, s + off, (size_t)(n - off));

		if (w <= 0)
			break;
		off += w;
	}
}

/* Format a pointer as "0", "H+0x..", or "M<id>+0x..". */
static void tok(void *p, char *out, size_t outsz)
{
	if (p == NULL) {
		snprintf(out, outsz, "0");
		return;
	}
	for (int i = 0; i < g_region_cnt; i++) {
		char *b = g_regions[i].base;

		if (g_regions[i].active && (char *)p >= b &&
		    (char *)p < b + g_regions[i].len) {
			snprintf(out, outsz, "M%d+0x%lx", g_regions[i].id,
				 (unsigned long)((char *)p - b));
			return;
		}
	}
	snprintf(out, outsz, "H+0x%lx", (unsigned long)((char *)p - g_base));
}

void *__wrap_sbrk(intptr_t increment)
{
	char line[128];
	void *prev = __real_sbrk(increment);

	if (!g_base_set && prev != (void *)-1) {
		g_base = prev;
		g_base_set = 1;
	}

	if (increment == 0) {
		/* brk(0): arg 0, returns current break. */
		snprintf(line, sizeof(line), "S|brk|0|H+0x%lx\n",
			 (unsigned long)((char *)prev - g_base));
	} else {
		/* sbrk(inc) -> brk(prev+inc): ltrace shows the target address. */
		char *target = (char *)prev + increment;

		snprintf(line, sizeof(line), "S|brk|H+0x%lx|H+0x%lx\n",
			 (unsigned long)(target - g_base),
			 (unsigned long)(target - g_base));
	}
	emit(line);
	return prev;
}

void *__wrap_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	char line[128];
	void *r = __real_mmap(addr, length, prot, flags, fd, offset);

	if (r != MAP_FAILED) {
		int id = ++g_map_id;

		if (g_region_cnt < (int)(sizeof(g_regions) / sizeof(g_regions[0]))) {
			g_regions[g_region_cnt].base = (char *)r;
			g_regions[g_region_cnt].len = length;
			g_regions[g_region_cnt].id = id;
			g_regions[g_region_cnt].active = 1;
			g_region_cnt++;
		}
		snprintf(line, sizeof(line), "S|mmap|%zu|M%d+0x0\n", length, id);
		emit(line);
	}
	return r;
}

int __wrap_munmap(void *addr, size_t length)
{
	char line[128];
	char tb[64];

	tok(addr, tb, sizeof(tb));
	snprintf(line, sizeof(line), "S|munmap|%s|%zu\n", tb, length);
	emit(line);

	for (int i = 0; i < g_region_cnt; i++) {
		if (g_regions[i].active && g_regions[i].base == (char *)addr)
			g_regions[i].active = 0;
	}
	return __real_munmap(addr, length);
}

void *__wrap_os_malloc(size_t size)
{
	char line[128], tb[64];
	void *r;

	snprintf(line, sizeof(line), "P|os_malloc|%zu\n", size);
	emit(line);
	r = __real_os_malloc(size);
	tok(r, tb, sizeof(tb));
	snprintf(line, sizeof(line), "R|%s\n", tb);
	emit(line);
	return r;
}

void *__wrap_os_calloc(size_t nmemb, size_t size)
{
	char line[128], tb[64];
	void *r;

	snprintf(line, sizeof(line), "P|os_calloc|%zu|%zu\n", nmemb, size);
	emit(line);
	r = __real_os_calloc(nmemb, size);
	tok(r, tb, sizeof(tb));
	snprintf(line, sizeof(line), "R|%s\n", tb);
	emit(line);
	return r;
}

void *__wrap_os_realloc(void *ptr, size_t size)
{
	char line[128], tb[64], rb[64];
	void *r;

	tok(ptr, tb, sizeof(tb));
	snprintf(line, sizeof(line), "P|os_realloc|%s|%zu\n", tb, size);
	emit(line);
	r = __real_os_realloc(ptr, size);
	tok(r, rb, sizeof(rb));
	snprintf(line, sizeof(line), "R|%s\n", rb);
	emit(line);
	return r;
}

void __wrap_os_free(void *ptr)
{
	char line[128], tb[64];

	tok(ptr, tb, sizeof(tb));
	snprintf(line, sizeof(line), "P|os_free|%s\n", tb);
	emit(line);
	__real_os_free(ptr);
	emit("R|void\n");
}
