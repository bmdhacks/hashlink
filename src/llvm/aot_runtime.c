/*
 * AOT Runtime Helper
 * Provides module loading functions for AOT-compiled binaries
 */
#include <hl.h>
#include <hlmodule.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

/* For backtrace support on Linux/Mac */
#if defined(HL_LINUX) && (!defined(HL_ANDROID) || __ANDROID_MIN_SDK_VERSION__ >= 33)
#define HL_LINUX_BACKTRACE
#endif

#if defined(HL_LINUX_BACKTRACE) || defined(HL_MAC)
#include <execinfo.h>
#endif

#if defined(HL_LINUX) || defined(HL_MAC)
#include <signal.h>

/*
 * Signal handler for AOT binaries.
 * Similar to setup_handler() in main.c - prints stack trace and re-raises.
 */
static void aot_handle_signal(int signum) {
    signal(signum, SIG_DFL);
    printf("SIGNAL %d\n", signum);
    fflush(stdout);
    if (hl_get_thread() != NULL) {
        hl_dump_stack();
    }
    fflush(stdout);
    raise(signum);
}

static void aot_setup_signals(void) {
    struct sigaction act;
    act.sa_sigaction = NULL;
    act.sa_handler = aot_handle_signal;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);
    signal(SIGPIPE, SIG_IGN);
    sigaction(SIGSEGV, &act, NULL);
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGINT, &act, NULL);  /* Also handle Ctrl+C */
}
#else
static void aot_setup_signals(void) {}
#endif

/*
 * Global types array - set after module initialization.
 *
 * AOT-compiled code needs access to runtime type information for:
 *   - Object allocation (hl_alloc_obj needs hl_type with initialized obj->rt)
 *   - Type casting and checks
 *   - Virtual method dispatch
 *
 * This global points to the types array from the loaded module, which has
 * all the runtime type info (obj->rt) properly initialized by hl_module_init().
 * AOT code accesses types as &aot_types[type_idx].
 */
hl_type *aot_types = NULL;
int aot_ntypes = 0;

/*
 * Global variables storage - set after module initialization.
 *
 * HashLink globals are stored in a byte buffer (globals_data) with
 * per-global offsets (globals_indexes). hl_module_init() initializes
 * these arrays.
 */
static unsigned char *aot_globals_data = NULL;
static int *aot_globals_indexes = NULL;
static int aot_nglobals = 0;

/*
 * Get a type pointer by index.
 * Used by AOT-compiled code to get properly initialized type pointers.
 */
void *aot_get_type(int idx) {
    if (idx >= 0 && idx < aot_ntypes && aot_types != NULL) {
        return (void *)&aot_types[idx];
    }
    return NULL;
}

/*
 * Get a global variable pointer by index.
 * Returns a pointer to the global's storage location in globals_data.
 */
void *aot_get_global(int idx) {
    if (idx >= 0 && idx < aot_nglobals && aot_globals_data != NULL && aot_globals_indexes != NULL) {
        return (void*)(aot_globals_data + aot_globals_indexes[idx]);
    }
    return NULL;
}

/*
 * JIT stub functions for AOT runtime.
 *
 * AOT-compiled binaries don't need JIT compilation, but hl_module_init() in
 * module.c calls these functions. We provide minimal stubs that:
 *   - Return non-NULL from hl_jit_alloc() so init doesn't fail
 *   - Return success (>=0) from hl_jit_function() for each function
 *   - Return a dummy code pointer from hl_jit_code()
 *
 * The actual function code is already AOT-compiled into the binary, so
 * we don't need to generate anything. The module init will set up
 * functions_ptrs based on the "JIT code" we return, but for AOT we'll
 * override those pointers later or use our own dispatch.
 */
static int dummy_jit_ctx;  /* Non-NULL pointer for hl_jit_alloc */
static char dummy_code[16] = {0};  /* Dummy code buffer */

jit_ctx *hl_jit_alloc(void) { return (jit_ctx *)&dummy_jit_ctx; }
void hl_jit_init(jit_ctx *ctx, hl_module *m) { (void)ctx; (void)m; }
int hl_jit_function(jit_ctx *ctx, hl_module *m, hl_function *f) {
    (void)ctx; (void)m; (void)f;
    return 0;  /* Return offset 0 - all functions point to start of dummy_code */
}
void hl_jit_free(jit_ctx *ctx, h_bool can_reset) { (void)ctx; (void)can_reset; }
void *hl_jit_code(jit_ctx *ctx, hl_module *m, int *size, hl_debug_infos **dbg, hl_module *prev) {
    (void)ctx; (void)m; (void)prev;
    if (size) *size = sizeof(dummy_code);
    if (dbg) *dbg = NULL;
    return dummy_code;  /* Return pointer to dummy code buffer */
}
void hl_jit_reset(jit_ctx *ctx, hl_module *m) { (void)ctx; (void)m; }
void hl_jit_patch_method(void *old_fun, void **new_fun) { (void)old_fun; (void)new_fun; }

