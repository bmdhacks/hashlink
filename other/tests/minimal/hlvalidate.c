/*
 * HashLink Bytecode Validator
 * Validates .hl files for correctness and suspicious patterns
 */
#include <hl.h>
#include <hlmodule.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <signal.h>
#include <setjmp.h>

/* For recovering from invalid pointer access */
static sigjmp_buf recovery_point;
static volatile sig_atomic_t in_risky_access = 0;

static void segv_handler(int sig) {
    (void)sig;
    if (in_risky_access) {
        siglongjmp(recovery_point, 1);
    }
    /* If not in risky access, re-raise to get normal crash behavior */
    signal(SIGSEGV, SIG_DFL);
    raise(SIGSEGV);
}

/* Check if a pointer looks valid (heuristic) */
static int is_valid_ptr(const void *ptr) {
    if (!ptr) return 0;
    uintptr_t addr = (uintptr_t)ptr;
    /* Reject very low addresses (likely garbage or small integers cast to pointers) */
    if (addr < 0x1000) return 0;
    return 1;
}

/* Safely read an int from a potentially invalid pointer */
static int safe_read_int(int *ptr, int *out_value) {
    if (!is_valid_ptr(ptr)) {
        return 0;
    }

    in_risky_access = 1;
    if (sigsetjmp(recovery_point, 1) == 0) {
        *out_value = *ptr;
        in_risky_access = 0;
        return 1;  /* success */
    } else {
        in_risky_access = 0;
        return 0;  /* failed - invalid pointer */
    }
}

/* ============================================================================
 * Opcode metadata
 * ============================================================================ */

static const char *opcode_names[] = {
    "OMov", "OInt", "OFloat", "OBool", "OBytes", "OString", "ONull",
    "OAdd", "OSub", "OMul", "OSDiv", "OUDiv", "OSMod", "OUMod",
    "OShl", "OSShr", "OUShr", "OAnd", "OOr", "OXor",
    "ONeg", "ONot", "OIncr", "ODecr",
    "OCall0", "OCall1", "OCall2", "OCall3", "OCall4", "OCallN", "OCallMethod", "OCallThis", "OCallClosure",
    "OStaticClosure", "OInstanceClosure", "OVirtualClosure",
    "OGetGlobal", "OSetGlobal",
    "OField", "OSetField", "OGetThis", "OSetThis",
    "ODynGet", "ODynSet",
    "OJTrue", "OJFalse", "OJNull", "OJNotNull", "OJSLt", "OJSGte", "OJSGt", "OJSLte", "OJULt", "OJUGte", "OJNotLt", "OJNotGte", "OJEq", "OJNotEq", "OJAlways",
    "OToDyn", "OToSFloat", "OToUFloat", "OToInt", "OSafeCast", "OUnsafeCast", "OToVirtual",
    "OLabel", "ORet", "OThrow", "ORethrow", "OSwitch", "ONullCheck", "OTrap", "OEndTrap",
    "OGetI8", "OGetI16", "OGetMem", "OGetArray", "OSetI8", "OSetI16", "OSetMem", "OSetArray",
    "ONew", "OArraySize", "OType", "OGetType", "OGetTID",
    "ORef", "OUnref", "OSetref",
    "OMakeEnum", "OEnumAlloc", "OEnumIndex", "OEnumField", "OSetEnumField",
    "OAssert", "ORefData", "ORefOffset",
    "ONop", "OPrefetch", "OAsm", "OCatch"
};

/* Number of base parameters per opcode (not counting extra[]) */
/* -1 means variable length with extra[] */
static const int opcode_nargs[] = {
    2, 2, 2, 2, 2, 2, 1,       /* OMov..ONull */
    3, 3, 3, 3, 3, 3, 3,       /* OAdd..OUMod */
    3, 3, 3, 3, 3, 3,          /* OShl..OXor */
    2, 2, 1, 1,                /* ONeg..ODecr */
    2, 3, 4, 5, 6, -1, -1, -1, -1, /* OCall0..OCallClosure */
    2, 3, 3,                   /* OStaticClosure..OVirtualClosure */
    2, 2,                      /* OGetGlobal, OSetGlobal */
    3, 3, 2, 2,                /* OField..OSetThis */
    3, 3,                      /* ODynGet, ODynSet */
    2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1, /* OJTrue..OJAlways */
    2, 2, 2, 2, 2, 2, 2,       /* OToDyn..OToVirtual */
    0, 1, 1, 1, -1, 1, 2, 1,   /* OLabel..OEndTrap */
    3, 3, 3, 3, 3, 3, 3, 3,    /* OGetI8..OSetArray */
    1, 2, 2, 2, 2,             /* ONew..OGetTID */
    2, 2, 2,                   /* ORef..OSetref */
    -1, 2, 2, 4, 3,            /* OMakeEnum..OSetEnumField */
    0, 2, 3, 0, 3, 3, 1        /* OAssert..OCatch */
};

/* ============================================================================
 * Byte offset tracking - lightweight bytecode scanner
 * ============================================================================ */

typedef struct {
    const unsigned char *b;
    int size;
    int pos;
} offset_scanner;

/* Mirror hl_read_index from code.c to skip variable-length indices */
static int scan_skip_index(offset_scanner *s) {
    if (s->pos >= s->size) return -1;
    unsigned char b = s->b[s->pos++];
    if ((b & 0x80) == 0) return s->pos;
    if ((b & 0x40) == 0) {
        s->pos++;  /* 2-byte index */
        return s->pos;
    }
    s->pos += 3;  /* 4-byte index */
    return s->pos;
}

static int scan_read_index(offset_scanner *s) {
    if (s->pos >= s->size) return 0;
    unsigned char b = s->b[s->pos++];
    if ((b & 0x80) == 0) return b & 0x7F;
    if ((b & 0x40) == 0) {
        int v = s->b[s->pos++] | ((b & 31) << 8);
        return (b & 0x20) == 0 ? v : -v;
    }
    int c = s->b[s->pos++];
    int d = s->b[s->pos++];
    int e = s->b[s->pos++];
    int v = ((b & 31) << 24) | (c << 16) | (d << 8) | e;
    return (b & 0x20) == 0 ? v : -v;
}

static int scan_read_uindex(offset_scanner *s) {
    int i = scan_read_index(s);
    return i < 0 ? 0 : i;
}

static void scan_skip_bytes(offset_scanner *s, int n) {
    s->pos += n;
    if (s->pos > s->size) s->pos = s->size;
}

static int scan_read_i32(offset_scanner *s) {
    if (s->pos + 4 > s->size) return 0;
    int v = s->b[s->pos] | (s->b[s->pos+1]<<8) | (s->b[s->pos+2]<<16) | (s->b[s->pos+3]<<24);
    s->pos += 4;
    return v;
}

/* Skip a type definition */
static void scan_skip_type(offset_scanner *s) {
    if (s->pos >= s->size) return;
    int kind = s->b[s->pos++];
    switch (kind) {
    case HFUN:
    case HMETHOD: {
        int nargs = s->b[s->pos++];
        for (int i = 0; i < nargs; i++) scan_skip_index(s);
        scan_skip_index(s);  /* ret */
        break;
    }
    case HOBJ:
    case HSTRUCT: {
        scan_skip_index(s);  /* name string */
        scan_skip_index(s);  /* super */
        scan_skip_index(s);  /* global */
        int nfields = scan_read_uindex(s);
        int nproto = scan_read_uindex(s);
        int nbindings = scan_read_uindex(s);
        for (int i = 0; i < nfields; i++) {
            scan_skip_index(s);  /* name */
            scan_skip_index(s);  /* type */
        }
        for (int i = 0; i < nproto; i++) {
            scan_skip_index(s);  /* name */
            scan_skip_index(s);  /* findex */
            scan_skip_index(s);  /* pindex */
        }
        for (int i = 0; i < nbindings; i++) {
            scan_skip_index(s);  /* field */
            scan_skip_index(s);  /* findex */
        }
        break;
    }
    case HREF:
    case HNULL:
    case HPACKED:
        scan_skip_index(s);
        break;
    case HVIRTUAL: {
        int nfields = scan_read_uindex(s);
        for (int i = 0; i < nfields; i++) {
            scan_skip_index(s);  /* name */
            scan_skip_index(s);  /* type */
        }
        break;
    }
    case HABSTRACT:
        scan_skip_index(s);  /* name */
        break;
    case HENUM: {
        scan_skip_index(s);  /* name */
        scan_skip_index(s);  /* global */
        int nconstructs = scan_read_uindex(s);
        for (int i = 0; i < nconstructs; i++) {
            scan_skip_index(s);  /* name */
            int nparams = scan_read_uindex(s);
            for (int j = 0; j < nparams; j++)
                scan_skip_index(s);  /* param type */
        }
        break;
    }
    default:
        break;
    }
}

