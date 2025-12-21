/*
 * JIT ELF Generation for Debug/Profiler Support
 *
 * Generates an ELF shared object containing JIT-compiled code with
 * debug symbols for use with profilers (heaptrack, perf) and debuggers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "jit_common.h"
#include "jit_elf.h"

/* Page size for alignment (64KB on ARM64) */
#define PAGE_SIZE 0x10000

/* Helper to align offset to boundary */
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

/* ELF writer context */
typedef struct {
    unsigned char *buf;
    int pos;
    int size;
    FILE *fp;
} elf_writer;

static void elf_init(elf_writer *w, int initial_size) {
    w->buf = (unsigned char *)malloc(initial_size);
    w->pos = 0;
    w->size = initial_size;
    w->fp = NULL;
}

static void elf_grow(elf_writer *w, int needed) {
    if (w->pos + needed > w->size) {
        int new_size = w->size * 2;
        while (w->pos + needed > new_size)
            new_size *= 2;
        w->buf = (unsigned char *)realloc(w->buf, new_size);
        w->size = new_size;
    }
}

static void elf_write(elf_writer *w, const void *data, int len) {
    elf_grow(w, len);
    memcpy(w->buf + w->pos, data, len);
    w->pos += len;
}

static void elf_write8(elf_writer *w, uint8_t val) {
    elf_grow(w, 1);
    w->buf[w->pos++] = val;
}

static void elf_write16(elf_writer *w, uint16_t val) {
    elf_grow(w, 2);
    memcpy(w->buf + w->pos, &val, 2);
    w->pos += 2;
}

static void elf_write32(elf_writer *w, uint32_t val) {
    elf_grow(w, 4);
    memcpy(w->buf + w->pos, &val, 4);
    w->pos += 4;
}

static void elf_write64(elf_writer *w, uint64_t val) {
    elf_grow(w, 8);
    memcpy(w->buf + w->pos, &val, 8);
    w->pos += 8;
}

static void elf_pad(elf_writer *w, int alignment) {
    int pad = ALIGN(w->pos, alignment) - w->pos;
    if (pad > 0) {
        elf_grow(w, pad);
        memset(w->buf + w->pos, 0, pad);
        w->pos += pad;
    }
}

static void elf_free(elf_writer *w) {
    if (w->buf) free(w->buf);
    w->buf = NULL;
}

/*
 * Build function name from hl_function structure.
 * Returns allocated string (caller must NOT free - uses hl_to_utf8 which is GC'd).
 */
static const char *get_function_name(hl_function *f, char *fallback, int fallback_size) {
    if (f->obj) {
        /* Method: ClassName.methodName */
        static char name[512];
        char *cls = hl_to_utf8(f->obj->name);
        char *meth = hl_to_utf8(f->field.name);
        snprintf(name, sizeof(name), "%s.%s", cls, meth);
        return name;
    } else if (f->field.ref) {
        /* Closure: ClassName.~parentMethod.closureIndex */
        static char name[512];
        char *cls = hl_to_utf8(f->field.ref->obj->name);
        char *meth = hl_to_utf8(f->field.ref->field.name);
        snprintf(name, sizeof(name), "%s.~%s.%d", cls, meth, f->ref);
        return name;
    } else {
        /* Anonymous function */
        snprintf(fallback, fallback_size, "fun$%d", f->findex);
        return fallback;
    }
}

/*
 * Section indices (order matters for section header table)
 */
enum {
    SEC_NULL = 0,
    SEC_TEXT,
    SEC_EH_FRAME,
    SEC_EH_FRAME_HDR,
    SEC_HASH,
    SEC_DYNAMIC,
    SEC_DYNSYM,
    SEC_DYNSTR,
    SEC_SYMTAB,
    SEC_STRTAB,
    SEC_SHSTRTAB,
    SEC_COUNT
};

/* Frame size used by JIT (must match CALLEE_SAVED_FRAME_SIZE in jit_aarch64.c) */
#define JIT_FRAME_SIZE 160

/*
 * Write JIT code as ELF shared object.
 */