/*
 * C2HL trampoline for dynamic function calls (AArch64).
 *
 * This is the equivalent of jit_c2hl in the JIT. It takes:
 *   X0 = function pointer to call
 *   X1 = pointer to register args (X0-X7, then D0-D7)
 *   X2 = pointer to stack args end
 *
 * It loads arguments from the prepared buffers and calls the function.
 */
#if defined(__aarch64__)
__asm__ (
    ".global aot_c2hl_trampoline\n"
    ".type aot_c2hl_trampoline, %function\n"
    "aot_c2hl_trampoline:\n"
    /* Save frame */
    "stp x29, x30, [sp, #-16]!\n"
    "mov x29, sp\n"

    /* Save function pointer and stack args pointers */
    "mov x9, x0\n"      /* X9 = function to call */
    "mov x10, x1\n"     /* X10 = reg args ptr */
    "mov x11, x2\n"     /* X11 = stack args end */

    /* Load integer registers X0-X7 from [X10] */
    "ldp x0, x1, [x10, #0]\n"
    "ldp x2, x3, [x10, #16]\n"
    "ldp x4, x5, [x10, #32]\n"
    "ldp x6, x7, [x10, #48]\n"

    /* Load FP registers D0-D7 from [X10 + 64] */
    "ldp d0, d1, [x10, #64]\n"
    "ldp d2, d3, [x10, #80]\n"
    "ldp d4, d5, [x10, #96]\n"
    "ldp d6, d7, [x10, #112]\n"

    /*
     * Push stack args correctly with 8-byte contiguous layout.
     * Stack args are in buffer at [X10+128, X11).
     * Need to allocate space and copy with proper 16-byte alignment.
     */
    "add x12, x10, #128\n"  /* X12 = start of stack args in buffer */
    "sub x13, x11, x12\n"   /* X13 = stack args size in bytes */
    "cbz x13, 2f\n"         /* Skip if no stack args */

    /* Round size up to 16-byte alignment and allocate */
    "add x14, x13, #15\n"
    "and x14, x14, #-16\n"  /* X14 = aligned size */
    "sub sp, sp, x14\n"     /* Allocate stack space */

    /* Copy stack args from buffer to stack (forward order) */
    /* Buffer: [X12, X11), Stack: [sp, sp + size) */
    "mov x14, sp\n"         /* X14 = destination pointer */
    "1:\n"
    "cmp x12, x11\n"
    "b.ge 2f\n"
    "ldr x15, [x12], #8\n"  /* Load 8 bytes from buffer, X12 += 8 */
    "str x15, [x14], #8\n"  /* Store 8 bytes to stack, X14 += 8 */
    "b 1b\n"
    "2:\n"

    /* Call the function */
    "blr x9\n"

    /* Restore frame and return */
    "mov sp, x29\n"
    "ldp x29, x30, [sp], #16\n"
    "ret\n"
);
extern void *aot_c2hl_trampoline(void *func, void *regs, void *stack_end);
#else
/* Stub for non-AArch64 platforms */
static void *aot_c2hl_trampoline(void *f, void *regs, void *stack) {
    (void)f; (void)regs; (void)stack;
    hl_error("AOT C2HL trampoline not implemented for this platform");
    return NULL;
}
#endif

/* Number of register arguments (X0-X7 for ints, D0-D7 for floats) */
#define CALL_NREGS 8
#define MAX_ARGS 64

/*
 * Closure hasValue field values:
 * - CLOSURE_NONE: No captured value (void closure, e.g. static function reference)
 * - CLOSURE_VALUE: Has captured value (closure with bound first argument)
 * - CLOSURE_WRAPPER: Wrapper closure for type conversion between function signatures
 */
#define CLOSURE_NONE    0
#define CLOSURE_VALUE   1
#define CLOSURE_WRAPPER 2

/*
 * Select which register to use for an argument (C2HL direction).
 * Returns register index (0-7 for int, 8-15 for FP) or -1 for stack.
 */
static int select_call_reg_c2hl(int *nextCpu, int *nextFpu, hl_type *t) {
    switch (t->kind) {
    case HF32:
    case HF64:
        if (*nextFpu < CALL_NREGS) return CALL_NREGS + (*nextFpu)++;
        return -1;
    default:
        if (*nextCpu < CALL_NREGS) return (*nextCpu)++;
        return -1;
    }
}

/*
 * Get stack size for a type.
 */
static int stack_size_c2hl(hl_type *t) {
    switch (t->kind) {
    case HUI8:
    case HBOOL:
        return 1;
    case HUI16:
        return 2;
    case HI32:
    case HF32:
        return 4;
    default:
        return 8;
    }
}

/*
 * Callback for dynamic function calls (C -> HL direction).
 * Called by hl_dyn_call/hl_call_method to invoke AOT functions dynamically.
 */