/* Skip strings section */
static void scan_skip_strings(offset_scanner *s, int nstrings) {
    int size = scan_read_i32(s);
    scan_skip_bytes(s, size);
    for (int i = 0; i < nstrings; i++)
        scan_skip_index(s);  /* string length */
}

/* Number of base parameters per opcode (from opcodes.h) */
static const int scan_op_nargs[] = {
    2, 2, 2, 2, 2, 2, 1,       /* OMov..ONull */
    3, 3, 3, 3, 3, 3, 3,       /* OAdd..OUMod */
    3, 3, 3, 3, 3, 3,          /* OShl..OXor */
    2, 2, 1, 1,                /* ONeg..ODecr */
    2, 3, 4, 5, 6, -1, -1, -1, -1, /* OCall0..OCallClosure */
    2, 3, 3,                   /* OStaticClosure..OVirtualClosure */
    2, 2,                      /* OGetGlobal, OSetGlobal */
    3, 3, 2, 2,                /* OField..OSetThis */
    3, 3,                      /* ODynGet, ODynSet */
    2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1, /* OJTrue..OJAlways */
    2, 2, 2, 2, 2, 2, 2,       /* OToDyn..OToVirtual */
    0, 1, 1, 1, -1, 1, 2, 1,   /* OLabel..OEndTrap */
    3, 3, 3, 3, 3, 3, 3, 3,    /* OGetI8..OSetArray */
    1, 2, 2, 2, 2,             /* ONew..OGetTID */
    2, 2, 2,                   /* ORef..OSetref */
    -1, 2, 2, 4, 3,            /* OMakeEnum..OSetEnumField */
    0, 2, 3, 0, 3, 3, 1        /* OAssert..OCatch */
};

/* Skip debug info section for a function.
 * Mirrors hl_read_debug_infos from code.c */
static void scan_skip_debug_info(offset_scanner *s, int nops, int version) {
    int i = 0;
    while (i < nops && s->pos < s->size) {
        int c = s->b[s->pos++];
        if (c & 1) {
            /* File reference: reads 1 more byte, does NOT increment i */
            s->pos++;
        } else if (c & 2) {
            /* Repeat: increments i by count, no extra bytes */
            int count = (c >> 2) & 15;
            i += count;
        } else if (c & 4) {
            /* Small line delta: no extra bytes, increments i */
            i++;
        } else {
            /* Large line number: reads 2 more bytes, increments i */
            s->pos += 2;
            i++;
        }
    }
    if (version >= 3) {
        int nassigns = scan_read_uindex(s);
        for (int j = 0; j < nassigns; j++) {
            scan_skip_index(s);
            scan_skip_index(s);
        }
    }
}

/* Scan an opcode and return its byte length */
static int scan_opcode(offset_scanner *s, int *opcode_pos) {
    *opcode_pos = s->pos;
    if (s->pos >= s->size) return 0;
    int op = s->b[s->pos++];
    if (op >= OLast) return s->pos - *opcode_pos;

    int nargs = scan_op_nargs[op];
    switch (nargs) {
    case 0: break;
    case 1: scan_skip_index(s); break;
    case 2: scan_skip_index(s); scan_skip_index(s); break;
    case 3: scan_skip_index(s); scan_skip_index(s); scan_skip_index(s); break;
    case 4: scan_skip_index(s); scan_skip_index(s); scan_skip_index(s); scan_skip_index(s); break;
    case -1:
        switch (op) {
        case OCallN: case OCallClosure: case OCallMethod: case OCallThis: case OMakeEnum: {
            scan_skip_index(s);
            scan_skip_index(s);
            int nextra = s->b[s->pos++];
            for (int i = 0; i < nextra; i++) scan_skip_index(s);
            break;
        }
        case OSwitch: {
            scan_skip_index(s);
            int ncases = scan_read_uindex(s);
            for (int i = 0; i < ncases; i++) scan_skip_index(s);
            scan_skip_index(s);  /* default */
            break;
        }
        default: break;
        }
        break;
    default: {
        int nextra = nargs - 3;
        scan_skip_index(s); scan_skip_index(s); scan_skip_index(s);
        for (int i = 0; i < nextra; i++) scan_skip_index(s);
        break;
    }
    }
    return s->pos - *opcode_pos;
}

/* Build offset tables for a bytecode file.
 * Returns 1 on success, 0 on failure.
 * Caller must free func_offsets, opcode_offsets[i], and func_nops arrays. */
static int build_offset_tables(const unsigned char *data, int size, hl_code *code,
                               int **out_func_offsets, int ***out_opcode_offsets,
                               int **out_func_nops) {
    offset_scanner _s = { data, size, 0 };
    offset_scanner *s = &_s;

    /* Skip header: "HLB" + version */
    if (size < 4) return 0;
    s->pos = 4;

    /* Skip flags and counts */
    scan_skip_index(s);  /* flags */
    int nints = scan_read_uindex(s);
    int nfloats = scan_read_uindex(s);
    int nstrings = scan_read_uindex(s);
    int nbytes = 0;
    if (code->version >= 5)
        nbytes = scan_read_uindex(s);
    int ntypes = scan_read_uindex(s);
    int nglobals = scan_read_uindex(s);
    int nnatives = scan_read_uindex(s);
    int nfunctions = scan_read_uindex(s);
    if (code->version >= 4)
        scan_skip_index(s);  /* nconstants */
    scan_skip_index(s);  /* entrypoint */

    /* Verify counts match */
    if (nfunctions != code->nfunctions) {
        fprintf(stderr, "Scanner: nfunctions mismatch: scanned %d, code has %d\n",
                nfunctions, code->nfunctions);
    }
    if (ntypes != code->ntypes) {
        fprintf(stderr, "Scanner: ntypes mismatch: scanned %d, code has %d\n",
                ntypes, code->ntypes);
    }

    /* Skip ints */
    scan_skip_bytes(s, nints * 4);

    /* Skip floats */
    scan_skip_bytes(s, nfloats * 8);

    /* Skip strings */
    scan_skip_strings(s, nstrings);

    /* Skip bytes (version >= 5) */
    if (code->version >= 5) {
        int bytes_size = scan_read_i32(s);
        scan_skip_bytes(s, bytes_size);
        for (int i = 0; i < nbytes; i++)
            scan_skip_index(s);
    }

    /* Skip debug files if present */
    if (code->hasdebug) {
        int ndebugfiles = scan_read_uindex(s);
        scan_skip_strings(s, ndebugfiles);
    }

    /* Skip types */
    for (int i = 0; i < ntypes; i++)
        scan_skip_type(s);

    /* Skip globals */
    for (int i = 0; i < nglobals; i++)
        scan_skip_index(s);

    /* Skip natives */
    for (int i = 0; i < nnatives; i++) {
        scan_skip_index(s);  /* lib */
        scan_skip_index(s);  /* name */
        scan_skip_index(s);  /* type */
        scan_skip_index(s);  /* findex */
    }

    /* Now we're at the functions section - record offsets */
    int *func_offsets = (int *)malloc(nfunctions * sizeof(int));
    int **opcode_offsets = (int **)malloc(nfunctions * sizeof(int *));
    int *func_nops = (int *)malloc(nfunctions * sizeof(int));
    if (!func_offsets || !opcode_offsets || !func_nops) {
        free(func_offsets);
        free(opcode_offsets);
        free(func_nops);
        return 0;
    }

    for (int fi = 0; fi < nfunctions; fi++) {
        func_offsets[fi] = s->pos;

        /* Read function header */
        scan_skip_index(s);  /* type */
        scan_skip_index(s);  /* findex */
        int nregs = scan_read_uindex(s);
        int nops = scan_read_uindex(s);
        func_nops[fi] = nops;  /* Store for validation */

        /* Skip register types */
        for (int i = 0; i < nregs; i++)
            scan_skip_index(s);

        /* Record opcode positions */
        opcode_offsets[fi] = (int *)malloc(nops * sizeof(int));
        if (!opcode_offsets[fi]) {
            /* Cleanup on failure */
            for (int j = 0; j < fi; j++)
                free(opcode_offsets[j]);
            free(func_offsets);
            free(opcode_offsets);
            free(func_nops);
            return 0;
        }

        for (int i = 0; i < nops; i++) {
            int op_pos;
            scan_opcode(s, &op_pos);
            opcode_offsets[fi][i] = op_pos;
        }

        /* Skip debug info if present */
        if (code->hasdebug) {
            scan_skip_debug_info(s, nops, code->version);
        }
    }

    *out_func_offsets = func_offsets;
    *out_opcode_offsets = opcode_offsets;
    *out_func_nops = func_nops;
    return 1;
}

