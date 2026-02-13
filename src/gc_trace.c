/*
 * GC Trace — built-in heaptrack-compatible allocation tracer for HashLink.
 *
 * Activated via HL_HEAPTRACK=<filename> environment variable.
 * Writes a heaptrack v1 text trace file viewable in heaptrack_gui.
 *
 * Frame pointer walking + HashLink JIT symbol resolution + dladdr for native.
 */
#include "hl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#if !defined(HL_WIN) && !defined(HL_CONSOLE)
#include <dlfcn.h>
#include <unistd.h>
#endif

int gc_trace_active = 0;

/* ---- Trace file state ---- */
static FILE *trace_file = NULL;
static struct timespec trace_start_time;
static int trace_alloc_count = 0;

/* ---- Output buffer ---- */
#define TRACE_BUF_SIZE (1 << 16)
static char *trace_buf = NULL;
static int trace_buf_pos = 0;

static void trace_flush(void) {
	if (trace_buf_pos > 0 && trace_file) {
		fwrite(trace_buf, 1, trace_buf_pos, trace_file);
		trace_buf_pos = 0;
	}
}

static void trace_write(const char *data, int len) {
	if (trace_buf_pos + len > TRACE_BUF_SIZE)
		trace_flush();
	if (len > TRACE_BUF_SIZE) {
		fwrite(data, 1, len, trace_file);
		return;
	}
	memcpy(trace_buf + trace_buf_pos, data, len);
	trace_buf_pos += len;
}

static void trace_printf(const char *fmt, ...) {
	char tmp[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n > 0)
		trace_write(tmp, n);
}

/* ---- Hash functions ---- */

static unsigned int hash_str(const char *s) {
	unsigned int h = 5381;
	while (*s)
		h = h * 33 + (unsigned char)*s++;
	return h;
}

static unsigned int hash_ptr(void *p) {
	uintptr_t v = (uintptr_t)p;
	v ^= v >> 16;
	v *= 0x45d9f3b;
	v ^= v >> 16;
	return (unsigned int)v;
}

static unsigned int hash_trace(uintptr_t ip_hex, int parent) {
	uintptr_t v = ip_hex ^ ((uintptr_t)parent * 2654435761u);
	v ^= v >> 16;
	v *= 0x45d9f3b;
	v ^= v >> 16;
	return (unsigned int)v;
}

static unsigned int hash_alloc_info(uint64_t size, int trace_idx) {
	uintptr_t v = (uintptr_t)size ^ ((uintptr_t)trace_idx * 2654435761u);
	v ^= v >> 16;
	v *= 0x9e3779b9;
	v ^= v >> 16;
	return (unsigned int)v;
}

/* ==== String intern table: char* content -> 1-based index ==== */

typedef struct {
	char *key;
	int index;
} str_entry;

static str_entry *str_table = NULL;
static int str_table_cap = 0;
static int str_table_count = 0;
static int str_next_index = 1;

static void str_table_grow(void) {
	int new_cap = str_table_cap ? str_table_cap * 2 : 1024;
	str_entry *new_table = (str_entry *)calloc(new_cap, sizeof(str_entry));
	for (int i = 0; i < str_table_cap; i++) {
		if (str_table[i].key) {
			unsigned int h = hash_str(str_table[i].key) & (new_cap - 1);
			while (new_table[h].key)
				h = (h + 1) & (new_cap - 1);
			new_table[h] = str_table[i];
		}
	}
	free(str_table);
	str_table = new_table;
	str_table_cap = new_cap;
}

static int intern_string(const char *s) {
	if (!s || !*s) s = "??";
	if (str_table_count * 10 >= str_table_cap * 7)
		str_table_grow();
	unsigned int h = hash_str(s) & (str_table_cap - 1);
	while (str_table[h].key) {
		if (strcmp(str_table[h].key, s) == 0)
			return str_table[h].index;
		h = (h + 1) & (str_table_cap - 1);
	}
	int idx = str_next_index++;
	char *copy = strdup(s);
	str_table[h].key = copy;
	str_table[h].index = idx;
	str_table_count++;
	trace_printf("s %s\n", copy);
	return idx;
}