static void *aot_callback_c2hl(void *_f, hl_type *t, void **args, vdynamic *ret) {
    void **f = (void**)_f;
    unsigned char stack[MAX_ARGS * 16];
    int nextCpu = 0, nextFpu = 0;
    int mappedRegs[MAX_ARGS];

    memset(stack, 0, sizeof(stack));

    if (t->fun->nargs > MAX_ARGS)
        hl_error("Too many arguments for dynamic call");

    /* First pass: determine register assignments and stack size */
    int i, size = 0;
    for (i = 0; i < t->fun->nargs; i++) {
        hl_type *at = t->fun->args[i];
        int creg = select_call_reg_c2hl(&nextCpu, &nextFpu, at);
        mappedRegs[i] = creg;
        if (creg < 0) {
            int tsize = stack_size_c2hl(at);
            if (tsize < 8) tsize = 8;
            size += tsize;
        }
    }

    /* Align stack size to 16 bytes */
    int pad = (-size) & 15;
    size += pad;

    /* Second pass: copy arguments to appropriate locations */
    /* stack layout: [0..64) = X0-X7, [64..128) = D0-D7, [128..) = stack args */
    /*
     * args[i] points to a vdynamic struct: { hl_type *t; union v; }
     * The actual value is in the 'v' union at offset 8, not at offset 0.
     * We need to read from the correct offset based on the expected type.
     */
    int pos = 128;  /* Stack args start after register save area */
    for (i = 0; i < t->fun->nargs; i++) {
        hl_type *at = t->fun->args[i];
        vdynamic *dyn = (vdynamic*)args[i];
        int creg = mappedRegs[i];
        void *store;

        if (creg >= 0) {
            store = stack + creg * 8;
        } else {
            store = stack + pos;
            pos += 8;
        }

        /*
         * args[] comes from hl_wrapper_call's vargs[]:
         * - For 'this' (w->value): raw object pointer
         * - For primitives: pointer to converted value (in tmp[] on wrapper's stack)
         * - For pointers: raw object pointer from hl_dyn_castp
         *
         * So for primitives, we dereference args[i] to get the value.
         * For pointers, args[i] IS the pointer value directly.
         */
        switch (at->kind) {
        case HUI8:
        case HBOOL:
            *(int*)store = *(unsigned char*)args[i];
            break;
        case HUI16:
            *(int*)store = *(unsigned short*)args[i];
            break;
        case HI32:
            *(int*)store = *(int*)args[i];
            break;
        case HI64:
            *(int_val*)store = *(int_val*)args[i];
            break;
        case HF32:
            *(float*)store = *(float*)args[i];
            break;
        case HF64:
            *(double*)store = *(double*)args[i];
            break;
        default:
            /* Pointer types: args[i] is the raw pointer, use it directly */
            *(void**)store = args[i];
            break;
        }
    }

    /* Call through trampoline */
    /* The casts for float/double are intentional - the trampoline returns
     * in D0 for FP types, and the cast tells the compiler to read from D0. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wbad-function-cast"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    switch (t->fun->ret->kind) {
    case HVOID:
        aot_c2hl_trampoline(*f, stack, stack + pos);
        return NULL;
    case HUI8:
    case HBOOL:
    case HUI16:
    case HI32:
        ret->v.i = (int)(int_val)aot_c2hl_trampoline(*f, stack, stack + pos);
        return &ret->v.i;
    case HI64:
        ret->v.i64 = (int_val)aot_c2hl_trampoline(*f, stack, stack + pos);
        return &ret->v.i64;
    case HF32: {
        float (*fp)(void*,void*,void*) = (float(*)(void*,void*,void*))(void*)aot_c2hl_trampoline;
        ret->v.f = fp(*f, stack, stack + pos);
        return &ret->v.f;
    }
    case HF64: {
        double (*dp)(void*,void*,void*) = (double(*)(void*,void*,void*))(void*)aot_c2hl_trampoline;
        ret->v.d = dp(*f, stack, stack + pos);
        return &ret->v.d;
    }
    default:
        return aot_c2hl_trampoline(*f, stack, stack + pos);
    }
#pragma GCC diagnostic pop
}

/*
 * HL-to-C Wrapper Support
 *
 * When a function is cast to a different signature (e.g., Float->Float to Int->Float),
 * hl_make_fun_wrapper creates a vclosure_wrapper that intercepts calls.
 * The wrapper trampoline saves native calling convention registers, unpacks them
 * to dynamic values, calls the wrapped function, and converts the return value.
 */

/*
 * HL-to-C wrapper call implementation.
 *
 * This follows the same pattern as hl_wrapper_call() in fun.c:
 * 1. Extract arguments from native calling convention (saved regs + stack)
 * 2. Convert arguments from wrapper type to wrapped function type
 * 3. Call the wrapped function using static_call
 * 4. Convert return value back to wrapper's return type
 *
 * This avoids using hl_dyn_call which would add an extra conversion layer
 * and cause issues with stack-allocated temporaries.
 *
 * @param c - The closure wrapper
 * @param stack_args - Pointer to stack arguments (caller's stack frame)
 * @param regs - Buffer containing saved registers: [X0-X7][D0-D7] = 128 bytes
 * @param ret - Pointer to store return value
 */
/*
 * Check if a pointer is on the stack by comparing to current SP.
 * Uses getrlimit to get actual stack size limit (cached).
 */
static unsigned long aot_stack_size = 0;

static inline bool aot_is_stack_ptr(void *p) {
    char stack_local;
    unsigned long sp = (unsigned long)&stack_local;
    unsigned long ptr = (unsigned long)p;
    unsigned long diff = (ptr > sp) ? (ptr - sp) : (sp - ptr);
    if (aot_stack_size == 0) {
        struct rlimit rl;
        aot_stack_size = 8 * 1024 * 1024;  /* Default 8MB */
        if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
            aot_stack_size = rl.rlim_cur;
        }
    }
    return diff < aot_stack_size;
}