/* Free offset tables */
static void free_offset_tables(int *func_offsets, int **opcode_offsets, int *func_nops, int nfunctions) {
    if (opcode_offsets) {
        for (int i = 0; i < nfunctions; i++)
            free(opcode_offsets[i]);
        free(opcode_offsets);
    }
    free(func_offsets);
    free(func_nops);
}

/* ============================================================================
 * Validator context
 * ============================================================================ */

typedef struct {
    hl_code *code;
    hl_function *f;
    int findex;
    int opindex;
    int error_count;
    int warning_count;
    int pedantic;
    int verbose;
    int single_func;  /* -1 for all, or specific function index */

    /* Byte offset tracking */
    int current_func_offset;   /* Byte offset of current function */
    int current_op_offset;     /* Byte offset of current opcode */
    int *func_offsets;         /* Byte offset of each function (by array index) */
    int **opcode_offsets;      /* Byte offset of each opcode within each function */
    int *func_nops;            /* Number of ops scanned for each function (for validation) */
} validator_ctx;

/* ============================================================================
 * Error/warning emission
 * ============================================================================ */

static void emit_error(validator_ctx *ctx, const char *fmt, ...) {
    va_list args;
    /* Print byte offset if available */
    if (ctx->current_op_offset > 0) {
        printf("ERROR: @0x%x F%d:%d: %s - ", ctx->current_op_offset, ctx->findex, ctx->opindex,
               ctx->f && ctx->opindex >= 0 && ctx->opindex < ctx->f->nops
               ? opcode_names[ctx->f->ops[ctx->opindex].op] : "?");
    } else {
        printf("ERROR: F%d:%d: %s - ", ctx->findex, ctx->opindex,
               ctx->f && ctx->opindex >= 0 && ctx->opindex < ctx->f->nops
               ? opcode_names[ctx->f->ops[ctx->opindex].op] : "?");
    }
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    ctx->error_count++;
}

static void emit_warning(validator_ctx *ctx, const char *fmt, ...) {
    if (!ctx->pedantic) return;
    va_list args;
    /* Print byte offset if available */
    if (ctx->current_op_offset > 0) {
        printf("WARNING: @0x%x F%d:%d: %s - ", ctx->current_op_offset, ctx->findex, ctx->opindex,
               ctx->f && ctx->opindex >= 0 && ctx->opindex < ctx->f->nops
               ? opcode_names[ctx->f->ops[ctx->opindex].op] : "?");
    } else {
        printf("WARNING: F%d:%d: %s - ", ctx->findex, ctx->opindex,
               ctx->f && ctx->opindex >= 0 && ctx->opindex < ctx->f->nops
               ? opcode_names[ctx->f->ops[ctx->opindex].op] : "?");
    }
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    ctx->warning_count++;
}

static void emit_global_error(validator_ctx *ctx, const char *fmt, ...) {
    va_list args;
    printf("ERROR: ");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    ctx->error_count++;
}

static void emit_global_warning(validator_ctx *ctx, const char *fmt, ...) {
    if (!ctx->pedantic) return;
    va_list args;
    printf("WARNING: ");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    ctx->warning_count++;
}

/* ============================================================================
 * Validation helpers
 * ============================================================================ */

static int validate_register(validator_ctx *ctx, int reg, const char *name) {
    if (reg < 0 || reg >= ctx->f->nregs) {
        emit_error(ctx, "%s register %d out of bounds (nregs=%d)", name, reg, ctx->f->nregs);
        return 0;
    }
    return 1;
}

/* Safely read extra[index] and validate as register, handling invalid pointers */
static int validate_extra_register(validator_ctx *ctx, int *extra, int index, const char *name) {
    int reg;
    if (!safe_read_int(&extra[index], &reg)) {
        emit_error(ctx, "invalid extra pointer - cannot read %s at extra[%d]", name, index);
        return 0;
    }
    return validate_register(ctx, reg, name);
}

/* Safely read extra[index], handling invalid pointers */
static int safe_read_extra(validator_ctx *ctx, int *extra, int index, int *out_value) {
    if (!safe_read_int(&extra[index], out_value)) {
        emit_error(ctx, "invalid extra pointer - cannot read extra[%d]", index);
        return 0;
    }
    return 1;
}

static int validate_int_index(validator_ctx *ctx, int idx) {
    if (idx < 0 || idx >= ctx->code->nints) {
        emit_error(ctx, "int constant index %d out of bounds (max=%d)", idx, ctx->code->nints);
        return 0;
    }
    return 1;
}

static int validate_float_index(validator_ctx *ctx, int idx) {
    if (idx < 0 || idx >= ctx->code->nfloats) {
        emit_error(ctx, "float constant index %d out of bounds (max=%d)", idx, ctx->code->nfloats);
        return 0;
    }
    return 1;
}

static int validate_string_index(validator_ctx *ctx, int idx) {
    if (idx < 0 || idx >= ctx->code->nstrings) {
        emit_error(ctx, "string constant index %d out of bounds (max=%d)", idx, ctx->code->nstrings);
        return 0;
    }
    return 1;
}

static int validate_bytes_index(validator_ctx *ctx, int idx) {
    if (idx < 0 || idx >= ctx->code->nbytes) {
        emit_error(ctx, "bytes constant index %d out of bounds (max=%d)", idx, ctx->code->nbytes);
        return 0;
    }
    return 1;
}

static int validate_type_index(validator_ctx *ctx, int idx) {
    if (idx < 0 || idx >= ctx->code->ntypes) {
        emit_error(ctx, "type index %d out of bounds (max=%d)", idx, ctx->code->ntypes);
        return 0;
    }
    return 1;
}

static int validate_global_index(validator_ctx *ctx, int idx) {
    if (idx < 0 || idx >= ctx->code->nglobals) {
        emit_error(ctx, "global index %d out of bounds (max=%d)", idx, ctx->code->nglobals);
        return 0;
    }
    return 1;
}

static int validate_function_index(validator_ctx *ctx, int idx) {
    int max_func = ctx->code->nfunctions + ctx->code->nnatives;
    if (idx < 0 || idx >= max_func) {
        emit_error(ctx, "function index %d out of bounds (max=%d)", idx, max_func);
        return 0;
    }
    return 1;
}

