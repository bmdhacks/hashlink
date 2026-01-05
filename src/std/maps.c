/*
 * Copyright (C)2005-2016 Haxe Foundation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include <hl.h>
#ifdef HL_VCC
#	pragma warning(disable:4034) // sizeof(void) == 0
#endif

#define H_SIZE_INIT 3

// successive primes that double every time
static int H_PRIMES[] = {
	7,17,37,79,163,331,673,1361,2729,5471,10949,21911,43853,87613,175229,350459,700919,1401857,2803727,5607457,11214943,22429903,44859823,89719661,179424673,373587883,776531401,1611623773
};

// ----- FREE LIST ---------------------------------

typedef struct {
	int pos;
	int count;
} hl_free_bucket;

typedef struct {
	hl_free_bucket *buckets;
	int head;
	int nbuckets;
} hl_free_list;

static void hl_freelist_resize( hl_free_list *f, int newsize ) {
	hl_free_bucket *buckets = (hl_free_bucket*)hl_gc_alloc_noptr(sizeof(hl_free_bucket)*newsize);
	memcpy(buckets,f->buckets,f->head * sizeof(hl_free_bucket));
	f->buckets = buckets;
	f->nbuckets = newsize;
}

static void hl_freelist_init( hl_free_list *f ) {
	memset(f,0,sizeof(hl_free_list));
}

static void hl_freelist_add_range( hl_free_list *f, int pos, int count ) {
	hl_free_bucket *b = f->buckets;
	hl_free_bucket *prev = NULL;
	if( !b ) {
		// special handling for countinuous space
		if( f->nbuckets == 0 ) {
			f->head = pos;
			f->nbuckets = count;
			return;
		} else if( f->head + f->nbuckets == pos ) {
			f->nbuckets += count;
			return;
		} else if( pos + count == f->head ) {
			f->head -= count;
			f->nbuckets += count;
			return;
		} else {
			int cur_pos = f->head, cur_count = f->nbuckets;
			f->head = 0;
			f->nbuckets = 0;
			hl_freelist_resize(f,2);
			if( cur_count ) hl_freelist_add_range(f,cur_pos,cur_count);
			b = f->buckets;
		}
	}
	while( b < f->buckets + f->head ) {
		if( b->pos > pos ) break;
		prev = b;
		b++;
	}
	if( b < f->buckets + f->head && b->pos == pos + count ) {
		b->pos -= count;
		b->count += count;
		// merge
		if( prev && prev->pos + prev->count == b->pos ) {
			prev->count += b->count;
			memmove(b,b+1,((f->buckets + f->head) - (b+1)) * sizeof(hl_free_bucket));
			f->head--;
		}
		return;
	}
	if( prev && prev->pos + prev->count == pos ) {
		prev->count += count;
		return;
	}
	// insert
	if( f->head == f->nbuckets ) {
		int pos = (int)(b - f->buckets);
		hl_freelist_resize(f,((f->nbuckets * 3) + 1) >> 1);
		b = f->buckets + pos;
	}
	memmove(b+1,b,((f->buckets + f->head) - b) * sizeof(hl_free_bucket));
	b->pos = pos;
	b->count = count;
	f->head++;
}

static void hl_freelist_add( hl_free_list *f, int pos ) {
	hl_freelist_add_range(f,pos,1);
}

static int hl_freelist_get( hl_free_list *f ) {
	hl_free_bucket *b;
	int p;
	if( !f->buckets ) {
		if( f->nbuckets == 0 ) return -1;
		f->nbuckets--;
		return f->head++;
	}
	if( f->head == 0 )
		return -1;
	b = f->buckets + f->head - 1;
	b->count--;
	p = b->pos + b->count;
	if( b->count == 0 ) {
		f->head--;
		if( f->head < (f->nbuckets>>1) )
			hl_freelist_resize(f,f->nbuckets>>1);
	}
	return p;
}

#define _MVAL_TYPE vdynamic*

// ----- INT MAP ---------------------------------

typedef struct {
	int key;
} hl_hi_entry;

typedef struct {
	vdynamic *value;
} hl_hi_value;

#define hlt_key		hlt_i32
#define hl_hifilter(key) key
#define hl_hihash(h)	((unsigned)(h))
#define _MKEY_TYPE	int
#define _MNAME(n)	hl_hi##n
#define _MMATCH(c)	m->entries[c].key == key
#define _MKEY(m,c)	m->entries[c].key
#define	_MSET(c)	m->entries[c].key = key
#define _MERASE(c)

#include "maps.h"


// ----- INT64 MAP ---------------------------------

typedef struct {
	int64 key;
} hl_hi64_entry;

typedef struct {
	vdynamic *value;
} hl_hi64_value;

#define hlt_key		hlt_i64
#define hl_hi64filter(key) key
#define hl_hi64hash(h)	(((unsigned int)h) ^ ((unsigned int)(h>>32)))
#define _MKEY_TYPE	int64
#define _MNAME(n)	hl_hi64##n
#define _MMATCH(c)	m->entries[c].key == key
#define _MKEY(m,c)	m->entries[c].key
#define	_MSET(c)	m->entries[c].key = key
#define _MERASE(c)

#include "maps.h"

// ----- BYTES MAP ---------------------------------

typedef struct {
	unsigned int hash;
} hl_hb_entry;

typedef struct {
	uchar *key;
	vdynamic *value;
} hl_hb_value;

#define hlt_key		hlt_bytes
#define hl_hbfilter(key) key
#define hl_hbhash(key)	((unsigned)hl_hash_gen(key,false))
#define _MKEY_TYPE	uchar*
#define _MNAME(n)	hl_hb##n
#define _MMATCH(c)	m->entries[c].hash == hash && ucmp(m->values[c].key,key) == 0
#define _MKEY(m,c)	m->values[c].key
#define	_MSET(c)	m->entries[c].hash = hash; m->values[c].key = key
#define _MERASE(c)  m->values[c].key = NULL
#define _MNO_EXPORTS

#include "maps.h"

#undef _MNO_EXPORTS

// ----- BYTES MAP PRESIZING ---------------------------------

// Lookup table for pre-sizing bytes maps based on first key prefix
// Add entries here when warnings appear about maps growing past 1000 entries
static struct { const char *prefix; int target_size; } hl_hb_presets[] = {
	// Atlas animation maps (reach 8000+ entries)
	{"activationLevier_", 10949},
	{"drink_", 10949},
	// Animation/asset maps (reach 4000-5000 entries)
	{"anims", 5471},
	// Localization strings (reach 4000+ entries)
	{"Abandonner", 5471},
	// Lab/level data (reach 2000+ entries)
	{"LabSeb", 2729},
	// FX animation maps (reach 2000+ entries)
	{"comboKickA/", 2729},
	{"fxSpikeBootsA/", 2729},
	{"fxGolemPunch/", 1361},
	{"basherAtkFx/", 1361},
	// UI/texture maps (reach 1000+ entries)
	{"ui/", 1361},
	{"64x64/", 1361},
	{"DLCPurple/", 1361},
	{"achemyPentagram", 1361},
	{"affectBerserker", 1361},
	{"fxDiamondRed", 710},
	{NULL, 0}
};

static void hl_hb_presize_direct(hl_hb_map *m, int target_entries) {
	// Directly allocate map to target size (single allocation)
	int i = 0;
	int ncells = target_entries >> 2;
	while(H_PRIMES[i] < ncells) i++;
	ncells = H_PRIMES[i];

	int ksize = target_entries < _MLIMIT ? 1 : sizeof(int);
	m->entries = (hl_hb_entry*)hl_gc_alloc_noptr(target_entries * sizeof(hl_hb_entry));
	m->values = (hl_hb_value*)hl_gc_alloc_raw(target_entries * sizeof(hl_hb_value));
	m->cells = hl_gc_alloc_noptr((ncells + target_entries) * ksize);
	m->nexts = (char*)m->cells + ncells * ksize;
	m->ncells = ncells;
	m->maxentries = target_entries;
	memset(m->cells, 0xFF, ncells * ksize);
	memset(m->values, 0, target_entries * sizeof(hl_hb_value));
	hl_freelist_init(&m->lfree);
	hl_freelist_add_range(&m->lfree, 0, target_entries);
}

static void hl_hb_presize_check(hl_hb_map *m, uchar *key) {
	// Check if key matches any preset prefix
	for(int i = 0; hl_hb_presets[i].prefix; i++) {
		const char *prefix = hl_hb_presets[i].prefix;
		int match = 1;
		for(int j = 0; prefix[j]; j++) {
			if(key[j] != (uchar)prefix[j]) {
				match = 0;
				break;
			}
		}
		if(match) {
			hl_hb_presize_direct(m, hl_hb_presets[i].target_size);
			return;
		}
	}
}

// Custom bytes map functions with pre-sizing support

// Simple first-key tracking for warning on missed pre-sizing
static hl_hb_map *hl_hb_tracked_map = NULL;
static char hl_hb_tracked_first_key[128];

HL_PRIM void hl_hbset( hl_hb_map *m, uchar *key, vdynamic *value ) {
	key = hl_hbfilter(key);
	int old_maxentries = m->maxentries;

	// Check for pre-sizing on first insert
	if(m->nentries == 0) {
		hl_hb_presize_check(m, key);
		// Track first key for potential warning (reuse single slot)
		hl_hb_tracked_map = m;
		int j = 0;
		for(int i = 0; key[i] && j < 127; i++) {
			if(key[i] < 128) hl_hb_tracked_first_key[j++] = (char)key[i];
		}
		hl_hb_tracked_first_key[j] = 0;
	}

	hl_hbset_impl(m, key, value);

	// Warn if map grew past 1000 entries (pre-sizing heuristic missed)
	if(m->maxentries > old_maxentries && m->maxentries >= 1000 && old_maxentries < 1000) {
		const char *first_key = (hl_hb_tracked_map == m) ? hl_hb_tracked_first_key : "(unknown)";
		fprintf(stderr, "[HL] Map grew to %d entries, consider pre-sizing for first_key=\"%s\"\n",
			m->maxentries, first_key);
	}
}

HL_PRIM bool hl_hbexists( hl_hb_map *m, uchar *key ) {
	return hl_hbfind(m, hl_hbfilter(key)) != NULL;
}

HL_PRIM vdynamic* hl_hbget( hl_hb_map *m, uchar *key ) {
	vdynamic **v = hl_hbfind(m, hl_hbfilter(key));
	if( v == NULL ) return NULL;
	return *v;
}

HL_PRIM bool hl_hbremove( hl_hb_map *m, uchar *key ) {
	int c, prev = -1, ckey;
	unsigned int hash;
	if( !m->cells ) return false;
	key = hl_hbfilter(key);
	hash = hl_hbhash(key);
	ckey = hash % ((unsigned)m->ncells);
	c = m->maxentries < _MLIMIT ? (int)((signed char*)m->cells)[ckey] : ((int*)m->cells)[ckey];
	while( c >= 0 ) {
		if( m->entries[c].hash == hash && ucmp(m->values[c].key,key) == 0 ) {
			hl_freelist_add(&m->lfree,c);
			m->nentries--;
			m->values[c].key = NULL;
			m->values[c].value = NULL;
			if( m->maxentries < _MLIMIT ) {
				if( prev >= 0 )
					((signed char*)m->nexts)[prev] = ((signed char*)m->nexts)[c];
				else
					((signed char*)m->cells)[ckey] = ((signed char*)m->nexts)[c];
			} else {
				if( prev >= 0 )
					((int*)m->nexts)[prev] = ((int*)m->nexts)[c];
				else
					((int*)m->cells)[ckey] = ((int*)m->nexts)[c];
			}
			return true;
		}
		prev = c;
		c = m->maxentries < _MLIMIT ? (int)((signed char*)m->nexts)[c] : ((int*)m->nexts)[c];
	}
	return false;
}

HL_PRIM varray* hl_hbkeys( hl_hb_map *m ) {
	varray *a = hl_alloc_array(&hlt_bytes,m->nentries);
	uchar **keys = hl_aptr(a,uchar*);
	int p = 0;
	for(int i = 0; i < m->ncells; i++) {
		int c = m->maxentries < _MLIMIT ? (int)((signed char*)m->cells)[i] : ((int*)m->cells)[i];
		while( c >= 0 ) {
			keys[p++] = m->values[c].key;
			c = m->maxentries < _MLIMIT ? (int)((signed char*)m->nexts)[c] : ((int*)m->nexts)[c];
		}
	}
	return a;
}

HL_PRIM varray* hl_hbvalues( hl_hb_map *m ) {
	varray *a = hl_alloc_array(&hlt_dyn,m->nentries);
	vdynamic **values = hl_aptr(a,vdynamic*);
	int p = 0;
	for(int i = 0; i < m->ncells; i++) {
		int c = m->maxentries < _MLIMIT ? (int)((signed char*)m->cells)[i] : ((int*)m->cells)[i];
		while( c >= 0 ) {
			values[p++] = m->values[c].value;
			c = m->maxentries < _MLIMIT ? (int)((signed char*)m->nexts)[c] : ((int*)m->nexts)[c];
		}
	}
	return a;
}

HL_PRIM void hl_hbclear( hl_hb_map *m ) {
	memset(m,0,sizeof(hl_hb_map));
}

HL_PRIM int hl_hbsize( hl_hb_map *m ) {
	return m->nentries;
}

// ----- OBJECT MAP ---------------------------------

typedef void hl_ho_entry;

typedef struct {
	vdynamic *key;
	vdynamic *value;
} hl_ho_value;

static vdynamic *hl_hofilter( vdynamic *key ) {
	if( key )
		switch( key->t->kind ) {
		// erase virtual (prevent mismatch once virtualized)
		case HVIRTUAL:
			key = hl_virtual_make_value((vvirtual*)key);
			break;
		// store real pointer instead of dynamic wrapper
		case HBYTES:
		case HTYPE:
		case HABSTRACT:
		case HREF:
		case HENUM:
			key = (vdynamic*)key->v.ptr;
			break;
		default:
			break;
		}
	return key;
}

#define hlt_key		hlt_dyn
#define hl_hohash(key)	((unsigned int)(int_val)(key))
#define _MKEY_TYPE	vdynamic*
#define _MNAME(n)	hl_ho##n
#define _MMATCH(c)	m->values[c].key == key
#define _MKEY(m,c)	m->values[c].key
#define	_MSET(c)	m->values[c].key = key
#define _MERASE(c)  m->values[c].key = NULL

#include "maps.h"

// ----- LOOKUP MAP ---------------------------------

#undef _MVAL_TYPE
#define _MVAL_TYPE int

typedef struct {
	void *key;
} hl_mlookup__entry;

typedef struct {
	int value;
} hl_mlookup__value;

#define hl_mlookup_hash(h) ((unsigned int)(int_val)(h))
#define _MKEY_TYPE	void*
#define _MNAME(n)	hl_mlookup_##n
#define _MMATCH(c)	m->entries[c].key == key
#define _MKEY(m,c)	m->entries[c].key
#define	_MSET(c)	m->entries[c].key = key
#define _MERASE(c)
#define _MNO_EXPORTS

#include "maps.h"

/// ----------------------------------------------

#define _IMAP _ABSTRACT(hl_int_map)
DEFINE_PRIM( _IMAP, hialloc, _NO_ARG );
DEFINE_PRIM( _VOID, hiset, _IMAP _I32 _DYN );
DEFINE_PRIM( _BOOL, hiexists, _IMAP _I32 );
DEFINE_PRIM( _DYN, higet, _IMAP _I32 );
DEFINE_PRIM( _BOOL, hiremove, _IMAP _I32 );
DEFINE_PRIM( _ARR, hikeys, _IMAP );
DEFINE_PRIM( _ARR, hivalues, _IMAP );
DEFINE_PRIM( _VOID, hiclear, _IMAP );
DEFINE_PRIM( _I32, hisize, _IMAP );

#define _I64MAP _ABSTRACT(hl_int64_map)
DEFINE_PRIM( _I64MAP, hi64alloc, _NO_ARG );
DEFINE_PRIM( _VOID, hi64set, _I64MAP _I64 _DYN );
DEFINE_PRIM( _BOOL, hi64exists, _I64MAP _I64 );
DEFINE_PRIM( _DYN, hi64get, _I64MAP _I64 );
DEFINE_PRIM( _BOOL, hi64remove, _I64MAP _I64 );
DEFINE_PRIM( _ARR, hi64keys, _I64MAP );
DEFINE_PRIM( _ARR, hi64values, _I64MAP );
DEFINE_PRIM( _VOID, hi64clear, _I64MAP );
DEFINE_PRIM( _I32, hi64size, _I64MAP );

#define _BMAP _ABSTRACT(hl_bytes_map)
DEFINE_PRIM( _BMAP, hballoc, _NO_ARG );
DEFINE_PRIM( _VOID, hbset, _BMAP _BYTES _DYN );
DEFINE_PRIM( _BOOL, hbexists, _BMAP _BYTES );
DEFINE_PRIM( _DYN, hbget, _BMAP _BYTES );
DEFINE_PRIM( _BOOL, hbremove, _BMAP _BYTES );
DEFINE_PRIM( _ARR, hbkeys, _BMAP );
DEFINE_PRIM( _ARR, hbvalues, _BMAP );
DEFINE_PRIM( _VOID, hbclear, _BMAP );
DEFINE_PRIM( _I32, hbsize, _BMAP );

#define _OMAP _ABSTRACT(hl_obj_map)
DEFINE_PRIM( _OMAP, hoalloc, _NO_ARG );
DEFINE_PRIM( _VOID, hoset, _OMAP _DYN _DYN );
DEFINE_PRIM( _BOOL, hoexists, _OMAP _DYN );
DEFINE_PRIM( _DYN, hoget, _OMAP _DYN );
DEFINE_PRIM( _BOOL, horemove, _OMAP _DYN );
DEFINE_PRIM( _ARR, hokeys, _OMAP );
DEFINE_PRIM( _ARR, hovalues, _OMAP );
DEFINE_PRIM( _VOID, hoclear, _OMAP );
DEFINE_PRIM( _I32, hosize, _OMAP );