static void *aot_wrapper_call_impl(vclosure_wrapper *c, char *stack_args, void **regs, vdynamic *ret) {
    hl_type_fun *tfun = c->cl.t->fun;  /* Wrapper's function type */
    vclosure *w = c->wrappedFun;
    union { double d; int i; float f; int64 i64; } tmp[MAX_ARGS];
    void *vargs[MAX_ARGS + 1];
    int i, p = 0;
    int nextCpu = 1;  /* Skip X0 which holds the closure pointer */
    int nextFpu = 0;
    void *pret, *aret;
    hl_type *call_type;

    /*
     * Note: Stack-allocated closures are now copied to heap in hl_make_fun_wrapper,
     * so wrappedFun should always be a valid heap pointer.
     */

    if (tfun->nargs > MAX_ARGS)
        hl_error("Too many arguments for wrapped call");

    /*
     * Handle var_args closures (created by Reflect.makeVarArgs).
     * These have w->t->fun->nargs == -1 as a marker.
     * The real callback is stored in w->value.
     */
    if (w->t->fun->nargs < 0) {
        varray *a;
        vclosure *real_cb = (vclosure*)w->value;

        /* Collect all wrapper arguments into a Dynamic array */
        a = hl_alloc_array(&hlt_dyn, tfun->nargs);
        for (i = 0; i < tfun->nargs; i++) {
            hl_type *t = tfun->args[i];
            void *v;

            /* Extract argument from registers or stack */
            if (t->kind == HF32 || t->kind == HF64) {
                if (nextFpu < CALL_NREGS) {
                    v = (void*)(regs + CALL_NREGS + nextFpu);
                    nextFpu++;
                } else {
                    v = stack_args;
                    stack_args += 8;
                }
            } else {
                if (nextCpu < CALL_NREGS) {
                    v = (void*)(regs + nextCpu);
                    nextCpu++;
                } else {
                    v = stack_args;
                    stack_args += 8;
                }
                /* v now points to where the value is stored (works for both pointers and ints) */
            }
            /* hl_make_dyn expects a pointer TO the data */
            hl_aptr(a, void*)[i] = hl_make_dyn(v, t);
        }

        /* Build args for the real callback */
        if (real_cb->hasValue)
            vargs[p++] = (vdynamic*)real_cb->value;
        vargs[p++] = (vdynamic*)a;

        /* Reassign w to real callback so return conversion uses correct type */
        w = real_cb;

        /* Call the real callback via static_call and fall through to return conversion */
        call_type = w->hasValue ? w->t->fun->parent : w->t;
        pret = hl_setup.static_call(
            hl_setup.static_call_ref ? &w->fun : w->fun,
            call_type,
            vargs, ret);
        goto do_return_conversion;
    }

    /*
     * If wrapped closure has a captured value (CLOSURE_VALUE), add it as first arg.
     * The parent type includes this value as its first formal argument.
     *
     * For CLOSURE_WRAPPER, we handle differently below - the wrapper must go
     * in X0 but isn't part of the formal type, so we can't use static_call.
     */
    if (w->hasValue == CLOSURE_VALUE) {
        vargs[p++] = (vdynamic*)w->value;
    }

    /* Convert each argument from wrapper type to wrapped function type */
    for (i = 0; i < w->t->fun->nargs; i++) {
        hl_type *t = tfun->args[i];   /* Wrapper's arg type (what caller provides) */
        hl_type *to = w->t->fun->args[i];  /* Wrapped function's arg type (what we need) */
        void *v;

        /*
         * Extract the raw argument value from saved regs or stack.
         *
         * Our saved registers layout: [X0-X7][D0-D7] = 128 bytes
         * - regs[0..7] = X0-X7 (integer/pointer registers)
         * - regs[8..15] = D0-D7 (float registers, stored as doubles)
         *
         * For all types, we pass the address of the storage location.
         * The hl_dyn_cast* functions will dereference appropriately.
         */
        if (t->kind == HF32 || t->kind == HF64) {
            /* Float argument - read from FP registers (D0-D7) or stack */
            if (nextFpu < CALL_NREGS) {
                v = (void*)(regs + CALL_NREGS + nextFpu);
                nextFpu++;
            } else {
                v = stack_args;
                stack_args += 8;
            }
        } else {
            /* Integer/pointer argument - read from X1-X7 or stack */
            if (nextCpu < CALL_NREGS) {
                v = (void*)(regs + nextCpu);
                nextCpu++;
            } else {
                v = stack_args;
                stack_args += 8;
            }
        }

        /* Convert from source type to target type */
        switch (to->kind) {
        case HUI8:
        case HUI16:
        case HI32:
        case HBOOL:
            tmp[i].i = hl_dyn_casti(v, t, to);
            v = &tmp[i].i;
            break;
        case HI64:
        case HGUID:
            tmp[i].i64 = hl_dyn_casti64(v, t);
            v = &tmp[i].i64;
            break;
        case HF32:
            tmp[i].f = hl_dyn_castf(v, t);
            v = &tmp[i].f;
            break;
        case HF64:
            tmp[i].d = hl_dyn_castd(v, t);
            v = &tmp[i].d;
            break;
        default:
            v = hl_dyn_castp(v, t, to);
            break;
        }
        vargs[p++] = v;
    }

    /*
     * Call the wrapped function.
     *
     * For CLOSURE_WRAPPER (nested wrappers), we need special handling:
     * The inner wrapper's trampoline expects X0 = wrapper pointer, but this
     * isn't part of the formal type. We call the trampoline directly with
     * X0 set to the wrapper, and the args placed in subsequent registers.
     *
     * For other cases, use static_call with appropriate type:
     * - CLOSURE_NONE: use w->t directly
     * - CLOSURE_VALUE: use w->t->fun->parent (includes captured value as first arg)
     */
    if (w->hasValue == CLOSURE_WRAPPER) {
        /*
         * Nested wrapper call: set up registers manually and call trampoline.
         * X0 = wrapper, then args according to wrapper's type.
         */
        unsigned char call_regs[MAX_ARGS * 16];
        int cpu_idx = 0, fpu_idx = 0;
        int stack_pos = 128;  /* After register save area */
        hl_type *arg_type;
        void *dest;

        memset(call_regs, 0, sizeof(call_regs));

        /* X0 = the inner wrapper */
        *(void**)(call_regs + 0) = w;
        cpu_idx = 1;

        /* Place converted args in registers/stack */
        for (i = 0; i < p; i++) {
            arg_type = w->t->fun->args[i];

            if (arg_type->kind == HF32 || arg_type->kind == HF64) {
                if (fpu_idx < CALL_NREGS) {
                    dest = call_regs + (CALL_NREGS + fpu_idx) * 8;
                    fpu_idx++;
                } else {
                    dest = call_regs + stack_pos;
                    stack_pos += 8;
                }
                /* vargs[i] points to a float/double value */
                if (arg_type->kind == HF32)
                    *(float*)dest = *(float*)vargs[i];
                else
                    *(double*)dest = *(double*)vargs[i];
            } else {
                if (cpu_idx < CALL_NREGS) {
                    dest = call_regs + cpu_idx * 8;
                    cpu_idx++;
                } else {
                    dest = call_regs + stack_pos;
                    stack_pos += 8;
                }
                /* For primitives, vargs[i] points to the value; for pointers, it IS the value */
                switch (arg_type->kind) {
                case HUI8:
                case HBOOL:
                    *(int_val*)dest = *(unsigned char*)vargs[i];
                    break;
                case HUI16:
                    *(int_val*)dest = *(unsigned short*)vargs[i];
                    break;
                case HI32:
                    *(int_val*)dest = *(int*)vargs[i];
                    break;
                case HI64:
                case HGUID:
                    *(int_val*)dest = *(int_val*)vargs[i];
                    break;
                default:
                    /* Pointer type - vargs[i] IS the pointer */
                    *(void**)dest = vargs[i];
                    break;
                }
            }
        }

        /* Call the trampoline directly */
        switch (w->t->fun->ret->kind) {
        case HVOID:
            aot_c2hl_trampoline(w->fun, call_regs, call_regs + stack_pos);
            pret = NULL;
            break;
        case HF32:
        case HF64:
            {
                double (*dp)(void*,void*,void*) = (double(*)(void*,void*,void*))(void*)aot_c2hl_trampoline;
                ret->v.d = dp(w->fun, call_regs, call_regs + stack_pos);
                pret = &ret->v.d;
            }
            break;
        default:
            {
                void *raw = aot_c2hl_trampoline(w->fun, call_regs, call_regs + stack_pos);
                /* For primitives, store in ret buffer and return pointer to it.
                 * For pointers, return the raw pointer directly.
                 * This matches the convention used by aot_callback_c2hl. */
                switch (w->t->fun->ret->kind) {
                case HUI8:
                case HBOOL:
                case HUI16:
                case HI32:
                    ret->v.i = (int)(int_val)raw;
                    pret = &ret->v.i;
                    break;
                case HI64:
                case HGUID:
                    ret->v.i64 = (int_val)raw;
                    pret = &ret->v.i64;
                    break;
                default:
                    /* Pointer types: return raw pointer */
                    pret = raw;
                    break;
                }
            }
            break;
        }
    } else {
        call_type = (w->hasValue == CLOSURE_VALUE) ? w->t->fun->parent : w->t;
        pret = hl_setup.static_call(
            hl_setup.static_call_ref ? &w->fun : w->fun,
            call_type,
            vargs, ret);
    }

do_return_conversion:
    /* Convert return value from wrapped type to wrapper's return type */
    aret = hl_is_ptr(w->t->fun->ret) ? &pret : pret;
    if (aret == NULL) aret = &pret;

    switch (tfun->ret->kind) {
    case HVOID:
        return NULL;
    case HUI8:
    case HUI16:
    case HI32:
    case HBOOL:
        /* Return the value itself (cast to void*), not a pointer to it */
        return (void*)(int_val)hl_dyn_casti(aret, w->t->fun->ret, tfun->ret);
    case HI64:
    case HGUID:
        return (void*)(int_val)hl_dyn_casti64(aret, w->t->fun->ret);
    case HF32:
        ret->v.f = hl_dyn_castf(aret, w->t->fun->ret);
        return pret;
    case HF64:
        ret->v.d = hl_dyn_castd(aret, w->t->fun->ret);
        return pret;
    default:
        return hl_dyn_castp(aret, w->t->fun->ret, tfun->ret);
    }
}