/* ==== IP intern table: void* -> 1-based index ==== */

typedef struct {
	void *key;
	int index;
} ip_entry;

static ip_entry *ip_table = NULL;
static int ip_table_cap = 0;
static int ip_table_count = 0;
static int ip_next_index = 1;

static void ip_table_grow(void) {
	int new_cap = ip_table_cap ? ip_table_cap * 2 : 4096;
	ip_entry *new_table = (ip_entry *)calloc(new_cap, sizeof(ip_entry));
	for (int i = 0; i < ip_table_cap; i++) {
		if (ip_table[i].key) {
			unsigned int h = hash_ptr(ip_table[i].key) & (new_cap - 1);
			while (new_table[h].key)
				h = (h + 1) & (new_cap - 1);
			new_table[h] = ip_table[i];
		}
	}
	free(ip_table);
	ip_table = new_table;
	ip_table_cap = new_cap;
}

static int intern_ip(void *addr) {
	if (ip_table_count * 10 >= ip_table_cap * 7)
		ip_table_grow();
	unsigned int h = hash_ptr(addr) & (ip_table_cap - 1);
	while (ip_table[h].key) {
		if (ip_table[h].key == addr)
			return ip_table[h].index;
		h = (h + 1) & (ip_table_cap - 1);
	}

	int idx = ip_next_index++;
	ip_table[h].key = addr;
	ip_table[h].index = idx;
	ip_table_count++;

	int fn_idx = 0, file_idx = 0, line = 0;
	int mod_idx = 0;

	/* Try JIT resolution via hl_setup.resolve_symbol (set by module.c on JIT init) */
	uchar sym_buf[256];
	int sym_size = 256;
	uchar *result = hl_setup.resolve_symbol ? hl_setup.resolve_symbol(addr, sym_buf, &sym_size) : NULL;
	if (result) {
		char utf8_buf[512];
		int len = 0;
		for (int i = 0; i < sym_size && len < (int)sizeof(utf8_buf) - 1; i++) {
			uchar c = sym_buf[i];
			if (c < 0x80) {
				utf8_buf[len++] = (char)c;
			} else if (c < 0x800) {
				utf8_buf[len++] = (char)(0xC0 | (c >> 6));
				utf8_buf[len++] = (char)(0x80 | (c & 0x3F));
			} else {
				utf8_buf[len++] = (char)(0xE0 | (c >> 12));
				utf8_buf[len++] = (char)(0x80 | ((c >> 6) & 0x3F));
				utf8_buf[len++] = (char)(0x80 | (c & 0x3F));
			}
		}
		utf8_buf[len] = 0;

		char fn_name[384];
		char file_name[256];
		int parsed_line = 0;
		char *paren = strchr(utf8_buf, '(');
		if (paren) {
			int fn_len = (int)(paren - utf8_buf);
			if (fn_len >= (int)sizeof(fn_name)) fn_len = (int)sizeof(fn_name) - 1;
			memcpy(fn_name, utf8_buf, fn_len);
			fn_name[fn_len] = 0;

			char *colon = strrchr(paren + 1, ':');
			if (colon) {
				int file_len = (int)(colon - (paren + 1));
				if (file_len >= (int)sizeof(file_name)) file_len = (int)sizeof(file_name) - 1;
				memcpy(file_name, paren + 1, file_len);
				file_name[file_len] = 0;
				parsed_line = atoi(colon + 1);
			} else {
				strncpy(file_name, paren + 1, sizeof(file_name) - 1);
				file_name[sizeof(file_name) - 1] = 0;
				int fl = (int)strlen(file_name);
				if (fl > 0 && file_name[fl - 1] == ')') file_name[fl - 1] = 0;
			}
		} else {
			strncpy(fn_name, utf8_buf, sizeof(fn_name) - 1);
			fn_name[sizeof(fn_name) - 1] = 0;
			strcpy(file_name, "??");
		}

		mod_idx = intern_string("jit");
		fn_idx = intern_string(fn_name);
		file_idx = intern_string(file_name);
		line = parsed_line;
	} else {
#if !defined(HL_WIN) && !defined(HL_CONSOLE)
		Dl_info info;
		if (dladdr(addr, &info)) {
			mod_idx = intern_string(info.dli_fname ? info.dli_fname : "??");
			fn_idx = intern_string(info.dli_sname ? info.dli_sname : "??");
			file_idx = intern_string("??");
			line = 0;
		} else
#endif
		{
			mod_idx = intern_string("??");
			fn_idx = intern_string("??");
			file_idx = intern_string("??");
			line = 0;
		}
	}

	trace_printf("i %lx %x %x %x %x\n",
		(unsigned long)(uintptr_t)addr, mod_idx, fn_idx, file_idx, line);
	return idx;
}