/* Validate jump target: offset is relative to (opindex + 1) */
static int validate_jump_target(validator_ctx *ctx, int offset) {
    int target = ctx->opindex + 1 + offset;
    /* Note: target == nops is valid (jump to end of function) */
    if (target < 0 || target > ctx->f->nops) {
        emit_error(ctx, "jump target %d out of bounds (nops=%d)", target, ctx->f->nops);
        return 0;
    }
    /* Pedantic: warn on suspicious jumps */
    if (ctx->pedantic) {
        if (offset == -1 || offset == 0) {
            emit_warning(ctx, "jump offset %d creates tight loop (target=%d)", offset, target);
        } else if (offset > 1000 || offset < -1000) {
            emit_warning(ctx, "suspiciously large jump offset %d", offset);
        }
    }
    return 1;
}

/* Forward declaration */
static const char *type_name(hl_type *t);

/* Format a type name into a buffer. Returns the buffer for convenience. */
static char *format_type_name(hl_type *t, char *buf, int bufsize) {
    if (!t || !is_valid_ptr(t)) {
        snprintf(buf, bufsize, "?");
        return buf;
    }
    switch (t->kind) {
    case HOBJ:
    case HSTRUCT:
        if (is_valid_ptr(t->obj) && is_valid_ptr(t->obj->name)) {
            snprintf(buf, bufsize, "%ls", (wchar_t*)t->obj->name);
        } else {
            snprintf(buf, bufsize, "%s", t->kind == HOBJ ? "obj" : "struct");
        }
        break;
    case HVIRTUAL:
        snprintf(buf, bufsize, "virtual");
        break;
    default:
        snprintf(buf, bufsize, "%s", type_name(t));
        break;
    }
    return buf;
}

/* Validate field index for object/struct/virtual */
static int validate_field_index(validator_ctx *ctx, hl_type *t, int field_idx, const char *context) {
    char type_buf[256];

    if (!is_valid_ptr(t)) {
        emit_error(ctx, "%s: invalid type pointer", context);
        return 0;
    }

    /* Basic sanity check - field index should be non-negative */
    if (field_idx < 0) {
        emit_error(ctx, "%s: negative field index %d", context, field_idx);
        return 0;
    }

    switch (t->kind) {
    case HOBJ:
        /* For objects, t->obj->nfields only counts fields defined on this class,
         * not inherited fields. The runtime builds a complete field table.
         * We can only validate if there's no superclass. */
        if (is_valid_ptr(t->obj) && !t->obj->super) {
            if (field_idx >= t->obj->nfields) {
                emit_error(ctx, "%s: field index %d out of bounds for %s (nfields=%d)",
                           context, field_idx, format_type_name(t, type_buf, sizeof(type_buf)),
                           t->obj->nfields);
                return 0;
            }
        }
        /* If there's a superclass, we can't easily validate without runtime info */
        break;
    case HSTRUCT:
        /* Structs don't have inheritance, so we can validate directly */
        if (is_valid_ptr(t->obj)) {
            if (field_idx >= t->obj->nfields) {
                emit_error(ctx, "%s: field index %d out of bounds for %s (nfields=%d)",
                           context, field_idx, format_type_name(t, type_buf, sizeof(type_buf)),
                           t->obj->nfields);
                return 0;
            }
        }
        break;
    case HVIRTUAL:
        if (is_valid_ptr(t->virt)) {
            if (field_idx >= t->virt->nfields) {
                emit_error(ctx, "%s: field index %d out of bounds for virtual (nfields=%d)",
                           context, field_idx, t->virt->nfields);
                return 0;
            }
        }
        break;
    default:
        /* For other types, we can't validate field index */
        break;
    }
    return 1;
}

/* Validate enum construct index */
static int validate_enum_construct(validator_ctx *ctx, hl_type *t, int construct_idx, const char *context) {
    if (!is_valid_ptr(t)) {
        emit_error(ctx, "%s: invalid type pointer", context);
        return 0;
    }
    if (t->kind != HENUM) {
        emit_error(ctx, "%s: expected enum type, got %d", context, t->kind);
        return 0;
    }
    if (is_valid_ptr(t->tenum)) {
        if (construct_idx < 0 || construct_idx >= t->tenum->nconstructs) {
            emit_error(ctx, "%s: construct index %d out of bounds (nconstructs=%d)",
                       context, construct_idx, t->tenum->nconstructs);
            return 0;
        }
    }
    return 1;
}

/* ============================================================================
 * Type checking helpers (for pedantic mode)
 * ============================================================================ */

static int is_numeric(hl_type *t) {
    if (!t) return 0;
    switch (t->kind) {
    case HUI8: case HUI16: case HI32: case HI64:
    case HF32: case HF64:
        return 1;
    default:
        return 0;
    }
}

static int is_integer(hl_type *t) {
    if (!t) return 0;
    switch (t->kind) {
    case HUI8: case HUI16: case HI32: case HI64:
        return 1;
    default:
        return 0;
    }
}

static int is_pointer(hl_type *t) {
    if (!t) return 0;
    switch (t->kind) {
    case HBYTES: case HDYN: case HFUN: case HOBJ: case HARRAY:
    case HTYPE: case HREF: case HVIRTUAL: case HDYNOBJ:
    case HABSTRACT: case HENUM: case HNULL: case HMETHOD:
    case HSTRUCT:
        return 1;
    default:
        return 0;
    }
}

static const char *type_name(hl_type *t) {
    if (!t) return "null";
    static const char *names[] = {
        "void", "u8", "u16", "i32", "i64", "f32", "f64", "bool",
        "bytes", "dyn", "fun", "obj", "array", "type", "ref",
        "virtual", "dynobj", "abstract", "enum", "null", "method",
        "struct", "packed", "guid"
    };
    if (t->kind >= 0 && t->kind < sizeof(names)/sizeof(names[0])) {
        return names[t->kind];
    }
    return "???";
}

/* ============================================================================
 * Per-opcode validation
 * ============================================================================ */

/* Safely get register type, returns NULL if invalid */
static hl_type *safe_get_reg_type(validator_ctx *ctx, int reg) {
    if (reg < 0 || reg >= ctx->f->nregs) return NULL;
    if (!is_valid_ptr(ctx->f->regs)) return NULL;
    hl_type *t = ctx->f->regs[reg];
    if (!is_valid_ptr(t)) return NULL;
    return t;
}