/*
 * Wrapper for pointer-returning HL->C calls.
 * NOTE: These must NOT be static - the assembly trampoline calls them via 'bl'.
 */
void *aot_wrapper_ptr(vclosure_wrapper *c, char *stack_args, void **regs) {
    vdynamic out;
    return aot_wrapper_call_impl(c, stack_args, regs, &out);
}

/*
 * Wrapper for float-returning HL->C calls.
 * Returns the result in D0 (double).
 */
double aot_wrapper_d(vclosure_wrapper *c, char *stack_args, void **regs) {
    vdynamic out;
    aot_wrapper_call_impl(c, stack_args, regs, &out);
    return out.v.d;
}

/*
 * HL-to-C trampoline for AArch64.
 *
 * Entry point when a wrapped closure is called with native calling convention.
 * Saves all argument registers, inspects return type, and dispatches to the
 * appropriate wrapper function.
 *
 * Register layout on entry:
 *   X0 = vclosure_wrapper* (the wrapper closure)
 *   X1-X7 = integer arguments 1-7
 *   D0-D7 = float arguments 0-7
 *
 * Stack layout created:
 *   [SP + 0..63]   = X0-X7 (saved integer regs)
 *   [SP + 64..127] = D0-D7 (saved FP regs)
 */
#if defined(__aarch64__)
__asm__ (
    ".global aot_hl2c_trampoline\n"
    ".type aot_hl2c_trampoline, %function\n"
    "aot_hl2c_trampoline:\n"
    /* Save frame */
    "stp x29, x30, [sp, #-16]!\n"
    "mov x29, sp\n"

    /* Allocate 128 bytes for saved registers */
    "sub sp, sp, #128\n"

    /* X0 = vclosure_wrapper*, save to X9 */
    "mov x9, x0\n"

    /* Save integer argument registers X0-X7 at [SP, #0..63] */
    "stp x0, x1, [sp, #0]\n"
    "stp x2, x3, [sp, #16]\n"
    "stp x4, x5, [sp, #32]\n"
    "stp x6, x7, [sp, #48]\n"

    /* Save FP argument registers D0-D7 at [SP, #64..127] */
    "stp d0, d1, [sp, #64]\n"
    "stp d2, d3, [sp, #80]\n"
    "stp d4, d5, [sp, #96]\n"
    "stp d6, d7, [sp, #112]\n"

    /* Get return type kind: closure->t->fun->ret->kind */
    /* vclosure.t is at offset 0 */
    "ldr x10, [x9]\n"           /* X10 = closure->t */
    /* hl_type.fun is at offset 8 (after kind + padding) */
    "ldr x10, [x10, #8]\n"      /* X10 = t->fun */
    /* hl_type_fun.ret is at offset 8 */
    "ldr x10, [x10, #8]\n"      /* X10 = fun->ret */
    /* hl_type.kind is at offset 0 */
    "ldr w10, [x10]\n"          /* W10 = ret->kind */

    /* Check for HF64 (5) or HF32 (4) */
    "cmp w10, #5\n"
    "b.eq .Laot_float\n"
    "cmp w10, #4\n"
    "b.eq .Laot_float\n"

    /* Integer/pointer path */
    "mov x0, x9\n"              /* arg0: closure wrapper */
    "add x1, x29, #16\n"        /* arg1: stack args (caller's stack frame) */
    "mov x2, sp\n"              /* arg2: saved regs buffer */
    "bl aot_wrapper_ptr\n"
    "b .Laot_exit\n"

    ".Laot_float:\n"
    "mov x0, x9\n"
    "add x1, x29, #16\n"
    "mov x2, sp\n"
    "bl aot_wrapper_d\n"
    /* Result already in D0 */

    ".Laot_exit:\n"
    "mov sp, x29\n"
    "ldp x29, x30, [sp], #16\n"
    "ret\n"
);
extern void aot_hl2c_trampoline(void);
#endif