/* ==== Trace tree intern table: (ip_hex, parent) -> 1-based index ==== */

typedef struct {
	uintptr_t ip_hex;
	int parent;
	int index;
} trace_entry;

static trace_entry *trace_table = NULL;
static int trace_table_cap = 0;
static int trace_table_count = 0;
static int trace_next_index = 1;

static void trace_table_grow(void) {
	int new_cap = trace_table_cap ? trace_table_cap * 2 : 4096;
	trace_entry *new_table = (trace_entry *)calloc(new_cap, sizeof(trace_entry));
	for (int i = 0; i < trace_table_cap; i++) {
		if (trace_table[i].index) {
			unsigned int h = hash_trace(trace_table[i].ip_hex, trace_table[i].parent) & (new_cap - 1);
			while (new_table[h].index)
				h = (h + 1) & (new_cap - 1);
			new_table[h] = trace_table[i];
		}
	}
	free(trace_table);
	trace_table = new_table;
	trace_table_cap = new_cap;
}

static int intern_trace(int ip_idx, uintptr_t ip_hex, int parent_trace) {
	if (trace_table_count * 10 >= trace_table_cap * 7)
		trace_table_grow();
	unsigned int h = hash_trace(ip_hex, parent_trace) & (trace_table_cap - 1);
	while (trace_table[h].index) {
		if (trace_table[h].ip_hex == ip_hex && trace_table[h].parent == parent_trace)
			return trace_table[h].index;
		h = (h + 1) & (trace_table_cap - 1);
	}

	int idx = trace_next_index++;
	trace_table[h].ip_hex = ip_hex;
	trace_table[h].parent = parent_trace;
	trace_table[h].index = idx;
	trace_table_count++;

	trace_printf("t %x %x\n", ip_idx, parent_trace);
	return idx;
}

/* ==== Alloc info intern table: (size, trace_idx) -> 0-based index ==== */
/* Emits "a <size> <trace_idx>" records for heaptrack format v1+ */

typedef struct {
	uint64_t size;
	int trace_idx;
	int index;    /* 0-based, -1 = empty */
} ainfo_entry;

static ainfo_entry *ainfo_table = NULL;
static int ainfo_table_cap = 0;
static int ainfo_table_count = 0;
static int ainfo_next_index = 0;

static void ainfo_table_grow(void) {
	int new_cap = ainfo_table_cap ? ainfo_table_cap * 2 : 4096;
	ainfo_entry *new_table = (ainfo_entry *)malloc(new_cap * sizeof(ainfo_entry));
	for (int i = 0; i < new_cap; i++)
		new_table[i].index = -1;
	for (int i = 0; i < ainfo_table_cap; i++) {
		if (ainfo_table[i].index >= 0) {
			unsigned int h = hash_alloc_info(ainfo_table[i].size, ainfo_table[i].trace_idx) & (new_cap - 1);
			while (new_table[h].index >= 0)
				h = (h + 1) & (new_cap - 1);
			new_table[h] = ainfo_table[i];
		}
	}
	free(ainfo_table);
	ainfo_table = new_table;
	ainfo_table_cap = new_cap;
}

