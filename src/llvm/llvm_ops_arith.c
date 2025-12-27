/*
 * Copyright (C)2005-2016 Haxe Foundation
 * LLVM Backend - Arithmetic and Logic Opcodes
 */
#include "llvm_codegen.h"

/* Apply fast-math flags to an FP instruction if enabled.
 * safe mode (1): reassoc + contract + arcp + afn (preserves NaN/Inf/signed-zero semantics)
 * full mode (2): all flags including nnan/ninf/nsz (not IEEE 754 compliant) */
static inline void apply_fast_math(llvm_ctx *ctx, LLVMValueRef val) {
    if (ctx->fast_math && LLVMCanValueUseFastMathFlags(val)) {
        if (ctx->fast_math == 1) {
            /* Safe: allow optimizations that don't break NaN/Inf handling */
            LLVMSetFastMathFlags(val,
                LLVMFastMathAllowReassoc |
                LLVMFastMathAllowContract |
                LLVMFastMathAllowReciprocal |
                LLVMFastMathApproxFunc);
        } else {
            /* Full: all optimizations, assumes no NaN/Inf/signed-zeros */
            LLVMSetFastMathFlags(val, LLVMFastMathAll);
        }
    }
}

void llvm_emit_arithmetic(llvm_ctx *ctx, hl_function *f, hl_opcode *op, int op_idx) {
    switch (op->op) {
    case OAdd: {
        /* dst = a + b */
        int dst = op->p1;
        int ra = op->p2;
        int rb = op->p3;
        hl_type *t = f->regs[dst];
        LLVMValueRef a = llvm_load_vreg(ctx, f, ra);
        LLVMValueRef b = llvm_load_vreg(ctx, f, rb);
        LLVMValueRef result;
        if (llvm_is_float_type(t)) {
            result = LLVMBuildFAdd(ctx->builder, a, b, "");
            apply_fast_math(ctx, result);
        } else {
            result = LLVMBuildAdd(ctx->builder, a, b, "");
        }
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case OSub: {
        /* dst = a - b */
        int dst = op->p1;
        int ra = op->p2;
        int rb = op->p3;
        hl_type *t = f->regs[dst];
        LLVMValueRef a = llvm_load_vreg(ctx, f, ra);
        LLVMValueRef b = llvm_load_vreg(ctx, f, rb);
        LLVMValueRef result;
        if (llvm_is_float_type(t)) {
            result = LLVMBuildFSub(ctx->builder, a, b, "");
            apply_fast_math(ctx, result);
        } else {
            result = LLVMBuildSub(ctx->builder, a, b, "");
        }
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case OMul: {
        /* dst = a * b */
        int dst = op->p1;
        int ra = op->p2;
        int rb = op->p3;
        hl_type *t = f->regs[dst];
        LLVMValueRef a = llvm_load_vreg(ctx, f, ra);
        LLVMValueRef b = llvm_load_vreg(ctx, f, rb);
        LLVMValueRef result;
        if (llvm_is_float_type(t)) {
            result = LLVMBuildFMul(ctx->builder, a, b, "");
            apply_fast_math(ctx, result);
        } else {
            result = LLVMBuildMul(ctx->builder, a, b, "");
        }
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case OSDiv: {
        /* dst = a / b (signed, but use unsigned for UI8/UI16)
         * Special cases matching JIT behavior:
         * - divisor == 0: return 0 (a * 0)
         * - divisor == -1: return a * -1 (avoids INT_MIN / -1 overflow) */
        int dst = op->p1;
        int ra = op->p2;
        int rb = op->p3;
        hl_type *t = f->regs[dst];
        LLVMValueRef a = llvm_load_vreg(ctx, f, ra);
        LLVMValueRef b = llvm_load_vreg(ctx, f, rb);
        LLVMValueRef result;
        if (llvm_is_float_type(t)) {
            result = LLVMBuildFDiv(ctx->builder, a, b, "");
            apply_fast_math(ctx, result);
        } else if (t->kind == HUI8 || t->kind == HUI16) {
            /* Unsigned types need unsigned division, check for 0 */
            LLVMValueRef zero = LLVMConstInt(LLVMTypeOf(b), 0, 0);
            LLVMValueRef is_zero = LLVMBuildICmp(ctx->builder, LLVMIntEQ, b, zero, "");
            LLVMValueRef safe_b = LLVMBuildSelect(ctx->builder, is_zero,
                LLVMConstInt(LLVMTypeOf(b), 1, 0), b, "");
            LLVMValueRef div = LLVMBuildUDiv(ctx->builder, a, safe_b, "");
            result = LLVMBuildSelect(ctx->builder, is_zero, zero, div, "");
        } else {
            /* Signed: check for 0 or -1. For both, return a * b:
             * - a * 0 = 0
             * - a * -1 = -a (with correct two's complement wrap for INT_MIN) */
            LLVMValueRef zero = LLVMConstInt(LLVMTypeOf(b), 0, 0);
            LLVMValueRef neg1 = LLVMConstInt(LLVMTypeOf(b), -1, 1);
            LLVMValueRef is_zero = LLVMBuildICmp(ctx->builder, LLVMIntEQ, b, zero, "");
            LLVMValueRef is_neg1 = LLVMBuildICmp(ctx->builder, LLVMIntEQ, b, neg1, "");
            LLVMValueRef is_special = LLVMBuildOr(ctx->builder, is_zero, is_neg1, "");
            /* Use 1 as safe divisor to avoid UB */
            LLVMValueRef safe_b = LLVMBuildSelect(ctx->builder, is_special,
                LLVMConstInt(LLVMTypeOf(b), 1, 0), b, "");
            LLVMValueRef div = LLVMBuildSDiv(ctx->builder, a, safe_b, "");
            LLVMValueRef mul = LLVMBuildMul(ctx->builder, a, b, "");
            result = LLVMBuildSelect(ctx->builder, is_special, mul, div, "");
        }
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case OUDiv: {
        /* dst = a / b (unsigned)
         * Special case: divisor == 0 returns 0 */
        int dst = op->p1;
        int ra = op->p2;
        int rb = op->p3;
        LLVMValueRef a = llvm_load_vreg(ctx, f, ra);
        LLVMValueRef b = llvm_load_vreg(ctx, f, rb);
        LLVMValueRef zero = LLVMConstInt(LLVMTypeOf(b), 0, 0);
        LLVMValueRef is_zero = LLVMBuildICmp(ctx->builder, LLVMIntEQ, b, zero, "");
        LLVMValueRef safe_b = LLVMBuildSelect(ctx->builder, is_zero,
            LLVMConstInt(LLVMTypeOf(b), 1, 0), b, "");
        LLVMValueRef div = LLVMBuildUDiv(ctx->builder, a, safe_b, "");
        LLVMValueRef result = LLVMBuildSelect(ctx->builder, is_zero, zero, div, "");
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case OSMod: {
        /* dst = a % b (signed, but use unsigned for UI8/UI16)
         * Special cases matching JIT behavior:
         * - divisor == 0: return 0 (avoid undefined behavior)
         * - divisor == -1: return 0 (avoid MIN % -1 overflow) */
        int dst = op->p1;
        int ra = op->p2;
        int rb = op->p3;
        hl_type *t = f->regs[dst];
        LLVMValueRef a = llvm_load_vreg(ctx, f, ra);
        LLVMValueRef b = llvm_load_vreg(ctx, f, rb);
        LLVMValueRef result;
        if (llvm_is_float_type(t)) {
            result = LLVMBuildFRem(ctx->builder, a, b, "");
            apply_fast_math(ctx, result);
        } else if (t->kind == HUI8 || t->kind == HUI16) {
            /* Unsigned types need unsigned remainder, and divisor can't be -1 */
            LLVMValueRef zero = LLVMConstInt(LLVMTypeOf(b), 0, 0);
            LLVMValueRef is_zero = LLVMBuildICmp(ctx->builder, LLVMIntEQ, b, zero, "");
            LLVMValueRef safe_b = LLVMBuildSelect(ctx->builder, is_zero,
                LLVMConstInt(LLVMTypeOf(b), 1, 0), b, "");
            LLVMValueRef rem = LLVMBuildURem(ctx->builder, a, safe_b, "");
            result = LLVMBuildSelect(ctx->builder, is_zero, zero, rem, "");
        } else {
            /* Signed: check for 0 or -1 */
            LLVMValueRef zero = LLVMConstInt(LLVMTypeOf(b), 0, 0);
            LLVMValueRef neg1 = LLVMConstInt(LLVMTypeOf(b), -1, 1);
            LLVMValueRef is_zero = LLVMBuildICmp(ctx->builder, LLVMIntEQ, b, zero, "");
            LLVMValueRef is_neg1 = LLVMBuildICmp(ctx->builder, LLVMIntEQ, b, neg1, "");
            LLVMValueRef is_special = LLVMBuildOr(ctx->builder, is_zero, is_neg1, "");
            /* Use 1 as safe divisor to avoid UB, then select 0 if special case */
            LLVMValueRef safe_b = LLVMBuildSelect(ctx->builder, is_special,
                LLVMConstInt(LLVMTypeOf(b), 1, 0), b, "");
            LLVMValueRef rem = LLVMBuildSRem(ctx->builder, a, safe_b, "");
            result = LLVMBuildSelect(ctx->builder, is_special, zero, rem, "");
        }
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case OUMod: {
        /* dst = a % b (unsigned)
         * Special case: divisor == 0 returns 0 */
        int dst = op->p1;
        int ra = op->p2;
        int rb = op->p3;
        LLVMValueRef a = llvm_load_vreg(ctx, f, ra);
        LLVMValueRef b = llvm_load_vreg(ctx, f, rb);
        LLVMValueRef zero = LLVMConstInt(LLVMTypeOf(b), 0, 0);
        LLVMValueRef is_zero = LLVMBuildICmp(ctx->builder, LLVMIntEQ, b, zero, "");
        /* Use 1 as safe divisor to avoid UB, then select 0 if zero */
        LLVMValueRef safe_b = LLVMBuildSelect(ctx->builder, is_zero,
            LLVMConstInt(LLVMTypeOf(b), 1, 0), b, "");
        LLVMValueRef rem = LLVMBuildURem(ctx->builder, a, safe_b, "");
        LLVMValueRef result = LLVMBuildSelect(ctx->builder, is_zero, zero, rem, "");
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case OShl: {
        /* dst = a << b
         * LLVM shift by >= bit width is UB, but x86/ARM mask the shift amount.
         * HashLink expects x86 semantics, so we must mask the shift amount. */
        int dst = op->p1;
        int ra = op->p2;
        int rb = op->p3;
        hl_type *t = f->regs[ra];
        LLVMValueRef a = llvm_load_vreg(ctx, f, ra);
        LLVMValueRef b = llvm_load_vreg(ctx, f, rb);
        /* Mask shift amount: 31 for i32, 63 for i64 */
        int mask = (t->kind == HI64) ? 63 : 31;
        LLVMValueRef mask_val = LLVMConstInt(LLVMTypeOf(b), mask, false);
        LLVMValueRef masked_b = LLVMBuildAnd(ctx->builder, b, mask_val, "");
        LLVMValueRef result = LLVMBuildShl(ctx->builder, a, masked_b, "");
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case OSShr: {
        /* dst = a >> b (signed/arithmetic)
         * Mask shift amount for same reason as OShl. */
        int dst = op->p1;
        int ra = op->p2;
        int rb = op->p3;
        hl_type *t = f->regs[ra];
        LLVMValueRef a = llvm_load_vreg(ctx, f, ra);
        LLVMValueRef b = llvm_load_vreg(ctx, f, rb);
        int mask = (t->kind == HI64) ? 63 : 31;
        LLVMValueRef mask_val = LLVMConstInt(LLVMTypeOf(b), mask, false);
        LLVMValueRef masked_b = LLVMBuildAnd(ctx->builder, b, mask_val, "");
        LLVMValueRef result = LLVMBuildAShr(ctx->builder, a, masked_b, "");
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case OUShr: {
        /* dst = a >>> b (unsigned/logical)
         * Mask shift amount for same reason as OShl. */
        int dst = op->p1;
        int ra = op->p2;
        int rb = op->p3;
        hl_type *t = f->regs[ra];
        LLVMValueRef a = llvm_load_vreg(ctx, f, ra);
        LLVMValueRef b = llvm_load_vreg(ctx, f, rb);
        int mask = (t->kind == HI64) ? 63 : 31;
        LLVMValueRef mask_val = LLVMConstInt(LLVMTypeOf(b), mask, false);
        LLVMValueRef masked_b = LLVMBuildAnd(ctx->builder, b, mask_val, "");
        LLVMValueRef result = LLVMBuildLShr(ctx->builder, a, masked_b, "");
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case OAnd: {
        /* dst = a & b */
        int dst = op->p1;
        int ra = op->p2;
        int rb = op->p3;
        LLVMValueRef a = llvm_load_vreg(ctx, f, ra);
        LLVMValueRef b = llvm_load_vreg(ctx, f, rb);
        LLVMValueRef result = LLVMBuildAnd(ctx->builder, a, b, "");
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case OOr: {
        /* dst = a | b */
        int dst = op->p1;
        int ra = op->p2;
        int rb = op->p3;
        LLVMValueRef a = llvm_load_vreg(ctx, f, ra);
        LLVMValueRef b = llvm_load_vreg(ctx, f, rb);
        LLVMValueRef result = LLVMBuildOr(ctx->builder, a, b, "");
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case OXor: {
        /* dst = a ^ b */
        int dst = op->p1;
        int ra = op->p2;
        int rb = op->p3;
        LLVMValueRef a = llvm_load_vreg(ctx, f, ra);
        LLVMValueRef b = llvm_load_vreg(ctx, f, rb);
        LLVMValueRef result = LLVMBuildXor(ctx->builder, a, b, "");
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case ONeg: {
        /* dst = -src */
        int dst = op->p1;
        int src = op->p2;
        hl_type *t = f->regs[dst];
        LLVMValueRef val = llvm_load_vreg(ctx, f, src);
        LLVMValueRef result;
        if (llvm_is_float_type(t)) {
            result = LLVMBuildFNeg(ctx->builder, val, "");
            apply_fast_math(ctx, result);
        } else {
            result = LLVMBuildNeg(ctx->builder, val, "");
        }
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case ONot: {
        /* dst = !src (logical/boolean NOT, XOR with 1) */
        int dst = op->p1;
        int src = op->p2;
        LLVMValueRef val = llvm_load_vreg(ctx, f, src);
        LLVMValueRef one = LLVMConstInt(LLVMTypeOf(val), 1, false);
        LLVMValueRef result = LLVMBuildXor(ctx->builder, val, one, "");
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case OIncr: {
        /* dst++ (in-place increment) */
        int dst = op->p1;
        hl_type *t = f->regs[dst];
        LLVMValueRef val = llvm_load_vreg(ctx, f, dst);
        LLVMValueRef result;
        if (llvm_is_float_type(t)) {
            LLVMValueRef one = LLVMConstReal(llvm_get_type(ctx, t), 1.0);
            result = LLVMBuildFAdd(ctx->builder, val, one, "");
            apply_fast_math(ctx, result);
        } else {
            LLVMValueRef one = LLVMConstInt(llvm_get_type(ctx, t), 1, false);
            result = LLVMBuildAdd(ctx->builder, val, one, "");
        }
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case ODecr: {
        /* dst-- (in-place decrement) */
        int dst = op->p1;
        hl_type *t = f->regs[dst];
        LLVMValueRef val = llvm_load_vreg(ctx, f, dst);
        LLVMValueRef result;
        if (llvm_is_float_type(t)) {
            LLVMValueRef one = LLVMConstReal(llvm_get_type(ctx, t), 1.0);
            result = LLVMBuildFSub(ctx->builder, val, one, "");
            apply_fast_math(ctx, result);
        } else {
            LLVMValueRef one = LLVMConstInt(llvm_get_type(ctx, t), 1, false);
            result = LLVMBuildSub(ctx->builder, val, one, "");
        }
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    default:
        break;
    }
}