static void validate_opcode(validator_ctx *ctx, hl_opcode *op) {
    int p1 = op->p1, p2 = op->p2, p3 = op->p3;
    hl_type *t1 = NULL, *t2 = NULL, *t3 = NULL;

    /* Get register types for type checking (if registers are valid) */
    t1 = safe_get_reg_type(ctx, p1);
    t2 = safe_get_reg_type(ctx, p2);
    t3 = safe_get_reg_type(ctx, p3);

    switch (op->op) {
    /* OMov: dst, src */
    case OMov:
        validate_register(ctx, p1, "dst");
        validate_register(ctx, p2, "src");
        break;

    /* OInt: dst, int_pool_index */
    case OInt:
        validate_register(ctx, p1, "dst");
        validate_int_index(ctx, p2);
        break;

    /* OFloat: dst, float_pool_index */
    case OFloat:
        validate_register(ctx, p1, "dst");
        validate_float_index(ctx, p2);
        break;

    /* OBool: dst, 0/1 */
    case OBool:
        validate_register(ctx, p1, "dst");
        if (p2 != 0 && p2 != 1) {
            emit_warning(ctx, "bool value %d is not 0 or 1", p2);
        }
        break;

    /* OBytes: dst, bytes_pool_index */
    case OBytes:
        validate_register(ctx, p1, "dst");
        validate_bytes_index(ctx, p2);
        break;

    /* OString: dst, string_pool_index */
    case OString:
        validate_register(ctx, p1, "dst");
        validate_string_index(ctx, p2);
        break;

    /* ONull: dst */
    case ONull:
        validate_register(ctx, p1, "dst");
        break;

    /* Arithmetic: dst, src1, src2 */
    case OAdd: case OSub: case OMul: case OSDiv: case OUDiv:
    case OSMod: case OUMod: case OShl: case OSShr: case OUShr:
    case OAnd: case OOr: case OXor:
        validate_register(ctx, p1, "dst");
        validate_register(ctx, p2, "src1");
        validate_register(ctx, p3, "src2");
        /* Type check in pedantic mode */
        if (ctx->pedantic && t2 && t3) {
            if (!is_numeric(t2)) {
                emit_warning(ctx, "operand r%d is %s, expected numeric", p2, type_name(t2));
            }
            if (!is_numeric(t3)) {
                emit_warning(ctx, "operand r%d is %s, expected numeric", p3, type_name(t3));
            }
        }
        break;

    /* Unary: dst, src */
    case ONeg: case ONot:
        validate_register(ctx, p1, "dst");
        validate_register(ctx, p2, "src");
        break;

    /* Increment/decrement: reg */
    case OIncr: case ODecr:
        validate_register(ctx, p1, "reg");
        break;

    /* OCall0: dst, func_index */
    case OCall0:
        validate_register(ctx, p1, "dst");
        validate_function_index(ctx, p2);
        break;

    /* OCall1: dst, func_index, arg0 */
    case OCall1:
        validate_register(ctx, p1, "dst");
        validate_function_index(ctx, p2);
        validate_register(ctx, p3, "arg0");
        break;

    /* OCall2: dst, func_index, arg0, extra=arg1 (extra is value, not pointer!) */
    case OCall2: {
        validate_register(ctx, p1, "dst");
        validate_function_index(ctx, p2);
        validate_register(ctx, p3, "arg0");
        /* Note: for OCall2, extra is the register index cast to pointer, not a pointer to array */
        int arg1_idx = (int)(intptr_t)op->extra;
        validate_register(ctx, arg1_idx, "arg1");
        break;
    }

    /* OCall3: dst, func_index, arg0, extra[0]=arg1, extra[1]=arg2 */
    case OCall3:
        validate_register(ctx, p1, "dst");
        validate_function_index(ctx, p2);
        validate_register(ctx, p3, "arg0");
        if (is_valid_ptr(op->extra)) {
            validate_extra_register(ctx, op->extra, 0, "arg1");
            validate_extra_register(ctx, op->extra, 1, "arg2");
        } else {
            emit_error(ctx, "missing or invalid extra array for OCall3");
        }
        break;

    /* OCall4: dst, func_index, arg0, extra[0..2]=arg1..arg3 */
    case OCall4:
        validate_register(ctx, p1, "dst");
        validate_function_index(ctx, p2);
        validate_register(ctx, p3, "arg0");
        if (is_valid_ptr(op->extra)) {
            validate_extra_register(ctx, op->extra, 0, "arg1");
            validate_extra_register(ctx, op->extra, 1, "arg2");
            validate_extra_register(ctx, op->extra, 2, "arg3");
        } else {
            emit_error(ctx, "missing or invalid extra array for OCall4");
        }
        break;

    /* OCallN: dst, func_index, nargs, extra[0..nargs-1]=args */
    case OCallN:
        validate_register(ctx, p1, "dst");
        validate_function_index(ctx, p2);
        if (p3 < 0) {
            emit_error(ctx, "nargs=%d is negative", p3);
        } else if (p3 > 0) {
            if (is_valid_ptr(op->extra)) {
                for (int i = 0; i < p3; i++) {
                    char argname[16];
                    snprintf(argname, sizeof(argname), "arg%d", i);
                    validate_extra_register(ctx, op->extra, i, argname);
                }
            } else {
                emit_error(ctx, "missing or invalid extra array for OCallN with nargs=%d", p3);
            }
        }
        break;

    /* OCallMethod: dst, method_index, nargs, extra[0]=obj, extra[1..nargs-1]=args */
    case OCallMethod:
        validate_register(ctx, p1, "dst");
        /* p2 is method index - can't validate without type info */
        if (p3 < 0) {
            emit_error(ctx, "nargs=%d is negative", p3);
        } else if (p3 > 0) {
            if (is_valid_ptr(op->extra)) {
                validate_extra_register(ctx, op->extra, 0, "obj");
                for (int i = 1; i < p3; i++) {
                    char argname[16];
                    snprintf(argname, sizeof(argname), "arg%d", i - 1);
                    validate_extra_register(ctx, op->extra, i, argname);
                }
            } else {
                emit_error(ctx, "missing or invalid extra array for OCallMethod with nargs=%d", p3);
            }
        }
        break;

    /* OCallThis: dst, method_index, nextra, extra[0..nextra-1]=extra_args (this is always r0) */
    case OCallThis:
        validate_register(ctx, p1, "dst");
        /* p2 is method index */
        /* p3 is nextra (extra args beyond 'this') */
        if (p3 < 0) {
            emit_error(ctx, "nextra=%d is negative", p3);
        } else if (p3 > 0) {
            if (is_valid_ptr(op->extra)) {
                for (int i = 0; i < p3; i++) {
                    char argname[16];
                    snprintf(argname, sizeof(argname), "extra_arg%d", i);
                    validate_extra_register(ctx, op->extra, i, argname);
                }
            } else {
                emit_error(ctx, "missing or invalid extra array for OCallThis with nextra=%d", p3);
            }
        }
        /* Implicit: 'this' is r0 */
        validate_register(ctx, 0, "this");
        break;

    /* OCallClosure: dst, closure_reg, nargs, extra[0..nargs-1]=args */
    case OCallClosure:
        validate_register(ctx, p1, "dst");
        validate_register(ctx, p2, "closure");
        if (p3 < 0) {
            emit_error(ctx, "nargs=%d is negative", p3);
        } else if (p3 > 0) {
            if (is_valid_ptr(op->extra)) {
                for (int i = 0; i < p3; i++) {
                    char argname[16];
                    snprintf(argname, sizeof(argname), "arg%d", i);
                    validate_extra_register(ctx, op->extra, i, argname);
                }
            } else {
                emit_error(ctx, "missing or invalid extra array for OCallClosure with nargs=%d", p3);
            }
        }
        break;

    /* OStaticClosure: dst, func_index */
    case OStaticClosure:
        validate_register(ctx, p1, "dst");
        validate_function_index(ctx, p2);
        break;

    /* OInstanceClosure: dst, func_index, obj */
    case OInstanceClosure:
        validate_register(ctx, p1, "dst");
        validate_function_index(ctx, p2);
        validate_register(ctx, p3, "obj");
        break;

    /* OVirtualClosure: dst, obj, method_index */
    case OVirtualClosure:
        validate_register(ctx, p1, "dst");
        validate_register(ctx, p2, "obj");
        /* p3 is method index */
        break;

    /* OGetGlobal: dst, global_index */
    case OGetGlobal:
        validate_register(ctx, p1, "dst");
        validate_global_index(ctx, p2);
        break;

    /* OSetGlobal: global_index, src */
    case OSetGlobal:
        validate_global_index(ctx, p1);
        validate_register(ctx, p2, "src");
        break;

    /* OField: dst, obj, field_index */
    case OField:
        validate_register(ctx, p1, "dst");
        validate_register(ctx, p2, "obj");
        /* Validate field index if we have type info */
        if (t2) {
            validate_field_index(ctx, t2, p3, "OField");
            if (ctx->pedantic) {
                if (t2->kind != HOBJ && t2->kind != HSTRUCT && t2->kind != HVIRTUAL && t2->kind != HDYN) {
                    emit_warning(ctx, "field access on type %s", type_name(t2));
                }
            }
        }
        break;

    /* OSetField: obj, field_index, val */
    case OSetField:
        validate_register(ctx, p1, "obj");
        validate_register(ctx, p3, "val");
        /* p2 is field index */
        if (t1) {
            validate_field_index(ctx, t1, p2, "OSetField");
        }
        break;

    /* OGetThis: dst, field_index */
    case OGetThis:
        validate_register(ctx, p1, "dst");
        /* p2 is field index - this is r0 */
        if (ctx->f->nregs > 0 && ctx->f->regs[0]) {
            validate_field_index(ctx, ctx->f->regs[0], p2, "OGetThis");
        }
        break;

    /* OSetThis: field_index, val */
    case OSetThis:
        validate_register(ctx, p2, "val");
        /* p1 is field index */
        if (ctx->f->nregs > 0 && ctx->f->regs[0]) {
            validate_field_index(ctx, ctx->f->regs[0], p1, "OSetThis");
        }
        break;

    /* ODynGet: dst, obj, field_name_string_index */
    case ODynGet:
        validate_register(ctx, p1, "dst");
        validate_register(ctx, p2, "obj");
        validate_string_index(ctx, p3);
        break;

    /* ODynSet: obj, field_name_string_index, val */
    case ODynSet:
        validate_register(ctx, p1, "obj");
        validate_string_index(ctx, p2);
        validate_register(ctx, p3, "val");
        break;

    /* Conditional jumps with 1 reg: cond_reg, offset */
    case OJTrue: case OJFalse:
        validate_register(ctx, p1, "cond");
        validate_jump_target(ctx, p2);
        if (ctx->pedantic && t1 && t1->kind != HBOOL) {
            emit_warning(ctx, "condition r%d is %s, expected bool", p1, type_name(t1));
        }
        break;

    case OJNull: case OJNotNull:
        validate_register(ctx, p1, "obj");
        validate_jump_target(ctx, p2);
        if (ctx->pedantic && t1 && !is_pointer(t1)) {
            emit_warning(ctx, "null check on r%d which is %s (non-pointer)", p1, type_name(t1));
        }
        break;

    /* Comparison jumps: r1, r2, offset */
    case OJSLt: case OJSGte: case OJSGt: case OJSLte:
    case OJULt: case OJUGte: case OJNotLt: case OJNotGte:
    case OJEq: case OJNotEq:
        validate_register(ctx, p1, "r1");
        validate_register(ctx, p2, "r2");
        validate_jump_target(ctx, p3);
        break;

    /* OJAlways: offset */
    case OJAlways:
        validate_jump_target(ctx, p1);
        /* Pedantic: warn on double unconditional */
        if (ctx->pedantic && ctx->opindex + 1 < ctx->f->nops) {
            hl_opcode *next = &ctx->f->ops[ctx->opindex + 1];
            if (next->op == OJAlways) {
                emit_warning(ctx, "double unconditional jump (next op is also OJAlways)");
            }
        }
        break;

    /* Type conversions: dst, src */
    case OToDyn: case OToSFloat: case OToUFloat: case OToInt:
    case OSafeCast: case OUnsafeCast: case OToVirtual:
        validate_register(ctx, p1, "dst");
        validate_register(ctx, p2, "src");
        break;

    /* OLabel: no params */
    case OLabel:
        /* Pedantic: warn on consecutive labels */
        if (ctx->pedantic && ctx->opindex > 0) {
            hl_opcode *prev = &ctx->f->ops[ctx->opindex - 1];
            if (prev->op == OLabel) {
                emit_warning(ctx, "consecutive labels");
            }
        }
        break;

    /* ORet: val */
    case ORet:
        validate_register(ctx, p1, "val");
        break;

    /* OThrow/ORethrow: exception */
    case OThrow: case ORethrow:
        validate_register(ctx, p1, "exception");
        break;

    /* OSwitch: val, ncases, extra[0..ncases-1]=case_offsets, p3=default_offset */
    case OSwitch:
        validate_register(ctx, p1, "val");
        if (p2 < 0) {
            emit_error(ctx, "ncases=%d is negative", p2);
        } else {
            if (ctx->pedantic && p2 == 0) {
                emit_warning(ctx, "switch with 0 cases");
            }
            if (p2 > 0) {
                if (is_valid_ptr(op->extra)) {
                    for (int i = 0; i < p2; i++) {
                        int case_offset;
                        if (safe_read_extra(ctx, op->extra, i, &case_offset)) {
                            int case_target = ctx->opindex + 1 + case_offset;
                            /* Note: target == nops is valid (jump to end of function) */
                            if (case_target < 0 || case_target > ctx->f->nops) {
                                emit_error(ctx, "switch case %d target %d out of bounds", i, case_target);
                            }
                        }
                    }
                } else {
                    emit_error(ctx, "missing or invalid extra array for OSwitch with ncases=%d", p2);
                }
            }
            /* Validate default jump */
            validate_jump_target(ctx, p3);
        }
        break;

    /* ONullCheck: obj */
    case ONullCheck:
        validate_register(ctx, p1, "obj");
        break;

    /* OTrap: dst, offset */
    case OTrap:
        validate_register(ctx, p1, "dst");
        validate_jump_target(ctx, p2);
        break;

    /* OEndTrap: bool (1 to catch, 0 otherwise) */
    case OEndTrap:
        /* p1 is a flag, not a register */
        break;

    /* Memory access: dst, base, offset */
    case OGetI8: case OGetI16: case OGetMem: case OGetArray:
        validate_register(ctx, p1, "dst");
        validate_register(ctx, p2, "base");
        validate_register(ctx, p3, "offset");
        break;

    /* Memory write: base, offset, val */
    case OSetI8: case OSetI16: case OSetMem: case OSetArray:
        validate_register(ctx, p1, "base");
        validate_register(ctx, p2, "offset");
        validate_register(ctx, p3, "val");
        break;

    /* ONew: dst */
    case ONew:
        validate_register(ctx, p1, "dst");
        break;

    /* OArraySize: dst, array */
    case OArraySize:
        validate_register(ctx, p1, "dst");
        validate_register(ctx, p2, "array");
        break;

    /* OType: dst, type_index */
    case OType:
        validate_register(ctx, p1, "dst");
        validate_type_index(ctx, p2);
        break;

    /* OGetType: dst, val */
    case OGetType:
        validate_register(ctx, p1, "dst");
        validate_register(ctx, p2, "val");
        break;

    /* OGetTID: dst, val */
    case OGetTID:
        validate_register(ctx, p1, "dst");
        validate_register(ctx, p2, "val");
        break;

    /* ORef: dst, val */
    case ORef:
        validate_register(ctx, p1, "dst");
        validate_register(ctx, p2, "val");
        break;

    /* OUnref: dst, ref */
    case OUnref:
        validate_register(ctx, p1, "dst");
        validate_register(ctx, p2, "ref");
        break;

    /* OSetref: ref, val */
    case OSetref:
        validate_register(ctx, p1, "ref");
        validate_register(ctx, p2, "val");
        break;

    /* OMakeEnum: dst, construct_index, nargs, extra[0..nargs-1]=args */
    case OMakeEnum:
        validate_register(ctx, p1, "dst");
        /* Validate construct index if we have type info */
        if (t1) {
            validate_enum_construct(ctx, t1, p2, "OMakeEnum");
            /* CRITICAL: Check that p3 (nargs provided) matches construct's nparams.
             * The JIT loops over c->nparams but reads from extra[0..p3-1].
             * If nparams > p3, the JIT reads garbage from extra array. */
            if (t1->kind == HENUM && is_valid_ptr(t1->tenum) &&
                p2 >= 0 && p2 < t1->tenum->nconstructs) {
                hl_enum_construct *c = &t1->tenum->constructs[p2];
                if (c->nparams != p3) {
                    emit_error(ctx, "OMakeEnum: construct %d has %d params but bytecode provides %d args",
                               p2, c->nparams, p3);
                }
            }
        }
        if (p3 < 0) {
            emit_error(ctx, "nargs=%d is negative", p3);
        } else if (p3 > 0) {
            if (is_valid_ptr(op->extra)) {
                for (int i = 0; i < p3; i++) {
                    char argname[16];
                    snprintf(argname, sizeof(argname), "arg%d", i);
                    validate_extra_register(ctx, op->extra, i, argname);
                }
            } else {
                emit_error(ctx, "missing or invalid extra array for OMakeEnum with nargs=%d", p3);
            }
        }
        break;

    /* OEnumAlloc: dst, construct_index */
    case OEnumAlloc:
        validate_register(ctx, p1, "dst");
        /* p2 is construct index - need type info to validate */
        break;

    /* OEnumIndex: dst, enum */
    case OEnumIndex:
        validate_register(ctx, p1, "dst");
        validate_register(ctx, p2, "enum");
        break;

    /* OEnumField: dst, enum, construct_index, extra=field_index (extra is value, not pointer!) */
    case OEnumField: {
        validate_register(ctx, p1, "dst");
        validate_register(ctx, p2, "enum");
        /* Note: for OEnumField, extra is the field index cast to pointer, not a pointer */
        int field_idx = (int)(intptr_t)op->extra;
        if (t2) {
            validate_enum_construct(ctx, t2, p3, "OEnumField");
            /* Validate field index within construct */
            if (t2->kind == HENUM && is_valid_ptr(t2->tenum) &&
                p3 >= 0 && p3 < t2->tenum->nconstructs &&
                is_valid_ptr(t2->tenum->constructs)) {
                hl_enum_construct *c = &t2->tenum->constructs[p3];
                if (is_valid_ptr(c)) {
                    if (field_idx < 0 || field_idx >= c->nparams) {
                        emit_error(ctx, "enum field index %d out of bounds for construct %d (nparams=%d)",
                                   field_idx, p3, c->nparams);
                    }
                }
            }
        }
        break;
    }

    /* OSetEnumField: enum, field_index, val */
    case OSetEnumField: {
        validate_register(ctx, p1, "enum");
        /* p2 is field index */
        validate_register(ctx, p3, "val");
        /* Validate field index - need enum type from p1 */
        if (t1 && t1->kind == HENUM && is_valid_ptr(t1->tenum)) {
            /* We don't know the construct index at validation time for OSetEnumField,
             * but we can at least check that p2 is non-negative and within the max
             * params of any construct */
            if (p2 < 0) {
                emit_error(ctx, "OSetEnumField: negative field index %d", p2);
            } else {
                /* Find max params across all constructs */
                int max_params = 0;
                for (int ci = 0; ci < t1->tenum->nconstructs; ci++) {
                    if (t1->tenum->constructs[ci].nparams > max_params) {
                        max_params = t1->tenum->constructs[ci].nparams;
                    }
                }
                if (p2 >= max_params) {
                    emit_error(ctx, "OSetEnumField: field index %d exceeds max params %d for any construct",
                               p2, max_params);
                }
            }
        }
        break;
    }

    /* OAssert: no params */
    case OAssert:
        break;

    /* ORefData: dst, ref */
    case ORefData:
        validate_register(ctx, p1, "dst");
        validate_register(ctx, p2, "ref");
        break;

    /* ORefOffset: dst, ref, offset */
    case ORefOffset:
        validate_register(ctx, p1, "dst");
        validate_register(ctx, p2, "ref");
        validate_register(ctx, p3, "offset");
        break;

    /* ONop: no params */
    case ONop:
        break;

    /* OPrefetch: ptr, offset, mode */
    case OPrefetch:
        validate_register(ctx, p1, "ptr");
        /* p2, p3 are offset and mode - not registers */
        break;

    /* OAsm: (inline assembly, rarely used) */
    case OAsm:
        /* p1 = asm type, p2 = dst reg, p3 = nregs, extra = regs */
        if (p2 >= 0) validate_register(ctx, p2, "dst");
        break;

    /* OCatch: exception_reg */
    case OCatch:
        validate_register(ctx, p1, "exception");
        break;

    default:
        if (op->op >= OLast) {
            emit_error(ctx, "invalid opcode %d", op->op);
        }
        break;
    }
}