/*
 * Stack trace support for exceptions.
 * These mirror the hlc_* functions in hlc_main.c.
 */
static uchar *aot_resolve_symbol(void *addr, uchar *out, int *outSize) {
#if defined(HL_LINUX_BACKTRACE) || defined(HL_MAC)
    void *array[1];
    char **strings;
    array[0] = addr;
    strings = backtrace_symbols(array, 1);
    if (strings != NULL) {
        *outSize = (int)strlen(strings[0]) << 1;
        out = (uchar*)hl_gc_alloc_noptr(*outSize);
        hl_from_utf8(out, *outSize, strings[0]);
        free(strings);
        return out;
    }
#else
    (void)addr;
    (void)out;
    (void)outSize;
#endif
    return NULL;
}

static int aot_capture_stack(void **stack, int size) {
    int count = 0;
#if defined(HL_LINUX_BACKTRACE) || defined(HL_MAC)
    /* Force return total count when output stack is null */
    static void *tmpstack[HL_EXC_MAX_STACK];
    if (stack == NULL) {
        stack = tmpstack;
        size = HL_EXC_MAX_STACK;
    }
#   if defined(HL_LINUX_BACKTRACE)
    count = backtrace(stack, size) - 8;
#   elif defined(HL_MAC)
    count = backtrace(stack, size) - 6;
#   endif
    if (count < 0) count = 0;
#else
    (void)stack;
    (void)size;
#endif
    return count;
}

