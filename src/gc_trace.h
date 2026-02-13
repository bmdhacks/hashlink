#ifndef GC_TRACE_H
#define GC_TRACE_H

void gc_trace_init(void);
void gc_trace_close(void);
void gc_trace_alloc(void *ptr, int size);
void gc_trace_free(void *ptr);
extern int gc_trace_active;

#endif