/* ============================================================================
 * Control flow validation (pedantic mode)
 * ============================================================================ */

static void validate_control_flow(validator_ctx *ctx) {
    int unreachable = 0;

    for (int i = 0; i < ctx->f->nops; i++) {
        hl_opcode *op = &ctx->f->ops[i];

        /* Check for unreachable code after unconditional jumps */
        if (unreachable) {
            if (op->op == OLabel) {
                unreachable = 0;  /* Label makes code reachable again */
            } else {
                emit_warning(ctx, "unreachable code at opcode %d (after unconditional jump/return)", i);
                unreachable = 0;  /* Only warn once per block */
            }
        }

        /* Mark following code as unreachable after these */
        if (op->op == OJAlways || op->op == ORet || op->op == OThrow || op->op == ORethrow) {
            unreachable = 1;
        }
    }
}

/* ============================================================================
 * Function validation
 * ============================================================================ */

static void validate_function(validator_ctx *ctx, hl_function *f, int func_array_idx) {
    if (!is_valid_ptr(f)) {
        emit_global_error(ctx, "invalid function pointer");
        return;
    }

    ctx->f = f;
    ctx->findex = f->findex;
    ctx->current_func_offset = 0;
    ctx->current_op_offset = 0;

    /* Set function offset if available */
    if (ctx->func_offsets && func_array_idx >= 0) {
        ctx->current_func_offset = ctx->func_offsets[func_array_idx];
    }

    if (ctx->verbose) {
        if (ctx->current_func_offset > 0) {
            printf("Validating F%d @0x%x (%d registers, %d opcodes)\n",
                   f->findex, ctx->current_func_offset, f->nregs, f->nops);
        } else {
            printf("Validating F%d (%d registers, %d opcodes)\n", f->findex, f->nregs, f->nops);
        }
    }

    /* Check function type pointer */
    if (!is_valid_ptr(f->type)) {
        emit_global_error(ctx, "F%d: invalid function type pointer", f->findex);
    } else {
        /* Check that function type is actually HFUN or HMETHOD, not some other type.
         * This catches bytecode corruption where a function's type field points to
         * an object type or other non-function type, which would cause the JIT to
         * read garbage when accessing f->type->fun->nargs. */
        if (f->type->kind != HFUN && f->type->kind != HMETHOD) {
            emit_global_error(ctx, "F%d: function type is %s (kind=%d), expected HFUN or HMETHOD",
                              f->findex, type_name(f->type), f->type->kind);
        } else {
            /* Validate function signature makes sense */
            if (!is_valid_ptr(f->type->fun)) {
                emit_global_error(ctx, "F%d: function type has invalid fun pointer", f->findex);
            } else {
                /* Check nargs is reasonable */
                if (f->type->fun->nargs < 0 || f->type->fun->nargs > 1000) {
                    emit_global_error(ctx, "F%d: function type has suspicious nargs=%d",
                                      f->findex, f->type->fun->nargs);
                }
                /* Check nargs doesn't exceed nregs (args must fit in registers) */
                if (f->type->fun->nargs > f->nregs) {
                    emit_global_error(ctx, "F%d: function has %d args but only %d registers",
                                      f->findex, f->type->fun->nargs, f->nregs);
                }
            }
        }
    }

    /* Check register types array */
    if (f->nregs > 0 && !is_valid_ptr(f->regs)) {
        emit_global_error(ctx, "F%d: invalid register types array (nregs=%d)", f->findex, f->nregs);
    }

    /* Check opcodes array */
    if (f->nops > 0 && !is_valid_ptr(f->ops)) {
        emit_global_error(ctx, "F%d: invalid opcodes array (nops=%d)", f->findex, f->nops);
        return;  /* Can't validate opcodes without valid array */
    }

    /* Suspicious values check */
    if (ctx->pedantic) {
        if (f->nops == 0) {
            emit_global_warning(ctx, "F%d: empty function (0 opcodes)", f->findex);
        } else if (f->nops > 10000) {
            emit_global_warning(ctx, "F%d: very large function (%d opcodes)", f->findex, f->nops);
        }
        if (f->nregs > 10000) {
            emit_global_warning(ctx, "F%d: very many registers (%d)", f->findex, f->nregs);
        }
        if (f->nregs < 0 || f->nops < 0) {
            emit_global_error(ctx, "F%d: negative nregs=%d or nops=%d", f->findex, f->nregs, f->nops);
            return;
        }
    }

    /* Check if we have valid offsets for this function.
     * Validate that scanner's nops matches actual nops to detect desync. */
    int *op_offsets = NULL;
    if (ctx->opcode_offsets && ctx->func_nops && func_array_idx >= 0 &&
        func_array_idx < ctx->code->nfunctions && ctx->opcode_offsets[func_array_idx]) {
        /* Only use offsets if scanner's nops matches actual nops */
        if (ctx->func_nops[func_array_idx] == f->nops) {
            op_offsets = ctx->opcode_offsets[func_array_idx];
        } else if (ctx->verbose) {
            printf("  Warning: scanner desync at F%d - scanned %d ops, actual %d ops\n",
                   f->findex, ctx->func_nops[func_array_idx], f->nops);
        }
    }

    /* Validate each opcode */
    for (int i = 0; i < f->nops; i++) {
        ctx->opindex = i;
        ctx->current_op_offset = op_offsets ? op_offsets[i] : 0;
        validate_opcode(ctx, &f->ops[i]);
    }

    /* Control flow validation in pedantic mode */
    if (ctx->pedantic) {
        ctx->opindex = -1;
        ctx->current_op_offset = 0;
        validate_control_flow(ctx);
    }
}

