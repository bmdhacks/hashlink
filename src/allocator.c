/*
 * Copyright (C)2005-2020 Haxe Foundation
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
#include "hl.h"

#ifdef HL_WIN
#	include <intrin.h>
static unsigned int __inline TRAILING_ONES( unsigned int x ) {
	DWORD msb = 0;
	if( _BitScanForward( &msb, ~x ) )
		return msb;
	return 32;
}
static unsigned int __inline TRAILING_ZEROES( unsigned int x ) {
	DWORD msb = 0;
	if( _BitScanForward( &msb, x ) )
		return msb;
	return 32;
}
#else
static inline unsigned int TRAILING_ONES( unsigned int x ) {
	return (~x) ? __builtin_ctz(~x) : 32;
}
static inline unsigned int TRAILING_ZEROES( unsigned int x ) {
	return x ? __builtin_ctz(x) : 32;
}
#endif

#define GC_PARTITIONS	9
#define GC_PART_BITS	4
#define GC_FIXED_PARTS	5
#define GC_LARGE_PART	(GC_PARTITIONS-1)
#define GC_LARGE_BLOCK	(1 << 20)
static const int GC_SBITS[GC_PARTITIONS] = {0,0,0,0,0,		3,6,13,0};

#ifdef HL_64
static const int GC_SIZES[GC_PARTITIONS] = {8,16,24,32,40,	8,64,1<<13,0};
#	define GC_ALIGN_BITS		3
#else
static const int GC_SIZES[GC_PARTITIONS] = {4,8,12,16,20,	8,64,1<<13,0};
#	define GC_ALIGN_BITS		2
#endif


#define GC_ALL_PAGES	(GC_PARTITIONS << PAGE_KIND_BITS)
#define	GC_ALIGN		(1 << GC_ALIGN_BITS)

static gc_pheader *gc_pages[GC_ALL_PAGES] = {NULL};
static gc_pheader *gc_free_pages[GC_ALL_PAGES] = {NULL};

#define MAX_FL_CACHED 16

typedef struct {
	int count;
	gc_fl *data[MAX_FL_CACHED];
} cached_slot;

static cached_slot cached_slots[32] = {0};
static int free_lists_size = 0;
static int free_lists_count = 0;

static void alloc_freelist( gc_freelist *fl, int size ) {
	cached_slot *slot = &cached_slots[size];
	if( slot->count ) {
		fl->data = slot->data[--slot->count];
		fl->size_bits = size;
		fl->count = 0;
		fl->current = 0;
		return;
	}
	int bytes = (int)sizeof(gc_fl) * (1<<size);
	free_lists_size += bytes;
	free_lists_count++;
	fl->data = (gc_fl*)malloc(bytes);
	fl->count = 0;
	fl->current = 0;
	fl->size_bits = size;
}

static void free_freelist( gc_freelist *fl ) {
	cached_slot *slot = &cached_slots[fl->size_bits];
	if( slot->count == MAX_FL_CACHED ) {
		free(fl->data);
		free_lists_size -= (int)sizeof(gc_fl) * (1 << fl->size_bits);
		return;
	}
	slot->data[slot->count++] = fl->data;
}


#define GET_FL(fl,pos) ((fl)->data + (pos))

static void freelist_append( gc_freelist *fl, int pos, int count ) {
	if( fl->count == 1<<fl->size_bits ) {
#		ifdef GC_DEBUG
		if( fl->current ) hl_fatal("assert");
#		endif
		gc_freelist fl2;
		alloc_freelist(&fl2, fl->size_bits + 1);
		memcpy(GET_FL(&fl2,0),GET_FL(fl,0),sizeof(gc_fl)*fl->count);
		free_freelist(fl);
		fl->size_bits++;
		fl->data = fl2.data;
	}
	gc_fl *p = GET_FL(fl,fl->count++);
	p->pos = (fl_cursor)pos;
	p->count = (fl_cursor)count;
}

static gc_pheader *gc_allocator_new_page( int pid, int block, int size, int kind, bool varsize ) {
	// increase size based on previously allocated pages
	if( block < 256 ) {
		int num_pages = 0;
		int count = 1;
		gc_pheader *ph = gc_pages[pid];
		while( ph ) {
			num_pages++;
			ph = ph->next_page;
		}
		while( num_pages > 8 && (size<<1) / block <= GC_PAGE_SIZE ) {
			size <<= 1;
			count <<= 1;
			num_pages /= 3;
#			ifdef HL_NX
			// do not allocate too much large pages with low memory
			if( count == 4 ) break;
#			endif
		}
	}

	int start_pos = 0;
	int max_blocks = size / block;

	gc_pheader *ph = gc_alloc_page(size, kind, max_blocks);
	gc_allocator_page_data *p = &ph->alloc;

	p->block_size = block;
	p->size_bits = 0;
	while( block < (1<<p->size_bits) )
		p->size_bits++;
	if( block != (1<<p->size_bits) )
		p->size_bits = 0;
	p->max_blocks = max_blocks;
	p->sizes = NULL;
	if( p->max_blocks > GC_PAGE_SIZE )
		hl_fatal("Too many blocks for this page");
	if( varsize ) {
		if( p->max_blocks <= SIZES_PADDING )
			p->sizes = (unsigned char*)&p->sizes_ref;
		else {
			p->sizes = ph->base + start_pos;
			start_pos += p->max_blocks;
			start_pos += (-start_pos) & 63; // align on cache line
		}
		MZERO(p->sizes,p->max_blocks);
	}
	int m = start_pos % block;
	if( m ) start_pos += block - m;
	int fl_bits = 1;
	while( fl_bits < 8 && (1<<fl_bits) < (p->max_blocks>>3) ) fl_bits++;
	p->first_block = start_pos / block;
	alloc_freelist(&p->free,fl_bits);
	freelist_append(&p->free,p->first_block, p->max_blocks - p->first_block);
	p->need_flush = false;

	ph->next_page = gc_pages[pid];
	gc_pages[pid] = ph;

	return ph;
}


static void flush_free_list( gc_pheader *ph ) {
	gc_allocator_page_data *p = &ph->alloc;

	int bid = p->first_block;
	int last = p->max_blocks;
	gc_freelist new_fl;
	alloc_freelist(&new_fl,p->free.size_bits);
	gc_freelist old_fl = p->free;
	gc_fl *cur_pos = NULL;
	int reuse_index = old_fl.current;
	gc_fl *reuse = reuse_index < old_fl.count ? GET_FL(&old_fl,reuse_index++) : NULL;
	int next_bid = reuse ? reuse->pos : -1;
	unsigned char *bmp = ph->bmp;

	while( bid < last ) {
		if( bid == next_bid ) {
			if( cur_pos && cur_pos->pos + cur_pos->count == bid ) {
				cur_pos->count += reuse->count;
			} else {
				freelist_append(&new_fl,reuse->pos,reuse->count);
				cur_pos = GET_FL(&new_fl,new_fl.count - 1);
			}
			reuse = reuse_index < old_fl.count ? GET_FL(&old_fl,reuse_index++) : NULL;
			bid = cur_pos->count + cur_pos->pos;
			next_bid = reuse ? reuse->pos : -1;
			continue;
		}
		fl_cursor count;
		if( p->sizes ) {
			count = p->sizes[bid];
			if( !count ) count = 1;
		} else
			count = 1;
		if( (bmp[bid>>3] & (1<<(bid&7))) == 0 ) {
			if (gc_trace_active) gc_trace_free(ph->base + bid * p->block_size);
			if( p->sizes ) p->sizes[bid] = 0;
			if( cur_pos && cur_pos->pos + cur_pos->count == bid )
				cur_pos->count += count;
			else {
				freelist_append(&new_fl,bid,count);
				cur_pos = GET_FL(&new_fl,new_fl.count - 1);
			}
		}
		bid += count;
	}
	p->free = new_fl;
	p->need_flush = false;
#ifdef __GC_DEBUG
	if( ph->page_id == -1 ) {
		int k;
		for(k=0;k<p->free.count;k++) {
			gc_fl *fl = GET_FL(&p->free,k);
			printf("(%d-%d)",fl->pos,fl->pos+fl->count-1);
		}
		printf("\n");
	}
	if( reuse && reuse->count ) hl_fatal("assert");
	for(bid=p->first_block;bid<p->max_blocks;bid++) {
		int k;
		bool is_free = false;
		for(k=0;k<p->free.count;k++) {
			gc_fl *fl = GET_FL(&p->free,k);
			if( fl->pos < p->first_block || fl->pos + fl->count > p->max_blocks ) hl_fatal("assert");
			if( bid >= fl->pos && bid < fl->pos + fl->count ) {
				if( is_free ) hl_fatal("assert");
				is_free = true;
			}
		}
		bool is_marked = ((ph->bmp[bid>>3] & (1<<(bid&7))) != 0);
		if( is_marked && is_free ) {
			// check if it was already free before
			for(k=0;k<old_fl.count;k++) {
				gc_fl *fl = GET_FL(&old_fl,k);
				if( bid >= fl->pos && bid < fl->pos+fl->count ) {
					is_marked = false; // false positive
					ph->bmp[bid>>3] &= ~(1<<(bid&7));
					break;
				}
			}
		}
		if( is_free == is_marked )
			hl_fatal("assert");
		if( p->sizes && !is_free )
			bid += p->sizes[bid]-1;
	}
#endif
	free_freelist(&old_fl);
}

static void *gc_alloc_fixed( int part, int kind ) {
	int pid = (part << PAGE_KIND_BITS) | kind;
	gc_pheader *ph = gc_free_pages[pid];
	gc_allocator_page_data *p = NULL;
	int bid = -1;
	while( ph ) {
		p = &ph->alloc;
		if( p->need_flush )
			flush_free_list(ph);
		gc_freelist *fl = &p->free;
		if( fl->current < fl->count ) {
			gc_fl *c = GET_FL(fl,fl->current);
			bid = c->pos++;
			c->count--;
#			ifdef GC_DEBUG
			if( c->count < 0 ) hl_fatal("assert");
#			endif
			if( !c->count ) fl->current++;
			break;
		}
		ph = ph->next_page;
	}
	if( ph == NULL ) {
		ph = gc_allocator_new_page(pid, GC_SIZES[part], GC_PAGE_SIZE, kind, false);
		p = &ph->alloc;
		bid = p->free.data->pos++;
		p->free.data->count--;
	}
	unsigned char *ptr = ph->base + bid * p->block_size;
#	ifdef GC_DEBUG
	{
		int i;
		if( bid < p->first_block || bid >= p->max_blocks )
			hl_fatal("assert");
		for(i=0;i<p->block_size;i++)
			if( ptr[i] != 0xDD )
				hl_fatal("assert");
	}
#	endif
	gc_free_pages[pid] = ph;
	return ptr;
}

static void *gc_alloc_var( int part, int size, int kind ) {
	int pid = (part << PAGE_KIND_BITS) | kind;
	gc_pheader *ph = gc_free_pages[pid];
	gc_allocator_page_data *p = NULL;
	unsigned char *ptr;
	fl_cursor nblocks = (fl_cursor)(size >> GC_SBITS[part]);
	int bid = -1;
	while( ph ) {
		p = &ph->alloc;
		if( p->need_flush )
			flush_free_list(ph);
		gc_freelist *fl = &p->free;
		int k;
		for(k=fl->current;k<fl->count;k++) {
			gc_fl *c = GET_FL(fl,k);
			if( c->count >= nblocks ) {
				bid = c->pos;
				c->pos += nblocks;
				c->count -= nblocks;
#				ifdef GC_DEBUG
				if( c->count < 0 ) hl_fatal("assert");
#				endif
				if( c->count == 0 ) fl->current++;
				goto alloc_var;
			}
		}
		ph = ph->next_page;
	}
	if( ph == NULL ) {
		int psize = GC_PAGE_SIZE;
		while( psize < size + 1024 )
			psize <<= 1;
		ph = gc_allocator_new_page(pid, GC_SIZES[part], psize, kind, true);
		p = &ph->alloc;
		bid = p->first_block;
		p->free.data->pos += nblocks;
		p->free.data->count -= nblocks;
	}
alloc_var:
	ptr = ph->base + bid * p->block_size;
#	ifdef GC_DEBUG
	{
		int i;
		if( bid < p->first_block || bid + nblocks > p->max_blocks )
			hl_fatal("assert");
		for(i=0;i<size;i++)
			if( ptr[i] != 0xDD )
				hl_fatal("assert");
	}
#	endif
	if( ph->bmp ) {
#		ifdef GC_DEBUG
		int i;
		for(i=0;i<nblocks;i++) {
			int b = bid + i;
			if( (ph->bmp[b>>3]&(1<<(b&7))) != 0 ) hl_fatal("Alloc on marked block");
		}
#		endif
		ph->bmp[bid>>3] |= 1<<(bid&7);
	}
	if( nblocks > 1 ) MZERO(p->sizes + bid, nblocks);
	p->sizes[bid] = (unsigned char)nblocks;
	gc_free_pages[pid] = ph;
	return ptr;
}

static void *gc_allocator_alloc( int *size, int page_kind ) {
	int sz = *size;
	sz += (-sz) & (GC_ALIGN - 1);
	if( sz >= GC_LARGE_BLOCK ) {
		sz += (-sz) & (GC_PAGE_SIZE - 1);
		*size = sz;
		gc_pheader *ph = gc_allocator_new_page((GC_LARGE_PART << PAGE_KIND_BITS) | page_kind,sz,sz,page_kind,false);
		return ph->base;
	}
	if( sz <= GC_SIZES[GC_FIXED_PARTS-1] && page_kind != MEM_KIND_FINALIZER ) {
		int part = (sz >> GC_ALIGN_BITS) - 1;
		*size = GC_SIZES[part];
		return gc_alloc_fixed(part, page_kind);
	}
	int p;
	for(p=GC_FIXED_PARTS;p<GC_PARTITIONS;p++) {
		int block = GC_SIZES[p];
		int query = sz + ((-sz) & (block - 1));
		if( query < block * 255 ) {
			*size = query;
			return gc_alloc_var(p, query, page_kind);
		}
	}
	*size = -1;
	return NULL;
}

static bool is_zero( void *ptr, int size ) {
	static char ZEROMEM[256] = {0};
	unsigned char *p = (unsigned char*)ptr;
	while( size>>8 ) {
		if( memcmp(p,ZEROMEM,256) ) return false;
		p += 256;
		size -= 256;
	}
	return memcmp(p,ZEROMEM,size) == 0;
}

static void gc_trace_free_page_blocks(gc_pheader *ph) {
	gc_allocator_page_data *p = &ph->alloc;
	if( p->sizes ) {
		int bid = p->first_block;
		while( bid < p->max_blocks ) {
			if( p->sizes[bid] ) {
				gc_trace_free(ph->base + bid * p->block_size);
				bid += p->sizes[bid];
			} else {
				bid++;
			}
		}
	} else {
		gc_freelist *fl = &p->free;
		int fl_idx = fl->current;
		int bid = p->first_block;
		while( bid < p->max_blocks ) {
			if( fl_idx < fl->count ) {
				gc_fl *entry = GET_FL(fl, fl_idx);
				if( bid < entry->pos ) {
					gc_trace_free(ph->base + bid * p->block_size);
					bid++;
				} else if( bid < entry->pos + entry->count ) {
					bid = entry->pos + entry->count;
					fl_idx++;
				} else {
					fl_idx++;
				}
			} else {
				gc_trace_free(ph->base + bid * p->block_size);
				bid++;
			}
		}
	}
}

static void gc_flush_empty_pages() {
	int i;
	for(i=0;i<GC_ALL_PAGES;i++) {
		gc_pheader *ph = gc_pages[i];
		gc_pheader *prev = NULL;
		while( ph ) {
			gc_allocator_page_data *p = &ph->alloc;
			gc_pheader *next = ph->next_page;
			if( ph->bmp && is_zero(ph->bmp+(p->first_block>>3),((p->max_blocks+7)>>3) - (p->first_block>>3)) ) {
				if (gc_trace_active) gc_trace_free_page_blocks(ph);
				if( prev )
					prev->next_page = next;
				else
					gc_pages[i] = next;
				if( gc_free_pages[i] == ph )
					gc_free_pages[i] = next;
				free_freelist(&p->free);
				gc_free_page(ph, p->max_blocks);
			} else
				prev = ph;
			ph = next;
		}
	}
}

// Tell OS to reclaim physical memory for free regions (Linux only)
// IMPORTANT: We can only call madvise on an OS page if ALL GC blocks
// that overlap with that page are free, because MADV_DONTNEED zeros the entire OS page.
// We also must skip any OS pages that overlap with reserved metadata areas.
#if defined(__linux__) && !defined(HL_CONSOLE)
#include <sys/mman.h>
#include <unistd.h>

static int gc_madvise_enabled = -1;  // -1 = not initialized, 0 = disabled, 1 = enabled
static long gc_os_page_size = 0;     // Actual OS page size (4KB, 16KB, 64KB, etc.)

static void gc_madvise_free_regions() {
	// Check if madvise is enabled (can be disabled via HL_GC_NO_MADVISE=1)
	if( gc_madvise_enabled < 0 ) {
		gc_madvise_enabled = getenv("HL_GC_NO_MADVISE") ? 0 : 1;
		gc_os_page_size = sysconf(_SC_PAGESIZE);
		if( gc_os_page_size <= 0 ) gc_os_page_size = 4096;  // fallback
	}
	if( !gc_madvise_enabled )
		return;

	long page_size = gc_os_page_size;
	long page_mask = page_size - 1;

	int i;
	for(i=0;i<GC_ALL_PAGES;i++) {
		gc_pheader *ph = gc_pages[i];
		while( ph ) {
			gc_allocator_page_data *p = &ph->alloc;

			if( ph->bmp == NULL ) {
				ph = ph->next_page;
				continue;
			}

			int block_size = p->block_size;
			int first_block = p->first_block;
			int max_blocks = p->max_blocks;
			unsigned char *bmp = ph->bmp;
			unsigned char *base = ph->base;
			int gc_page_size = ph->page_size;

			if( p->sizes != NULL ) {
				// Variable-size page: walk allocations to find contiguous free ranges
				int bid = first_block;
				int free_range_start = -1;  // byte offset, or -1 if not in a free range

				while( bid < max_blocks ) {
					int alloc_size = p->sizes[bid];
					if( alloc_size == 0 ) alloc_size = 1;

					bool is_live = (bmp[bid>>3] & (1<<(bid&7))) != 0;

					if( is_live ) {
						// End of free range - madvise any full OS pages within it
						if( free_range_start >= 0 ) {
							int free_range_end = bid * block_size;
							// Align start up, end down to OS page boundaries
							int aligned_start = (free_range_start + page_size - 1) & ~(page_size - 1);
							int aligned_end = free_range_end & ~(page_size - 1);
							if( aligned_end > aligned_start ) {
								madvise(base + aligned_start, aligned_end - aligned_start, MADV_DONTNEED);
							}
							free_range_start = -1;
						}
					} else {
						// Free allocation - extend or start free range
						if( free_range_start < 0 ) {
							free_range_start = bid * block_size;
						}
						// free_range implicitly extends to (bid + alloc_size) * block_size
					}

					bid += alloc_size;
				}

				// Handle trailing free range
				if( free_range_start >= 0 ) {
					int free_range_end = max_blocks * block_size;
					if( free_range_end > gc_page_size ) free_range_end = gc_page_size;
					int aligned_start = (free_range_start + page_size - 1) & ~(page_size - 1);
					int aligned_end = free_range_end & ~(page_size - 1);
					if( aligned_end > aligned_start ) {
						madvise(base + aligned_start, aligned_end - aligned_start, MADV_DONTNEED);
					}
				}
			} else {
				// Fixed-size page: each block is one allocation, bitmap is authoritative
				// Calculate the byte offset where usable blocks start
				int usable_start_offset = first_block * block_size;
				// Align up to next OS page boundary
				int first_safe_os_page = (usable_start_offset + page_size - 1) & ~(page_size - 1);

				// Iterate through each OS page that's FULLY within usable block range
				for( int os_page_offset = first_safe_os_page; os_page_offset + page_size <= gc_page_size; os_page_offset += page_size ) {
					// Calculate which blocks overlap with this OS page
					int start_block = os_page_offset / block_size;
					int end_block = (os_page_offset + page_size + block_size - 1) / block_size;
					if( start_block < first_block ) start_block = first_block;
					if( end_block > max_blocks ) end_block = max_blocks;

					// Skip if this OS page doesn't contain any usable blocks
					if( start_block >= end_block )
						continue;

					// Check if ALL blocks overlapping this OS page are free
					bool all_free = true;
					for( int bid = start_block; bid < end_block && all_free; bid++ ) {
						if( bmp[bid>>3] & (1<<(bid&7)) ) {
							all_free = false;  // Found a live block
						}
					}

					if( all_free ) {
						madvise(base + os_page_offset, page_size, MADV_DONTNEED);
					}
				}
			}
			ph = ph->next_page;
		}
	}
}
#else
static void gc_madvise_free_regions() {
	// No-op on non-Linux platforms
}
#endif

static int64 gc_allocator_private_memory() {
	return free_lists_size;
}

static int gc_free_memory( gc_pheader *ph ) {
	gc_allocator_page_data *p = &ph->alloc;
	if( p->need_flush )
		flush_free_list(ph);
	gc_freelist *fl = &p->free;
	int k;
	int free = 0;
	for(k=fl->current;k<fl->count;k++) {
		gc_fl *c = GET_FL(fl,k);
		free += c->count * p->block_size;
	}
	return free;
}

#ifdef GC_DEBUG
static void gc_clear_unmarked_mem() {
	int i;
	for(i=0;i<GC_ALL_PAGES;i++) {
		gc_pheader *ph = gc_pages[i];
		while( ph ) {
			int bid;
			gc_allocator_page_data *p = &ph->alloc;
			for(bid=p->first_block;bid<p->max_blocks;bid++) {
				if( p->sizes && !p->sizes[bid] ) continue;
				int size = p->sizes ? p->sizes[bid] * p->block_size : p->block_size;
				unsigned char *ptr = ph->base + bid * p->block_size;
				if( bid * p->block_size + size > ph->page_size ) hl_fatal("invalid block size");
#				ifdef GC_MEMCHK
				int_val eob = *(int_val*)(ptr + size - HL_WSIZE);
#				ifdef HL_64
				if( eob != 0xEEEEEEEEEEEEEEEE && eob != 0xDDDDDDDDDDDDDDDD )
#				else
				if( eob != 0xEEEEEEEE && eob != 0xDDDDDDDD )
#				endif
					hl_fatal("Block written out of bounds");
#				endif
				if( (ph->bmp[bid>>3] & (1<<(bid&7))) == 0 ) {
					memset(ptr,0xDD,size);
					if( p->sizes ) p->sizes[bid] = 0;
				}
			}
			ph = ph->next_page;
		}
	}
}
#endif

static void gc_call_finalizers(){
	int i;
	for(i=MEM_KIND_FINALIZER;i<GC_ALL_PAGES;i+=1<<PAGE_KIND_BITS) {
		gc_pheader *ph = gc_pages[i];
		while( ph ) {
			int bid;
			gc_allocator_page_data *p = &ph->alloc;
			for(bid=p->first_block;bid<p->max_blocks;bid++) {
				int size = p->sizes[bid];
				if( !size ) continue;
				if( (ph->bmp[bid>>3] & (1<<(bid&7))) == 0 ) {
					unsigned char *ptr = ph->base + bid * p->block_size;
					void *finalizer = *(void**)ptr;
					p->sizes[bid] = 0;
					if( finalizer )
						((void(*)(void *))finalizer)(ptr);
#					ifdef GC_DEBUG
					memset(ptr,0xDD,size*p->block_size);
#					endif
				}
			}
			ph = ph->next_page;
		}
	}
}

static void gc_allocator_before_mark( unsigned char *mark_cur ) {
	int pid;
	for(pid=0;pid<GC_ALL_PAGES;pid++) {
		gc_pheader *p = gc_pages[pid];
		gc_free_pages[pid] = p;
		while( p ) {
			p->bmp = mark_cur;
			p->alloc.need_flush = true;
			mark_cur += (p->alloc.max_blocks + 7) >> 3;
			p = p->next_page;
		}
	}
}

#define gc_allocator_fast_block_size(page,block) \
	(page->alloc.sizes ? page->alloc.sizes[(int)(((unsigned char*)(block)) - page->base) / page->alloc.block_size] * page->alloc.block_size : page->alloc.block_size)

static void gc_allocator_init() {
	if( TRAILING_ONES(0x080003FF) != 10 || TRAILING_ONES(0) != 0 || TRAILING_ONES(0xFFFFFFFF) != 32 )
		hl_fatal("Invalid builtin tl1");
	if( TRAILING_ZEROES((unsigned)~0x080003FF) != 10 || TRAILING_ZEROES(0) != 32 || TRAILING_ZEROES(0xFFFFFFFF) != 0 )
		hl_fatal("Invalid builtin tl0");
}

static int gc_allocator_get_block_id( gc_pheader *page, void *block ) {
	int offset = (int)((unsigned char*)block - page->base);
	int bid;
	if( page->alloc.size_bits ) {
		bid = offset >> page->alloc.size_bits;
		if( bid << page->alloc.size_bits != offset )
			return -1;
	} else {
		bid = offset / page->alloc.block_size;
		if( bid * page->alloc.block_size != offset )
			return -1;
	}
	if( bid >= page->alloc.max_blocks )
		return -1;
	if( page->alloc.sizes ) {
		if( bid < page->alloc.first_block )
			return -1;
		if( page->alloc.sizes[bid] == 0 )
			return -1;
	}
	return bid;
}

#ifdef GC_INTERIOR_POINTERS
static int gc_allocator_get_block_interior( gc_pheader *page, void **block ) {
	int offset = (int)((unsigned char*)*block - page->base);
	int bid = offset / page->alloc.block_size;
	if( bid >= page->alloc.max_blocks )
		return -1;
	if( page->alloc.sizes ) {
		if( bid < page->alloc.first_block ) return -1;
		int start = bid;
		while( page->alloc.sizes[bid] == 0 ) {
			if( bid == page->alloc.first_block ) return -1;
			bid--;
		}
		int size = page->alloc.sizes[bid];
		if( (start - bid) >= size )
			return -1;
	}
	*block = page->base + bid * page->alloc.block_size;
	return bid;
}
#endif

static void gc_allocator_after_mark() {
	gc_call_finalizers();
#	ifdef GC_DEBUG
	gc_clear_unmarked_mem();
#	endif
	gc_flush_empty_pages();
	gc_madvise_free_regions();
	if (gc_trace_active) {
		// Force eager free-list rebuild on all pages so tracer sees
		// frees at GC time rather than at next allocation.
		int pid;
		for(pid=0;pid<GC_ALL_PAGES;pid++) {
			gc_pheader *ph = gc_pages[pid];
			while( ph ) {
				if( ph->alloc.need_flush )
					flush_free_list(ph);
				ph = ph->next_page;
			}
		}
	}
}

static void gc_get_stats( int *page_count, int *private_data ) {
	int count = 0;
	int i;
	for(i=0;i<GC_ALL_PAGES;i++) {
		gc_pheader *p = gc_pages[i];
		while( p ) {
			count++;
			p = p->next_page;
		}
	}
	*page_count = count;
	*private_data = 0; // no malloc
}

static void gc_iter_pages( gc_page_iterator iter ) {
	int i;
	for(i=0;i<GC_ALL_PAGES;i++) {
		gc_pheader *p = gc_pages[i];
		while( p ) {
			int size = 0;
			if( p->alloc.sizes && p->alloc.max_blocks > 8 ) size = p->alloc.max_blocks;
			iter(p,size);
			p = p->next_page;
		}
	}
}

static void gc_iter_live_blocks( gc_pheader *ph, gc_block_iterator iter ) {
	int i;
	gc_allocator_page_data *p = &ph->alloc;
	for(i=0;i<p->max_blocks;i++) {
		if( ph->bmp[(i>>3)] & (1<<(i&7)) )
			iter(ph->base + i*p->block_size,p->sizes?p->sizes[i]*p->block_size:p->block_size);
	}
}