/*
 * Get wrapper function for a given type.
 * Returns the HL-to-C trampoline that handles all function signature conversions.
 */
static void *aot_get_wrapper(hl_type *t) {
    (void)t;
#if defined(__aarch64__)
    return (void*)aot_hl2c_trampoline;
#else
    /* x86_64 support can be added later */
    return NULL;
#endif
}

/* Export wrapper functions that can be linked */
static hl_code *aot_code_read(const char *path, char **error_msg) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (error_msg) *error_msg = "Cannot open file";
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    int size = (int)ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = (unsigned char *)malloc(size);
    if (fread(data, 1, size, f) != (size_t)size) {
        free(data);
        fclose(f);
        if (error_msg) *error_msg = "Failed to read file";
        return NULL;
    }
    fclose(f);
    hl_code *code = hl_code_read(data, size, error_msg);
    free(data);
    return code;
}

/*
 * AOT function pointer table - generated by LLVM codegen.
 * Maps findex -> function pointer for HL functions.
 * Native function entries are NULL (resolved by hl_module_init).
 */
extern void *aot_function_table[];
extern int aot_function_count;

/*
 * Initialize module from embedded bytecode data.
 *
 * AOT-compiled binaries embed the .hl bytecode directly in the executable
 * as a global byte array. This allows the binary to be fully standalone
 * without needing the original .hl file at runtime.
 *
 * The bytecode is needed at runtime because it contains type metadata
 * (hl_type structures, vtables, field layouts) that the runtime uses for:
 *   - Object allocation (needs type size and layout)
 *   - Type casting and runtime type checks
 *   - Dynamic dispatch through vtables
 *   - Field access offset calculations
 *
 * While the function *code* is AOT-compiled to native instructions,
 * the type *metadata* is loaded from the embedded bytecode and used
 * to initialize the runtime type system.
 */