/* ============================================================================
 * Global validation
 * ============================================================================ */

static void validate_globals(validator_ctx *ctx) {
    /* Check entrypoint is valid */
    if (!validate_function_index(ctx, ctx->code->entrypoint)) {
        emit_global_error(ctx, "invalid entrypoint function index %d", ctx->code->entrypoint);
    }

    /* Check type references in globals */
    if (is_valid_ptr(ctx->code->globals)) {
        for (int i = 0; i < ctx->code->nglobals; i++) {
            hl_type *t = ctx->code->globals[i];
            if (!is_valid_ptr(t)) {
                emit_global_error(ctx, "global %d has invalid type pointer", i);
            } else if (t->kind >= HLAST) {
                emit_global_error(ctx, "global %d has invalid type kind %d", i, t->kind);
            }
        }
    } else if (ctx->code->nglobals > 0) {
        emit_global_error(ctx, "globals array is invalid but nglobals=%d", ctx->code->nglobals);
    }

    /* Check native function types */
    if (is_valid_ptr(ctx->code->natives)) {
        for (int i = 0; i < ctx->code->nnatives; i++) {
            hl_native *n = &ctx->code->natives[i];
            if (!is_valid_ptr(n)) {
                emit_global_error(ctx, "native %d has invalid pointer", i);
                continue;
            }
            if (!is_valid_ptr(n->t)) {
                emit_global_error(ctx, "native F%d has invalid type pointer", n->findex);
            } else if (n->t->kind != HFUN && n->t->kind != HMETHOD) {
                emit_global_warning(ctx, "native F%d has non-function type %d", n->findex, n->t->kind);
            }
        }
    } else if (ctx->code->nnatives > 0) {
        emit_global_error(ctx, "natives array is invalid but nnatives=%d", ctx->code->nnatives);
    }
}