/* Returns 0-based alloc info index. Emits "a" record on first insertion. */
static int intern_alloc_info(uint64_t size, int trace_idx) {
	if (ainfo_table_count * 10 >= ainfo_table_cap * 7)
		ainfo_table_grow();
	unsigned int h = hash_alloc_info(size, trace_idx) & (ainfo_table_cap - 1);
	while (ainfo_table[h].index >= 0) {
		if (ainfo_table[h].size == size && ainfo_table[h].trace_idx == trace_idx)
			return ainfo_table[h].index;
		h = (h + 1) & (ainfo_table_cap - 1);
	}
	int idx = ainfo_next_index++;
	ainfo_table[h].size = size;
	ainfo_table[h].trace_idx = trace_idx;
	ainfo_table[h].index = idx;
	ainfo_table_count++;
	trace_printf("a %lx %x\n", (unsigned long)size, trace_idx);
	return idx;
}

/* ==== Pointer map: void* -> alloc_info_index (for frees) ==== */

typedef struct {
	void *key;         /* NULL = empty */
	int alloc_info_idx;
} ptr_entry;

static ptr_entry *ptr_table = NULL;
static int ptr_table_cap = 0;
static int ptr_table_count = 0;

static void ptr_table_grow(void) {
	int new_cap = ptr_table_cap ? ptr_table_cap * 2 : (1 << 16);
	ptr_entry *new_table = (ptr_entry *)calloc(new_cap, sizeof(ptr_entry));
	for (int i = 0; i < ptr_table_cap; i++) {
		if (ptr_table[i].key) {
			unsigned int h = hash_ptr(ptr_table[i].key) & (new_cap - 1);
			while (new_table[h].key)
				h = (h + 1) & (new_cap - 1);
			new_table[h] = ptr_table[i];
		}
	}
	free(ptr_table);
	ptr_table = new_table;
	ptr_table_cap = new_cap;
}

static void ptr_map_insert(void *ptr, int alloc_info_idx) {
	if (ptr_table_count * 10 >= ptr_table_cap * 7)
		ptr_table_grow();
	unsigned int h = hash_ptr(ptr) & (ptr_table_cap - 1);
	while (ptr_table[h].key) {
		if (ptr_table[h].key == ptr) {
			/* Overwrite (reallocation of same address) */
			ptr_table[h].alloc_info_idx = alloc_info_idx;
			return;
		}
		h = (h + 1) & (ptr_table_cap - 1);
	}
	ptr_table[h].key = ptr;
	ptr_table[h].alloc_info_idx = alloc_info_idx;
	ptr_table_count++;
}

/* Returns alloc_info_idx, or -1 if not found. Removes the entry. */
static int ptr_map_remove(void *ptr) {
	if (!ptr_table_cap) return -1;
	unsigned int h = hash_ptr(ptr) & (ptr_table_cap - 1);
	while (ptr_table[h].key) {
		if (ptr_table[h].key == ptr) {
			int idx = ptr_table[h].alloc_info_idx;
			/* Tombstone: mark empty and rehash following cluster */
			ptr_table[h].key = NULL;
			ptr_table_count--;
			/* Rehash entries that might have been displaced past this slot */
			unsigned int j = (h + 1) & (ptr_table_cap - 1);
			while (ptr_table[j].key) {
				void *k = ptr_table[j].key;
				int v = ptr_table[j].alloc_info_idx;
				ptr_table[j].key = NULL;
				ptr_table_count--;
				ptr_map_insert(k, v);
				j = (j + 1) & (ptr_table_cap - 1);
			}
			return idx;
		}
		h = (h + 1) & (ptr_table_cap - 1);
	}
	return -1;
}

/* ---- Frame pointer walking ---- */

#define GC_TRACE_MAX_DEPTH 64

static int gc_trace_capture(void **buf, int max) {
	int depth = 0;
	void **fp = (void **)__builtin_frame_address(0);
	hl_thread_info *t = hl_get_thread();
	void *stack_top = t ? t->stack_top : NULL;

	while (fp && depth < max) {
		void *lr = fp[1];
		if (!lr) break;
		buf[depth++] = lr;
		void **next = (void **)fp[0];
		if (next <= fp) break;
		if (stack_top && (void *)next >= stack_top) break;
		if ((uintptr_t)next & 0xF) break;
		fp = next;
	}
	return depth;
}