int aot_init_module_data(const unsigned char *data, int size) {
    char *error_msg = NULL;
    hl_code *code;

    /* Set up signal handlers early */
    aot_setup_signals();

    code = hl_code_read(data, size, &error_msg);
    if (!code) {
        fprintf(stderr, "Failed to read embedded HL code: %s\n", error_msg ? error_msg : "unknown error");
        return 0;
    }
    hl_module *m = hl_module_alloc(code);
    if (!m) {
        fprintf(stderr, "Failed to allocate module\n");
        return 0;
    }
    if (!hl_module_init(m, 0)) {
        fprintf(stderr, "Failed to initialize module\n");
        return 0;
    }

    /*
     * Cache all strings that may be used as dynamic field names.
     * The LLVM backend bakes field name hashes into the code, but at runtime
     * hl_field_name() needs to reverse-lookup the name from the hash using
     * the global hash cache. The JIT calls hl_hash_gen(..., true) at compile
     * time which runs in the same process, so the cache is populated. But AOT
     * compilation happens in a separate process, so we must cache here.
     */
    for (int i = 0; i < code->nstrings; i++) {
        hl_hash_gen(hl_get_ustring(code, i), true);
    }

    /*
     * Export the types and globals arrays for AOT code to access.
     * After hl_module_init(), all types have their runtime info (obj->rt)
     * properly initialized, and globals are allocated/initialized.
     */
    aot_types = code->types;
    aot_ntypes = code->ntypes;

    aot_globals_data = m->globals_data;
    aot_globals_indexes = m->globals_indexes;
    aot_nglobals = code->nglobals;

    /*
     * Patch functions_ptrs with AOT-compiled function addresses.
     *
     * After hl_module_init(), m->functions_ptrs contains:
     *   - For natives: properly resolved function pointers from shared libraries
     *   - For HL functions: pointers to dummy_code (useless)
     *
     * We patch the HL function entries with the actual AOT-compiled addresses
     * from aot_function_table. Native entries (NULL in the table) are left alone.
     *
     * This is critical for closures and method dispatch to work correctly.
     */
    for (int i = 0; i < aot_function_count; i++) {
        if (aot_function_table[i] != NULL) {
            m->functions_ptrs[i] = aot_function_table[i];
        }
    }

    /*
     * Refresh cached method pointers in vobj_proto and rt->methods.
     *
     * hl_get_obj_proto may have been called during hl_module_init (e.g., when
     * initializing constants that allocate objects). At that point, functions_ptrs
     * contained dummy values from our fake JIT. Now that we've patched the real
     * AOT function addresses, we need to update the cached method pointers.
     */
    for (int i = 0; i < code->ntypes; i++) {
        hl_type *t = &code->types[i];
        if ((t->kind == HOBJ || t->kind == HSTRUCT) && t->obj) {
            hl_type_obj *o = t->obj;

            /* Refresh vobj_proto (vtable for OCallMethod) */
            if (t->vobj_proto && t->vobj_proto != (void*)1) {
                void **fptr = (void**)t->vobj_proto;
                for (int j = 0; j < o->nproto; j++) {
                    hl_obj_proto *p = &o->proto[j];
                    if (p->pindex >= 0) {
                        fptr[p->pindex] = m->functions_ptrs[p->findex];
                    }
                }
            }

            /* Refresh rt->methods (for hl_dyn_call_obj dynamic dispatch) */
            hl_runtime_obj *rt = o->rt;
            if (rt && rt->methods) {
                /* Re-apply method pointers from nproto entries.
                 * The method_index calculation mirrors hl_get_obj_proto logic. */
                hl_runtime_obj *parent_rt = rt->parent;
                int nmethods = parent_rt ? parent_rt->nmethods : 0;
                for (int j = 0; j < o->nproto; j++) {
                    hl_obj_proto *pr = &o->proto[j];
                    int method_index;
                    if (parent_rt) {
                        if (pr->pindex >= 0 && pr->pindex < parent_rt->nproto) {
                            /* Overriding parent method - find via lookup */
                            hl_field_lookup *l = hl_lookup_find(parent_rt->lookup, parent_rt->nlookup, pr->hashed_name);
                            if (l && l->field_index < 0) {
                                method_index = -l->field_index - 1;
                            } else {
                                method_index = nmethods++;
                            }
                        } else {
                            method_index = nmethods++;
                        }
                    } else {
                        method_index = nmethods++;
                    }
                    if (method_index < rt->nmethods) {
                        rt->methods[method_index] = m->functions_ptrs[pr->findex];
                    }
                }
            }
        }
    }

    /*
     * Set up callbacks for dynamic function calls.
     *
     * hl_dyn_call and hl_call_method use hlc_static_call to invoke functions
     * dynamically. We provide our AOT-compatible callback that uses a trampoline
     * to call AOT functions with the correct calling convention.
     *
     * static_call_ref=true tells the runtime to pass &cl->fun (address of function pointer).
     * Our callback_c2hl then dereferences it with *f to get the actual function pointer.
     */
    hl_setup.get_wrapper = aot_get_wrapper;
    hl_setup.static_call = aot_callback_c2hl;
    hl_setup.static_call_ref = true;

    /*
     * Set up stack trace support for exceptions.
     * These functions are used by hl_throw to capture and resolve stack traces.
     */
    hl_setup.resolve_symbol = aot_resolve_symbol;
    hl_setup.capture_stack = aot_capture_stack;

    return 1;
}