int write_jit_elf(const char *path, jit_ctx *ctx, hl_module *m,
                  int code_size, unsigned char *code) {
    elf_writer w;
    elf_writer strtab;   /* .strtab string table */
    elf_writer shstrtab; /* .shstrtab section name strings */
    int i;

    /* Section name string offsets */
    int shstr_text, shstr_eh_frame, shstr_eh_frame_hdr;
    int shstr_hash, shstr_dynamic, shstr_dynsym, shstr_dynstr;
    int shstr_symtab, shstr_strtab, shstr_shstrtab;

    /* Section offsets and sizes */
    uint64_t text_offset, text_size;
    uint64_t eh_frame_offset, eh_frame_size;
    uint64_t eh_frame_hdr_offset, eh_frame_hdr_size;
    uint64_t hash_offset, hash_size;
    uint64_t dynamic_offset, dynamic_size;
    uint64_t dynsym_offset, dynsym_size;
    uint64_t dynstr_offset, dynstr_size;
    uint64_t symtab_offset, symtab_size;
    uint64_t strtab_offset, strtab_size;
    uint64_t shstrtab_offset, shstrtab_size;
    uint64_t shdr_offset;

    /* Virtual addresses (relative, will be relocated by dlopen) */
    uint64_t text_vaddr = 0;
    uint64_t dynamic_vaddr;

    int num_functions = m->code->nfunctions;
    int num_symbols = num_functions + 1; /* +1 for _hl_jit_code base symbol */

    elf_init(&w, code_size + 0x10000);
    elf_init(&strtab, 0x10000);
    elf_init(&shstrtab, 256);

    /* ========== Build section name string table (.shstrtab) ========== */
    elf_write8(&shstrtab, 0); /* First byte is null */
    shstr_text = shstrtab.pos;
    elf_write(&shstrtab, ".text", 6);
    shstr_eh_frame = shstrtab.pos;
    elf_write(&shstrtab, ".eh_frame", 10);
    shstr_eh_frame_hdr = shstrtab.pos;
    elf_write(&shstrtab, ".eh_frame_hdr", 14);
    shstr_hash = shstrtab.pos;
    elf_write(&shstrtab, ".hash", 6);
    shstr_dynamic = shstrtab.pos;
    elf_write(&shstrtab, ".dynamic", 9);
    shstr_dynsym = shstrtab.pos;
    elf_write(&shstrtab, ".dynsym", 8);
    shstr_dynstr = shstrtab.pos;
    elf_write(&shstrtab, ".dynstr", 8);
    shstr_symtab = shstrtab.pos;
    elf_write(&shstrtab, ".symtab", 8);
    shstr_strtab = shstrtab.pos;
    elf_write(&shstrtab, ".strtab", 8);
    shstr_shstrtab = shstrtab.pos;
    elf_write(&shstrtab, ".shstrtab", 10);

    /* ========== Build symbol string table (.strtab) ========== */
    elf_write8(&strtab, 0); /* First byte is null */

    /* Base symbol for dlsym */
    int base_sym_name = strtab.pos;
    elf_write(&strtab, "_hl_jit_code", 13);

    /* Function symbols - record name offsets */
    int *sym_names = (int *)malloc(sizeof(int) * num_functions);
    for (i = 0; i < num_functions; i++) {
        hl_function *f = &m->code->functions[i];
        char fallback[64];
        const char *name = get_function_name(f, fallback, sizeof(fallback));
        sym_names[i] = strtab.pos;
        elf_write(&strtab, name, strlen(name) + 1);
    }

    /* ========== Calculate layout ========== */

    /* ELF header at 0 */
    int ehdr_size = sizeof(Elf64_Ehdr);

    /* Program headers follow ELF header */
    int phdr_count = 4; /* PT_LOAD for code, PT_LOAD for data, PT_DYNAMIC, PT_GNU_EH_FRAME */
    int phdr_offset = ehdr_size;
    int phdr_size = phdr_count * sizeof(Elf64_Phdr);

    /* .text section (page-aligned for execution) */
    text_offset = ALIGN(phdr_offset + phdr_size, PAGE_SIZE);
    text_size = code_size;
    text_vaddr = text_offset; /* Will be 0-based after dlopen relocation */

    /* Data sections follow .text (page-aligned for separate permissions) */
    uint64_t data_start = ALIGN(text_offset + text_size, PAGE_SIZE);
    uint64_t data_vaddr = data_start;

    /* .eh_frame - DWARF CFI for stack unwinding
     * CIE (24 bytes) + single FDE covering all code (24 bytes) = 48 bytes */
    eh_frame_offset = data_start;
    eh_frame_size = 48;

    /* .eh_frame_hdr - sorted index for binary search
     * Header (12 bytes) + 1 entry (8 bytes) = 20 bytes */
    eh_frame_hdr_offset = eh_frame_offset + eh_frame_size;
    eh_frame_hdr_size = 20;

    /* .dynsym - minimal, NULL + _hl_jit_code symbol */
    dynsym_offset = ALIGN(eh_frame_hdr_offset + eh_frame_hdr_size, 8);
    dynsym_size = 2 * sizeof(Elf64_Sym); /* NULL + _hl_jit_code */

    /* .dynstr - contains "_hl_jit_code" for dlsym */
    dynstr_offset = dynsym_offset + dynsym_size;
    dynstr_size = 1 + 13; /* null byte + "_hl_jit_code\0" */

    /* .hash - SYSV hash table for dlsym
     * Format: nbucket, nchain, bucket[nbucket], chain[nchain]
     * For 2 symbols (NULL + _hl_jit_code) with 1 bucket:
     * nbucket=1, nchain=2, bucket[0]=1, chain[0]=0, chain[1]=0 */
    hash_offset = ALIGN(dynstr_offset + dynstr_size, 4);
    hash_size = 4 + 4 + 4 + 4 + 4; /* nbucket + nchain + bucket[1] + chain[2] = 20 bytes */

    /* .dynamic section */
    dynamic_offset = ALIGN(hash_offset + hash_size, 8);
    dynamic_size = 6 * sizeof(Elf64_Dyn); /* HASH, SYMTAB, STRTAB, STRSZ, SYMENT, NULL */
    dynamic_vaddr = dynamic_offset;

    /* End of data segment (for PT_LOAD) */
    uint64_t data_end = dynamic_offset + dynamic_size;
    uint64_t data_segment_size = data_end - data_start;

    /* .symtab - full symbol table for debuggers */
    symtab_offset = dynamic_offset + dynamic_size;
    symtab_size = (1 + num_symbols) * sizeof(Elf64_Sym); /* NULL + base + functions */

    /* .strtab */
    strtab_offset = symtab_offset + symtab_size;
    strtab_size = strtab.pos;

    /* .shstrtab */
    shstrtab_offset = strtab_offset + strtab_size;
    shstrtab_size = shstrtab.pos;

    /* Section headers at end */
    shdr_offset = ALIGN(shstrtab_offset + shstrtab_size, 8);

    /* ========== Write ELF header ========== */
    /* e_ident */
    elf_write(&w, "\x7f" "ELF", 4);           /* Magic */
    elf_write8(&w, ELFCLASS64);                /* 64-bit */
    elf_write8(&w, ELFDATA2LSB);               /* Little-endian */
    elf_write8(&w, EV_CURRENT);                /* Version */
    elf_write8(&w, 0);                         /* OS/ABI (NONE) */
    elf_write(&w, "\0\0\0\0\0\0\0\0", 8);      /* Padding */

    elf_write16(&w, ET_DYN);                   /* e_type: shared object */
    elf_write16(&w, EM_AARCH64);               /* e_machine */
    elf_write32(&w, EV_CURRENT);               /* e_version */
    elf_write64(&w, 0);                        /* e_entry (none for .so) */
    elf_write64(&w, phdr_offset);              /* e_phoff */
    elf_write64(&w, shdr_offset);              /* e_shoff */
    elf_write32(&w, 0);                        /* e_flags */
    elf_write16(&w, sizeof(Elf64_Ehdr));       /* e_ehsize */
    elf_write16(&w, sizeof(Elf64_Phdr));       /* e_phentsize */
    elf_write16(&w, phdr_count);               /* e_phnum */
    elf_write16(&w, sizeof(Elf64_Shdr));       /* e_shentsize */
    elf_write16(&w, SEC_COUNT);                /* e_shnum */
    elf_write16(&w, SEC_SHSTRTAB);             /* e_shstrndx */

    /* ========== Write program headers ========== */

    /* PT_LOAD for .text (executable) */
    elf_write32(&w, PT_LOAD);                  /* p_type */
    elf_write32(&w, PF_R | PF_X);              /* p_flags */
    elf_write64(&w, text_offset);              /* p_offset */
    elf_write64(&w, text_vaddr);               /* p_vaddr */
    elf_write64(&w, text_vaddr);               /* p_paddr */
    elf_write64(&w, text_size);                /* p_filesz */
    elf_write64(&w, text_size);                /* p_memsz */
    elf_write64(&w, PAGE_SIZE);                /* p_align */

    /* PT_LOAD for data (.eh_frame, .eh_frame_hdr, .dynsym, .dynstr, .dynamic) */
    elf_write32(&w, PT_LOAD);                  /* p_type */
    elf_write32(&w, PF_R | PF_W);              /* p_flags */
    elf_write64(&w, data_start);               /* p_offset */
    elf_write64(&w, data_vaddr);               /* p_vaddr */
    elf_write64(&w, data_vaddr);               /* p_paddr */
    elf_write64(&w, data_segment_size);        /* p_filesz */
    elf_write64(&w, data_segment_size);        /* p_memsz */
    elf_write64(&w, PAGE_SIZE);                /* p_align */

    /* PT_DYNAMIC */
    elf_write32(&w, PT_DYNAMIC);               /* p_type */
    elf_write32(&w, PF_R | PF_W);              /* p_flags */
    elf_write64(&w, dynamic_offset);           /* p_offset */
    elf_write64(&w, dynamic_vaddr);            /* p_vaddr */
    elf_write64(&w, dynamic_vaddr);            /* p_paddr */
    elf_write64(&w, dynamic_size);             /* p_filesz */
    elf_write64(&w, dynamic_size);             /* p_memsz */
    elf_write64(&w, 8);                        /* p_align */

    /* PT_GNU_EH_FRAME - points to .eh_frame_hdr for unwinder */
    elf_write32(&w, PT_GNU_EH_FRAME);          /* p_type */
    elf_write32(&w, PF_R);                     /* p_flags */
    elf_write64(&w, eh_frame_hdr_offset);      /* p_offset */
    elf_write64(&w, eh_frame_hdr_offset);      /* p_vaddr */
    elf_write64(&w, eh_frame_hdr_offset);      /* p_paddr */
    elf_write64(&w, eh_frame_hdr_size);        /* p_filesz */
    elf_write64(&w, eh_frame_hdr_size);        /* p_memsz */
    elf_write64(&w, 4);                        /* p_align */

    /* ========== Write .text section ========== */
    elf_pad(&w, PAGE_SIZE);
    if (w.pos != (int)text_offset) {
        fprintf(stderr, "ELF: text offset mismatch: %d != %d\n", w.pos, (int)text_offset);
    }
    elf_write(&w, code, code_size);

    /* ========== Write .eh_frame ========== */
    elf_pad(&w, PAGE_SIZE);
    if (w.pos != (int)eh_frame_offset) {
        fprintf(stderr, "ELF: eh_frame offset mismatch: %d != %d\n", w.pos, (int)eh_frame_offset);
    }
    {
        int cie_start = w.pos;

        /* CIE (Common Information Entry)
         * Content: CIE_id(4) + version(1) + aug(1) + code_align(1) + data_align(1) +
         *          return_reg(1) + instructions(3) = 12 bytes */
        elf_write32(&w, 12);                       /* length = 12 bytes of content */
        elf_write32(&w, 0);                        /* CIE_id = 0 (marks this as CIE) */
        elf_write8(&w, 1);                         /* version */
        elf_write8(&w, 0);                         /* augmentation string (empty) */
        elf_write8(&w, 4);                         /* code_alignment_factor (ULEB128) = 4 */
        elf_write8(&w, 0x78);                      /* data_alignment_factor (SLEB128) = -8 */
        elf_write8(&w, DW_REG_X30);                /* return_address_register (ULEB128) = X30 */
        /* Initial instructions: DW_CFA_def_cfa SP, 0 */
        elf_write8(&w, DW_CFA_def_cfa);
        elf_write8(&w, DW_REG_SP);                 /* register = SP */
        elf_write8(&w, 0);                         /* offset = 0 */
        /* Already 4-byte aligned: 4 (length) + 12 (content) = 16 bytes */

        int fde_start = w.pos;
        (void)fde_start;  /* Used conceptually for alignment verification */

        /* FDE (Frame Description Entry) - single FDE for all code
         * Content: CIE_ptr(4) + init_loc(8) + range(8) + instructions(8) = 28 bytes */
        elf_write32(&w, 28);                       /* length = 28 bytes of content */
        elf_write32(&w, w.pos - cie_start);        /* CIE_pointer (offset back to CIE) */
        elf_write64(&w, text_vaddr);               /* initial_location (code start) */
        elf_write64(&w, text_size);                /* address_range (code size) */
        /* Instructions: describe frame after prologue */
        /* DW_CFA_def_cfa X29, JIT_FRAME_SIZE (160 needs 2-byte ULEB128) */
        elf_write8(&w, DW_CFA_def_cfa);
        elf_write8(&w, DW_REG_X29);                /* register = X29 (FP) */
        elf_write8(&w, 0xa0);                      /* 160 low 7 bits with continuation */
        elf_write8(&w, 0x01);                      /* 160 high bits */
        /* DW_CFA_offset X29, JIT_FRAME_SIZE/8 (X29 at CFA-160) */
        elf_write8(&w, DW_CFA_offset | DW_REG_X29);
        elf_write8(&w, JIT_FRAME_SIZE / 8);        /* offset factor = 20 (20 * -8 = -160) */
        /* DW_CFA_offset X30, (JIT_FRAME_SIZE-8)/8 (X30 at CFA-152) */
        elf_write8(&w, DW_CFA_offset | DW_REG_X30);
        elf_write8(&w, (JIT_FRAME_SIZE - 8) / 8);  /* offset factor = 19 (19 * -8 = -152) */
        /* Already 4-byte aligned: 4 (length) + 28 (content) = 32 bytes */
    }

    /* ========== Write .eh_frame_hdr ========== */
    if (w.pos != (int)eh_frame_hdr_offset) {
        fprintf(stderr, "ELF: eh_frame_hdr offset mismatch: %d != %d\n", w.pos, (int)eh_frame_hdr_offset);
    }
    {
        /* .eh_frame_hdr format:
         * version (1 byte) = 1
         * eh_frame_ptr_enc (1 byte) = DW_EH_PE_pcrel | DW_EH_PE_sdata4
         * fde_count_enc (1 byte) = DW_EH_PE_udata4
         * table_enc (1 byte) = DW_EH_PE_pcrel | DW_EH_PE_sdata4
         * eh_frame_ptr (4 bytes) = offset to .eh_frame from this location
         * fde_count (4 bytes) = 1
         * table[0] = (initial_loc, fde_ptr) */
        elf_write8(&w, 1);                         /* version */
        elf_write8(&w, DW_EH_PE_pcrel | DW_EH_PE_sdata4);  /* eh_frame_ptr encoding */
        elf_write8(&w, DW_EH_PE_udata4);           /* fde_count encoding */
        elf_write8(&w, DW_EH_PE_pcrel | DW_EH_PE_sdata4);  /* table encoding */
        /* eh_frame_ptr: PC-relative offset to .eh_frame from current position */
        elf_write32(&w, (int32_t)(eh_frame_offset - w.pos));
        /* fde_count */
        elf_write32(&w, 1);
        /* table entry: (initial_loc, fde_ptr) both PC-relative */
        elf_write32(&w, (int32_t)(text_vaddr - w.pos));      /* initial_loc */
        elf_write32(&w, (int32_t)(eh_frame_offset + 24 - w.pos));  /* fde_ptr (CIE is 24 bytes) */
    }

    /* ========== Write .dynsym ========== */
    elf_pad(&w, 8);
    if (w.pos != (int)dynsym_offset) {
        fprintf(stderr, "ELF: dynsym offset mismatch: %d != %d\n", w.pos, (int)dynsym_offset);
    }
    /* NULL symbol (index 0) */
    for (i = 0; i < (int)sizeof(Elf64_Sym); i++) elf_write8(&w, 0);
    /* _hl_jit_code symbol (index 1) */
    elf_write32(&w, 1);                        /* st_name: offset 1 in .dynstr */
    elf_write8(&w, ELF64_ST_INFO(STB_GLOBAL, STT_FUNC));
    elf_write8(&w, 0);                         /* st_other */
    elf_write16(&w, SEC_TEXT);                 /* st_shndx */
    elf_write64(&w, text_vaddr);               /* st_value */
    elf_write64(&w, text_size);                /* st_size */

    /* ========== Write .dynstr ========== */
    elf_write8(&w, 0);                         /* null string at offset 0 */
    elf_write(&w, "_hl_jit_code", 13);         /* symbol name at offset 1 */

    /* ========== Write .hash ========== */
    elf_pad(&w, 4);
    if (w.pos != (int)hash_offset) {
        fprintf(stderr, "ELF: hash offset mismatch: %d != %d\n", w.pos, (int)hash_offset);
    }
    /* SYSV hash table: nbucket, nchain, bucket[], chain[] */
    elf_write32(&w, 1);                        /* nbucket = 1 */
    elf_write32(&w, 2);                        /* nchain = 2 (NULL + _hl_jit_code) */
    elf_write32(&w, 1);                        /* bucket[0] = 1 (index of _hl_jit_code) */
    elf_write32(&w, 0);                        /* chain[0] = 0 (NULL symbol, no chain) */
    elf_write32(&w, 0);                        /* chain[1] = 0 (_hl_jit_code, end of chain) */

    /* ========== Write .dynamic ========== */
    elf_pad(&w, 8);
    if (w.pos != (int)dynamic_offset) {
        fprintf(stderr, "ELF: dynamic offset mismatch: %d != %d\n", w.pos, (int)dynamic_offset);
    }
    /* DT_HASH */
    elf_write64(&w, DT_HASH);
    elf_write64(&w, hash_offset);
    /* DT_SYMTAB */
    elf_write64(&w, DT_SYMTAB);
    elf_write64(&w, dynsym_offset);
    /* DT_STRTAB */
    elf_write64(&w, DT_STRTAB);
    elf_write64(&w, dynstr_offset);
    /* DT_STRSZ */
    elf_write64(&w, DT_STRSZ);
    elf_write64(&w, dynstr_size);
    /* DT_SYMENT */
    elf_write64(&w, DT_SYMENT);
    elf_write64(&w, sizeof(Elf64_Sym));
    /* DT_NULL */
    elf_write64(&w, DT_NULL);
    elf_write64(&w, 0);

    /* ========== Write .symtab (full symbols for debuggers) ========== */
    if (w.pos != (int)symtab_offset) {
        fprintf(stderr, "ELF: symtab offset mismatch: %d != %d\n", w.pos, (int)symtab_offset);
    }

    /* NULL symbol */
    for (i = 0; i < (int)sizeof(Elf64_Sym); i++) elf_write8(&w, 0);

    /* _hl_jit_code base symbol */
    elf_write32(&w, base_sym_name);            /* st_name */
    elf_write8(&w, ELF64_ST_INFO(STB_GLOBAL, STT_FUNC));
    elf_write8(&w, 0);                         /* st_other */
    elf_write16(&w, SEC_TEXT);                 /* st_shndx */
    elf_write64(&w, text_vaddr);               /* st_value */
    elf_write64(&w, text_size);                /* st_size */

    /* Function symbols */
    for (i = 0; i < num_functions; i++) {
        hl_function *f = &m->code->functions[i];
        hl_debug_infos *dbg = ctx->debug ? &ctx->debug[i] : NULL;

        uint64_t func_addr = text_vaddr;
        uint64_t func_size = 0;

        if (dbg && dbg->offsets) {
            func_addr = text_vaddr + dbg->start;
            func_size = dbg->large ?
                ((int *)dbg->offsets)[f->nops] :
                ((unsigned short *)dbg->offsets)[f->nops];
        }

        elf_write32(&w, sym_names[i]);         /* st_name */
        elf_write8(&w, ELF64_ST_INFO(STB_GLOBAL, STT_FUNC));
        elf_write8(&w, 0);                     /* st_other */
        elf_write16(&w, SEC_TEXT);             /* st_shndx */
        elf_write64(&w, func_addr);            /* st_value */
        elf_write64(&w, func_size);            /* st_size */
    }

    /* ========== Write .strtab ========== */
    if (w.pos != (int)strtab_offset) {
        fprintf(stderr, "ELF: strtab offset mismatch: %d != %d\n", w.pos, (int)strtab_offset);
    }
    elf_write(&w, strtab.buf, strtab.pos);

    /* ========== Write .shstrtab ========== */
    if (w.pos != (int)shstrtab_offset) {
        fprintf(stderr, "ELF: shstrtab offset mismatch: %d != %d\n", w.pos, (int)shstrtab_offset);
    }
    elf_write(&w, shstrtab.buf, shstrtab.pos);

    /* ========== Write section headers ========== */
    elf_pad(&w, 8);
    if (w.pos != (int)shdr_offset) {
        fprintf(stderr, "ELF: shdr offset mismatch: %d != %d\n", w.pos, (int)shdr_offset);
    }

    /* SEC_NULL */
    for (i = 0; i < (int)sizeof(Elf64_Shdr); i++) elf_write8(&w, 0);

    /* SEC_TEXT */
    elf_write32(&w, shstr_text);               /* sh_name */
    elf_write32(&w, SHT_PROGBITS);             /* sh_type */
    elf_write64(&w, SHF_ALLOC | SHF_EXECINSTR);/* sh_flags */
    elf_write64(&w, text_vaddr);               /* sh_addr */
    elf_write64(&w, text_offset);              /* sh_offset */
    elf_write64(&w, text_size);                /* sh_size */
    elf_write32(&w, 0);                        /* sh_link */
    elf_write32(&w, 0);                        /* sh_info */
    elf_write64(&w, PAGE_SIZE);                /* sh_addralign */
    elf_write64(&w, 0);                        /* sh_entsize */

    /* SEC_EH_FRAME */
    elf_write32(&w, shstr_eh_frame);           /* sh_name */
    elf_write32(&w, SHT_PROGBITS);             /* sh_type */
    elf_write64(&w, SHF_ALLOC);                /* sh_flags */
    elf_write64(&w, eh_frame_offset);          /* sh_addr */
    elf_write64(&w, eh_frame_offset);          /* sh_offset */
    elf_write64(&w, eh_frame_size);            /* sh_size */
    elf_write32(&w, 0);                        /* sh_link */
    elf_write32(&w, 0);                        /* sh_info */
    elf_write64(&w, 8);                        /* sh_addralign */
    elf_write64(&w, 0);                        /* sh_entsize */

    /* SEC_EH_FRAME_HDR */
    elf_write32(&w, shstr_eh_frame_hdr);       /* sh_name */
    elf_write32(&w, SHT_PROGBITS);             /* sh_type */
    elf_write64(&w, SHF_ALLOC);                /* sh_flags */
    elf_write64(&w, eh_frame_hdr_offset);      /* sh_addr */
    elf_write64(&w, eh_frame_hdr_offset);      /* sh_offset */
    elf_write64(&w, eh_frame_hdr_size);        /* sh_size */
    elf_write32(&w, 0);                        /* sh_link */
    elf_write32(&w, 0);                        /* sh_info */
    elf_write64(&w, 4);                        /* sh_addralign */
    elf_write64(&w, 0);                        /* sh_entsize */

    /* SEC_HASH */
    elf_write32(&w, shstr_hash);               /* sh_name */
    elf_write32(&w, SHT_HASH);                 /* sh_type */
    elf_write64(&w, SHF_ALLOC);                /* sh_flags */
    elf_write64(&w, hash_offset);              /* sh_addr */
    elf_write64(&w, hash_offset);              /* sh_offset */
    elf_write64(&w, hash_size);                /* sh_size */
    elf_write32(&w, SEC_DYNSYM);               /* sh_link -> .dynsym */
    elf_write32(&w, 0);                        /* sh_info */
    elf_write64(&w, 4);                        /* sh_addralign */
    elf_write64(&w, 4);                        /* sh_entsize */

    /* SEC_DYNAMIC */
    elf_write32(&w, shstr_dynamic);
    elf_write32(&w, SHT_DYNAMIC);
    elf_write64(&w, SHF_ALLOC | SHF_WRITE);
    elf_write64(&w, dynamic_vaddr);
    elf_write64(&w, dynamic_offset);
    elf_write64(&w, dynamic_size);
    elf_write32(&w, SEC_DYNSTR);               /* sh_link -> .dynstr */
    elf_write32(&w, 0);
    elf_write64(&w, 8);
    elf_write64(&w, sizeof(Elf64_Dyn));

    /* SEC_DYNSYM */
    elf_write32(&w, shstr_dynsym);
    elf_write32(&w, SHT_DYNSYM);
    elf_write64(&w, SHF_ALLOC);
    elf_write64(&w, dynsym_offset);            /* sh_addr = offset for simplicity */
    elf_write64(&w, dynsym_offset);
    elf_write64(&w, dynsym_size);
    elf_write32(&w, SEC_DYNSTR);               /* sh_link -> .dynstr */
    elf_write32(&w, 1);                        /* sh_info: first non-local */
    elf_write64(&w, 8);
    elf_write64(&w, sizeof(Elf64_Sym));

    /* SEC_DYNSTR */
    elf_write32(&w, shstr_dynstr);
    elf_write32(&w, SHT_STRTAB);
    elf_write64(&w, SHF_ALLOC);
    elf_write64(&w, dynstr_offset);
    elf_write64(&w, dynstr_offset);
    elf_write64(&w, dynstr_size);
    elf_write32(&w, 0);
    elf_write32(&w, 0);
    elf_write64(&w, 1);
    elf_write64(&w, 0);

    /* SEC_SYMTAB */
    elf_write32(&w, shstr_symtab);
    elf_write32(&w, SHT_SYMTAB);
    elf_write64(&w, 0);                        /* Not loaded */
    elf_write64(&w, 0);
    elf_write64(&w, symtab_offset);
    elf_write64(&w, symtab_size);
    elf_write32(&w, SEC_STRTAB);               /* sh_link -> .strtab */
    elf_write32(&w, 1);                        /* sh_info: first non-local */
    elf_write64(&w, 8);
    elf_write64(&w, sizeof(Elf64_Sym));

    /* SEC_STRTAB */
    elf_write32(&w, shstr_strtab);
    elf_write32(&w, SHT_STRTAB);
    elf_write64(&w, 0);
    elf_write64(&w, 0);
    elf_write64(&w, strtab_offset);
    elf_write64(&w, strtab_size);
    elf_write32(&w, 0);
    elf_write32(&w, 0);
    elf_write64(&w, 1);
    elf_write64(&w, 0);

    /* SEC_SHSTRTAB */
    elf_write32(&w, shstr_shstrtab);
    elf_write32(&w, SHT_STRTAB);
    elf_write64(&w, 0);
    elf_write64(&w, 0);
    elf_write64(&w, shstrtab_offset);
    elf_write64(&w, shstrtab_size);
    elf_write32(&w, 0);
    elf_write32(&w, 0);
    elf_write64(&w, 1);
    elf_write64(&w, 0);

    /* ========== Write file ========== */
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "ELF: Failed to create %s\n", path);
        free(sym_names);
        elf_free(&w);
        elf_free(&strtab);
        elf_free(&shstrtab);
        return 0;
    }

    if (fwrite(w.buf, 1, w.pos, fp) != (size_t)w.pos) {
        fprintf(stderr, "ELF: Failed to write %s\n", path);
        fclose(fp);
        free(sym_names);
        elf_free(&w);
        elf_free(&strtab);
        elf_free(&shstrtab);
        return 0;
    }

    fclose(fp);
    free(sym_names);
    elf_free(&w);
    elf_free(&strtab);
    elf_free(&shstrtab);

    return 1;
}
