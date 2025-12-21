/*
 * JIT ELF Generation for Debug/Profiler Support
 *
 * When HL_JIT_DEBUG=1 is set, generates an ELF shared object containing
 * the JIT-compiled code with debug symbols. This allows profilers like
 * heaptrack, perf, and debuggers like GDB to show proper function names.
 *
 * NOTE: This header must be included AFTER jit_common.h which provides
 * the necessary type definitions (jit_ctx, hl_module, etc.)
 */

#ifndef JIT_ELF_H
#define JIT_ELF_H

#include <stdint.h>

/*
 * Write JIT code as an ELF shared object.
 *
 * Parameters:
 *   path      - Output file path (e.g., "/tmp/hl-jit-12345.so")
 *   ctx       - JIT context with debug info
 *   m         - HashLink module with function metadata
 *   code_size - Size of generated code in bytes
 *   code      - Pointer to generated machine code
 *
 * Returns:
 *   1 on success, 0 on failure
 */
int write_jit_elf(const char *path, jit_ctx *ctx, hl_module *m,
                  int code_size, unsigned char *code);

/* ELF constants for AArch64 */
#define ELF_MAGIC       "\x7f" "ELF"
#define ELFCLASS64      2
#define ELFDATA2LSB     1       /* Little-endian */
#define EV_CURRENT      1
#define ET_DYN          3       /* Shared object */
#define EM_AARCH64      183

/* Program header types */
#define PT_NULL         0
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_GNU_EH_FRAME 0x6474e550

/* Program header flags */
#define PF_X            1       /* Execute */
#define PF_W            2       /* Write */
#define PF_R            4       /* Read */

/* Section header types */
#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_HASH        5
#define SHT_DYNAMIC     6
#define SHT_DYNSYM      11

/* Section header flags */
#define SHF_WRITE       1
#define SHF_ALLOC       2
#define SHF_EXECINSTR   4

/* Dynamic section tags */
#define DT_NULL         0
#define DT_HASH         4
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_STRSZ        10
#define DT_SYMENT       11

/* Symbol binding/type */
#define STB_LOCAL       0
#define STB_GLOBAL      1
#define STT_NOTYPE      0
#define STT_FUNC        2

#define ELF64_ST_INFO(bind, type) (((bind) << 4) | ((type) & 0xf))

/* DWARF Call Frame Information (CFI) opcodes */
#define DW_CFA_nop              0x00
#define DW_CFA_advance_loc1     0x02
#define DW_CFA_advance_loc2     0x03
#define DW_CFA_advance_loc4     0x04
#define DW_CFA_offset_extended  0x05
#define DW_CFA_def_cfa          0x0c
#define DW_CFA_def_cfa_register 0x0d
#define DW_CFA_def_cfa_offset   0x0e
#define DW_CFA_offset           0x80  /* High 2 bits = 10, low 6 bits = register */

/* DWARF register numbers for AArch64 */
#define DW_REG_X29      29  /* Frame pointer */
#define DW_REG_X30      30  /* Link register (return address) */
#define DW_REG_SP       31  /* Stack pointer */

/* .eh_frame_hdr encodings */
#define DW_EH_PE_omit       0xff
#define DW_EH_PE_absptr     0x00
#define DW_EH_PE_udata4     0x03
#define DW_EH_PE_sdata4     0x0b
#define DW_EH_PE_pcrel      0x10
#define DW_EH_PE_datarel    0x30

/* Special section indices */
#define SHN_UNDEF       0
#define SHN_ABS         0xfff1

/* ELF64 structures */
typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} Elf64_Shdr;

typedef struct {
    uint32_t st_name;
    unsigned char st_info;
    unsigned char st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym;

typedef struct {
    int64_t d_tag;
    uint64_t d_val;
} Elf64_Dyn;

#endif /* JIT_ELF_H */