/* ---- Timestamp ---- */

static long trace_elapsed_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - trace_start_time.tv_sec) * 1000L +
		(now.tv_nsec - trace_start_time.tv_nsec) / 1000000L;
}

/* ---- Public API ---- */

void gc_trace_init(void) {
#ifndef HL_CONSOLE
	const char *filename = getenv("HL_HEAPTRACK");
	if (!filename || !*filename)
		return;

	trace_file = fopen(filename, "w");
	if (!trace_file) {
		fprintf(stderr, "[gc_trace] Failed to open %s for writing\n", filename);
		return;
	}

	trace_buf = (char *)malloc(TRACE_BUF_SIZE);
	trace_buf_pos = 0;

	clock_gettime(CLOCK_MONOTONIC, &trace_start_time);

	/* Header: heaptrack version 1.4.0, file format version 1 */
	trace_printf("v 10400 1\n");

#if !defined(HL_WIN)
	char exe[1024];
	ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (len > 0) {
		exe[len] = 0;
		trace_printf("x %lx %s\n", (unsigned long)len, exe);
	} else {
		trace_printf("x 2 hl\n");
	}
#else
	trace_printf("x 2 hl\n");
#endif

	trace_printf("c 0\n");

	gc_trace_active = 1;
	fprintf(stderr, "[gc_trace] Tracing to %s\n", filename);
#endif
}

void gc_trace_close(void) {
	if (!trace_file) return;
	gc_trace_active = 0;

	trace_printf("c %lx\n", (unsigned long)trace_elapsed_ms());
	trace_flush();
	fclose(trace_file);
	trace_file = NULL;

	/* Free all intern tables */
	if (str_table) {
		for (int i = 0; i < str_table_cap; i++)
			free(str_table[i].key);
		free(str_table);
		str_table = NULL;
	}
	str_table_cap = str_table_count = 0;
	str_next_index = 1;

	free(ip_table);
	ip_table = NULL;
	ip_table_cap = ip_table_count = 0;
	ip_next_index = 1;

	free(trace_table);
	trace_table = NULL;
	trace_table_cap = trace_table_count = 0;
	trace_next_index = 1;

	free(ainfo_table);
	ainfo_table = NULL;
	ainfo_table_cap = ainfo_table_count = 0;
	ainfo_next_index = 0;

	free(ptr_table);
	ptr_table = NULL;
	ptr_table_cap = ptr_table_count = 0;

	free(trace_buf);
	trace_buf = NULL;
	trace_buf_pos = 0;
}

void gc_trace_alloc(void *ptr, int size) {
	if (!trace_file) return;

	void *frames[GC_TRACE_MAX_DEPTH];
	int nframes = gc_trace_capture(frames, GC_TRACE_MAX_DEPTH);

	/* Build trace tree: outermost to innermost */
	int parent_trace = 0;
	for (int i = nframes - 1; i >= 0; i--) {
		int ip_idx = intern_ip(frames[i]);
		parent_trace = intern_trace(ip_idx, (uintptr_t)frames[i], parent_trace);
	}

	/* Intern (size, trace) -> alloc info index, emits "a" record if new */
	int ainfo_idx = intern_alloc_info((uint64_t)size, parent_trace);

	/* Remember ptr -> alloc_info_idx for later free */
	ptr_map_insert(ptr, ainfo_idx);

	/* Periodic timestamp */
	if ((++trace_alloc_count & 0x3FF) == 0)
		trace_printf("c %lx\n", (unsigned long)trace_elapsed_ms());

	/* v1 format: + <alloc_info_idx> */
	trace_printf("+ %x\n", ainfo_idx);
}

void gc_trace_free(void *ptr) {
	if (!trace_file) return;

	int ainfo_idx = ptr_map_remove(ptr);
	if (ainfo_idx < 0) return;  /* Unknown pointer — skip */

	/* v1 format: - <alloc_info_idx> */
	trace_printf("- %x\n", ainfo_idx);
}