/* ============================================================================
 * Main validation entry point
 * ============================================================================ */

static int validate_code(hl_code *code, int pedantic, int verbose, int single_func,
                         int *func_offsets, int **opcode_offsets, int *func_nops) {
    validator_ctx ctx = {0};
    ctx.code = code;
    ctx.pedantic = pedantic;
    ctx.verbose = verbose;
    ctx.single_func = single_func;
    ctx.opindex = -1;
    ctx.func_offsets = func_offsets;
    ctx.opcode_offsets = opcode_offsets;
    ctx.func_nops = func_nops;

    printf("Validating bytecode...\n");

    /* Check code structure itself */
    if (!is_valid_ptr(code)) {
        printf("ERROR: invalid code pointer\n");
        return 1;
    }

    /* Global validation */
    ctx.findex = -1;
    validate_globals(&ctx);

    /* Function validation */
    if (!is_valid_ptr(code->functions) && code->nfunctions > 0) {
        emit_global_error(&ctx, "invalid functions array but nfunctions=%d", code->nfunctions);
    } else {
        for (int i = 0; i < code->nfunctions; i++) {
            hl_function *f = &code->functions[i];

            /* Skip if filtering to single function */
            if (single_func >= 0 && f->findex != single_func) continue;

            validate_function(&ctx, f, i);
        }
    }

    printf("\nValidation complete: %d error%s, %d warning%s\n",
           ctx.error_count, ctx.error_count == 1 ? "" : "s",
           ctx.warning_count, ctx.warning_count == 1 ? "" : "s");

    return ctx.error_count > 0 ? 1 : 0;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file.hl> [-w] [-v] [-f <function_index>]\n", argv[0]);
        fprintf(stderr, "  -w         Pedantic mode (include warnings)\n");
        fprintf(stderr, "  -v         Verbose output\n");
        fprintf(stderr, "  -f <idx>   Validate only function at index\n");
        return 2;
    }

    const char *filename = argv[1];
    int pedantic = 0;
    int verbose = 0;
    int single_func = -1;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0) {
            pedantic = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            single_func = atoi(argv[++i]);
        }
    }

    /* Install signal handler for safe pointer access */
    signal(SIGSEGV, segv_handler);

    /* Initialize HL */
    hl_global_init();

    /* Load the bytecode */
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", filename);
        return 2;
    }

    fseek(f, 0, SEEK_END);
    int size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *data = malloc(size);
    if (fread(data, 1, size, f) != (size_t)size) {
        fprintf(stderr, "Failed to read %s\n", filename);
        fclose(f);
        free(data);
        return 2;
    }
    fclose(f);

    /* Parse bytecode */
    char *error_msg = NULL;
    hl_code *code = hl_code_read((unsigned char*)data, size, &error_msg);

    if (!code) {
        fprintf(stderr, "Failed to parse bytecode: %s\n", error_msg ? error_msg : "unknown error");
        free(data);
        return 2;
    }

    /* Build offset tables for byte-level error reporting */
    int *func_offsets = NULL;
    int **opcode_offsets = NULL;
    int *func_nops = NULL;
    if (!build_offset_tables((unsigned char*)data, size, code, &func_offsets, &opcode_offsets, &func_nops)) {
        fprintf(stderr, "Warning: failed to build offset tables, byte offsets will not be shown\n");
    }
    free(data);  /* No longer needed after building offset tables */

    /* Print summary */
    printf("HashLink Bytecode: %s\n", filename);
    printf("  Version: %d\n", code->version);
    printf("  Entrypoint: F%d\n", code->entrypoint);
    printf("  Types: %d, Globals: %d, Natives: %d, Functions: %d\n",
           code->ntypes, code->nglobals, code->nnatives, code->nfunctions);
    printf("  Strings: %d, Ints: %d, Floats: %d, Bytes: %d\n",
           code->nstrings, code->nints, code->nfloats, code->nbytes);
    printf("\n");

    /* Validate */
    int result = validate_code(code, pedantic, verbose, single_func, func_offsets, opcode_offsets, func_nops);

    /* Cleanup */
    free_offset_tables(func_offsets, opcode_offsets, func_nops, code->nfunctions);

    return result;
}
