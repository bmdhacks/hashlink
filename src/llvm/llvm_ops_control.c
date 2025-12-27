/*
 * Copyright (C)2005-2016 Haxe Foundation
 * LLVM Backend - Control Flow Opcodes
 */
#include "llvm_codegen.h"

/* Apply fast-math flags to an FP instruction if enabled.
 * safe mode (1): reassoc + contract + arcp + afn (preserves NaN/Inf/signed-zero semantics)
 * full mode (2): all flags including nnan/ninf/nsz (not IEEE 754 compliant) */
static inline void apply_fast_math(llvm_ctx *ctx, LLVMValueRef val) {
    if (ctx->fast_math && LLVMCanValueUseFastMathFlags(val)) {
        if (ctx->fast_math == 1) {
            LLVMSetFastMathFlags(val,
                LLVMFastMathAllowReassoc |
                LLVMFastMathAllowContract |
                LLVMFastMathAllowReciprocal |
                LLVMFastMathApproxFunc);
        } else {
            LLVMSetFastMathFlags(val, LLVMFastMathAll);
        }
    }
}

/* Check if a type is the String type (HOBJ with bytes + length fields).
 * This matches the JIT's is_string_type() function. */
static bool is_string_type(hl_type *t) {
    if (t->kind != HOBJ || !t->obj) return false;
    if (t->obj->nfields != 2) return false;
    return t->obj->fields[0].t->kind == HBYTES &&
           t->obj->fields[1].t->kind == HI32;
}

/*
 * Get the LLVM type to use for loading HNULL inner values.
 * hl_make_dyn stores values as:
 * - UI8, UI16, I32, BOOL: stored as v.i (32-bit int)
 * - I64, GUID: stored as v.i64 (64-bit)
 * - F32: stored as v.f (32-bit float, but compare as i32)
 * - F64: stored as v.d (64-bit double, but compare as i64)
 * - Pointers: stored as v.ptr (64-bit)
 */
static LLVMTypeRef get_null_inner_load_type(llvm_ctx *ctx, hl_type *null_type) {
    if (null_type->kind != HNULL || !null_type->tparam)
        return ctx->i64_type;  /* Fallback to 64-bit */

    switch (null_type->tparam->kind) {
    case HUI8:
    case HUI16:
    case HI32:
    case HBOOL:
    case HF32:
        return ctx->i32_type;  /* 32-bit storage */
    default:
        return ctx->i64_type;  /* 64-bit storage (I64, F64, pointers, etc.) */
    }
}

/*
 * Unified helper for nullable pointer equality comparison with safe unwrapping.
 *
 * Compares two pointers that may be NULL, optionally unwrapping one or both
 * by loading from offset 8 (used by HNULL and HVIRTUAL types).
 *
 * Parameters:
 *   a, b: The pointers to compare
 *   unwrap_a: If true, compare a->value (loaded from a+8) instead of a
 *   unwrap_b: If true, compare b->value (loaded from b+8) instead of b
 *   load_type: Type to use for loads (ptr for HVIRTUAL, i32/i64 for HNULL)
 *   check_inner_null: If true, treat NULL inner values as not-equal (HVIRTUAL semantics)
 *
 * Semantics:
 *   - If a == b (pointer equality, including both NULL): return true
 *   - If exactly one is NULL: return false
 *   - Otherwise: unwrap as needed and compare values
 *   - If check_inner_null and either inner value is NULL: return false
 */
static LLVMValueRef build_nullable_ptr_eq(llvm_ctx *ctx,
                                           LLVMValueRef a, LLVMValueRef b,
                                           bool unwrap_a, bool unwrap_b,
                                           LLVMTypeRef load_type,
                                           bool check_inner_null) {
    LLVMValueRef null_ptr = LLVMConstNull(ctx->ptr_type);
    LLVMValueRef val_offset = LLVMConstInt(ctx->i64_type, 8, false);

    LLVMValueRef result_alloca = llvm_create_entry_alloca(ctx, ctx->i1_type, "ptr_eq_result");
    LLVMBasicBlockRef current_bb = LLVMGetInsertBlock(ctx->builder);
    LLVMValueRef func = LLVMGetBasicBlockParent(current_bb);

    LLVMBasicBlockRef ptr_eq_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "peq_same");
    LLVMBasicBlockRef check_a_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "peq_check_a");
    LLVMBasicBlockRef check_b_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "peq_check_b");
    LLVMBasicBlockRef compare_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "peq_compare");
    LLVMBasicBlockRef not_eq_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "peq_false");
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(ctx->context, func, "peq_merge");

    /* First check: a == b (pointer equality, handles both-NULL case) */
    LLVMValueRef ptr_eq = LLVMBuildICmp(ctx->builder, LLVMIntEQ, a, b, "");
    LLVMBuildCondBr(ctx->builder, ptr_eq, ptr_eq_bb, check_a_bb);

    /* ptr_eq_bb: pointers are equal, result is true */
    LLVMPositionBuilderAtEnd(ctx->builder, ptr_eq_bb);
    LLVMBuildStore(ctx->builder, LLVMConstInt(ctx->i1_type, 1, false), result_alloca);
    LLVMBuildBr(ctx->builder, merge_bb);

    /* check_a_bb: check if a is NULL */
    LLVMPositionBuilderAtEnd(ctx->builder, check_a_bb);
    LLVMValueRef a_is_null = LLVMBuildICmp(ctx->builder, LLVMIntEQ, a, null_ptr, "");
    LLVMBuildCondBr(ctx->builder, a_is_null, not_eq_bb, check_b_bb);

    /* check_b_bb: check if b is NULL */
    LLVMPositionBuilderAtEnd(ctx->builder, check_b_bb);
    LLVMValueRef b_is_null = LLVMBuildICmp(ctx->builder, LLVMIntEQ, b, null_ptr, "");
    LLVMBuildCondBr(ctx->builder, b_is_null, not_eq_bb, compare_bb);

    /* compare_bb: both non-null, unwrap and compare */
    LLVMPositionBuilderAtEnd(ctx->builder, compare_bb);
    LLVMValueRef a_cmp = a;
    LLVMValueRef b_cmp = b;

    if (unwrap_a) {
        LLVMValueRef a_val_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type, a, &val_offset, 1, "");
        a_cmp = LLVMBuildLoad2(ctx->builder, load_type, a_val_ptr, "a_inner");
    }
    if (unwrap_b) {
        LLVMValueRef b_val_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type, b, &val_offset, 1, "");
        b_cmp = LLVMBuildLoad2(ctx->builder, load_type, b_val_ptr, "b_inner");
    }

    LLVMValueRef val_eq = LLVMBuildICmp(ctx->builder, LLVMIntEQ, a_cmp, b_cmp, "");

    if (check_inner_null && (unwrap_a || unwrap_b)) {
        /* For HVIRTUAL: if either inner value is NULL, not equal */
        LLVMValueRef either_null = LLVMConstInt(ctx->i1_type, 0, false);
        if (unwrap_a) {
            LLVMValueRef a_null = LLVMBuildICmp(ctx->builder, LLVMIntEQ, a_cmp, null_ptr, "");
            either_null = LLVMBuildOr(ctx->builder, either_null, a_null, "");
        }
        if (unwrap_b) {
            LLVMValueRef b_null = LLVMBuildICmp(ctx->builder, LLVMIntEQ, b_cmp, null_ptr, "");
            either_null = LLVMBuildOr(ctx->builder, either_null, b_null, "");
        }
        val_eq = LLVMBuildSelect(ctx->builder, either_null,
            LLVMConstInt(ctx->i1_type, 0, false), val_eq, "");
    }

    LLVMBuildStore(ctx->builder, val_eq, result_alloca);
    LLVMBuildBr(ctx->builder, merge_bb);

    /* not_eq_bb: one is null and other isn't, result is false */
    LLVMPositionBuilderAtEnd(ctx->builder, not_eq_bb);
    LLVMBuildStore(ctx->builder, LLVMConstInt(ctx->i1_type, 0, false), result_alloca);
    LLVMBuildBr(ctx->builder, merge_bb);

    /* merge_bb: load and return result */
    LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);
    return LLVMBuildLoad2(ctx->builder, ctx->i1_type, result_alloca, "ptr_eq");
}

/*
 * Build HNULL (Null<T>) equality comparison.
 * HNULL wraps values at offset 8 (HDYN_VALUE).
 * Semantics: if (a == b || (a && b && a->v == b->v))
 */
static LLVMValueRef build_null_eq(llvm_ctx *ctx, LLVMValueRef a, LLVMValueRef b,
                                   hl_type *ta, hl_type *tb) {
    /* Determine load type based on inner type */
    LLVMTypeRef load_type = ctx->i64_type;
    if (ta->kind == HNULL)
        load_type = get_null_inner_load_type(ctx, ta);
    else if (tb->kind == HNULL)
        load_type = get_null_inner_load_type(ctx, tb);

    return build_nullable_ptr_eq(ctx, a, b, true, true, load_type, false);
}

/*
 * Build HVIRTUAL equality comparison.
 * vvirtual has a 'value' pointer at offset 8 pointing to the underlying object.
 */
static LLVMValueRef build_virtual_eq(llvm_ctx *ctx, LLVMValueRef a, LLVMValueRef b,
                                      hl_type *ta, hl_type *tb) {
    if (ta->kind == HVIRTUAL && tb->kind != HVIRTUAL) {
        /* HVIRTUAL vs HOBJ: compare a->value with b */
        return build_nullable_ptr_eq(ctx, a, b, true, false, ctx->ptr_type, false);
    } else if (tb->kind == HVIRTUAL && ta->kind != HVIRTUAL) {
        /* HOBJ vs HVIRTUAL: compare a with b->value */
        return build_nullable_ptr_eq(ctx, a, b, false, true, ctx->ptr_type, false);
    } else {
        /* Both HVIRTUAL: compare a->value with b->value, check for null inner values */
        return build_nullable_ptr_eq(ctx, a, b, true, true, ctx->ptr_type, true);
    }
}

/*
 * Emit a comparison jump for all comparison opcodes.
 * Handles: OJEq, OJNotEq, OJSLt, OJSGte, OJSGt, OJSLte, OJULt, OJUGte, OJNotLt, OJNotGte
 *
 * For equality (OJEq/OJNotEq): handles HVIRTUAL unwrapping, HDYN/HFUN/HNULL, strings.
 * For ordering comparisons: handles floats, strings (via dyn_compare), integers.
 */
static void emit_cmp_jump(llvm_ctx *ctx, hl_function *f, hl_opcode *op, int op_idx,
                          LLVMIntPredicate int_pred, LLVMRealPredicate float_pred) {
    int ra = op->p1;
    int rb = op->p2;
    int offset = op->p3;
    hl_type *ta = f->regs[ra];
    hl_type *tb = f->regs[rb];
    LLVMValueRef a = llvm_load_vreg(ctx, f, ra);
    LLVMValueRef b = llvm_load_vreg(ctx, f, rb);
    LLVMValueRef cmp;

    bool is_equality = (int_pred == LLVMIntEQ || int_pred == LLVMIntNE);

    if (llvm_is_float_type(ta)) {
        cmp = LLVMBuildFCmp(ctx->builder, float_pred, a, b, "");
        apply_fast_math(ctx, cmp);
    } else if (is_equality && (ta->kind == HNULL || tb->kind == HNULL)) {
        /* HNULL (Null<T>) equality: load inner values from offset 8 and compare.
         * This matches JIT semantics which directly compares values at offset 8,
         * avoiding type-dispatch issues in hl_dyn_compare.
         * Uses appropriate load size based on inner type to avoid garbage bits. */
        cmp = build_null_eq(ctx, a, b, ta, tb);
        if (int_pred == LLVMIntNE) cmp = LLVMBuildNot(ctx->builder, cmp, "");
    } else if (ta->kind == HDYN || tb->kind == HDYN ||
               ta->kind == HFUN || tb->kind == HFUN) {
        /* HDYN/HFUN types require runtime comparison via hl_dyn_compare. */
        LLVMValueRef args[] = { a, b };
        LLVMValueRef result = LLVMBuildCall2(ctx->builder,
            LLVMGlobalGetValueType(ctx->rt_dyn_compare),
            ctx->rt_dyn_compare, args, 2, "");
        LLVMValueRef zero = LLVMConstInt(ctx->i32_type, 0, false);
        cmp = LLVMBuildICmp(ctx->builder, int_pred, result, zero, "");
    } else if (is_string_type(ta) && is_string_type(tb)) {
        /* String comparison via hl_dyn_compare (returns ordering: -1, 0, 1) */
        LLVMValueRef args[] = { a, b };
        LLVMValueRef result = LLVMBuildCall2(ctx->builder,
            LLVMGlobalGetValueType(ctx->rt_dyn_compare),
            ctx->rt_dyn_compare, args, 2, "");
        LLVMValueRef zero = LLVMConstInt(ctx->i32_type, 0, false);
        cmp = LLVMBuildICmp(ctx->builder, int_pred, result, zero, "");
    } else if (is_equality && (ta->kind == HVIRTUAL || tb->kind == HVIRTUAL)) {
        /* HVIRTUAL equality: unwrap and compare underlying values */
        cmp = build_virtual_eq(ctx, a, b, ta, tb);
        if (int_pred == LLVMIntNE) cmp = LLVMBuildNot(ctx->builder, cmp, "");
    } else {
        cmp = LLVMBuildICmp(ctx->builder, int_pred, a, b, "");
    }

    LLVMBasicBlockRef then_bb = llvm_get_block_for_offset(ctx, op_idx, offset);
    LLVMBasicBlockRef else_bb = llvm_get_block_for_offset(ctx, op_idx, 0);
    if (then_bb && else_bb) {
        LLVMBuildCondBr(ctx->builder, cmp, then_bb, else_bb);
    }
}

void llvm_emit_control_flow(llvm_ctx *ctx, hl_function *f, hl_opcode *op, int op_idx) {
    switch (op->op) {
    case OLabel: {
        /* Label is handled by basic block creation in pre-scan */
        /* Just ensure we're in the right block */
        break;
    }

    case ORet: {
        /* Return from function */
        int src = op->p1;
        hl_type *ret_type = f->type->fun->ret;
        if (ret_type->kind == HVOID) {
            LLVMBuildRetVoid(ctx->builder);
        } else {
            LLVMValueRef val = llvm_load_vreg(ctx, f, src);
            LLVMBuildRet(ctx->builder, val);
        }
        break;
    }

    case OJAlways: {
        /* Unconditional jump */
        int offset = op->p1;
        LLVMBasicBlockRef target = llvm_get_block_for_offset(ctx, op_idx, offset);
        if (target) {
            LLVMBuildBr(ctx->builder, target);
        }
        break;
    }

    case OJTrue: {
        /* Jump if true (non-zero) */
        int cond_reg = op->p1;
        int offset = op->p2;
        LLVMValueRef cond = llvm_load_vreg(ctx, f, cond_reg);
        LLVMValueRef zero = LLVMConstInt(LLVMTypeOf(cond), 0, false);
        LLVMValueRef cmp = LLVMBuildICmp(ctx->builder, LLVMIntNE, cond, zero, "");
        LLVMBasicBlockRef then_bb = llvm_get_block_for_offset(ctx, op_idx, offset);
        LLVMBasicBlockRef else_bb = llvm_get_block_for_offset(ctx, op_idx, 0); /* fallthrough */
        if (then_bb && else_bb) {
            LLVMBuildCondBr(ctx->builder, cmp, then_bb, else_bb);
        }
        break;
    }

    case OJFalse: {
        /* Jump if false (zero) */
        int cond_reg = op->p1;
        int offset = op->p2;
        LLVMValueRef cond = llvm_load_vreg(ctx, f, cond_reg);
        LLVMValueRef zero = LLVMConstInt(LLVMTypeOf(cond), 0, false);
        LLVMValueRef cmp = LLVMBuildICmp(ctx->builder, LLVMIntEQ, cond, zero, "");
        LLVMBasicBlockRef then_bb = llvm_get_block_for_offset(ctx, op_idx, offset);
        LLVMBasicBlockRef else_bb = llvm_get_block_for_offset(ctx, op_idx, 0); /* fallthrough */
        if (then_bb && else_bb) {
            LLVMBuildCondBr(ctx->builder, cmp, then_bb, else_bb);
        }
        break;
    }

    case OJNull: {
        /* Jump if null */
        int src = op->p1;
        int offset = op->p2;
        LLVMValueRef val = llvm_load_vreg(ctx, f, src);
        LLVMValueRef null_val = LLVMConstNull(ctx->ptr_type);
        LLVMValueRef cmp = LLVMBuildICmp(ctx->builder, LLVMIntEQ, val, null_val, "");
        LLVMBasicBlockRef then_bb = llvm_get_block_for_offset(ctx, op_idx, offset);
        LLVMBasicBlockRef else_bb = llvm_get_block_for_offset(ctx, op_idx, 0); /* fallthrough */
        if (then_bb && else_bb) {
            LLVMBuildCondBr(ctx->builder, cmp, then_bb, else_bb);
        }
        break;
    }

    case OJNotNull: {
        /* Jump if not null */
        int src = op->p1;
        int offset = op->p2;
        LLVMValueRef val = llvm_load_vreg(ctx, f, src);
        LLVMValueRef null_val = LLVMConstNull(ctx->ptr_type);
        LLVMValueRef cmp = LLVMBuildICmp(ctx->builder, LLVMIntNE, val, null_val, "");
        LLVMBasicBlockRef then_bb = llvm_get_block_for_offset(ctx, op_idx, offset);
        LLVMBasicBlockRef else_bb = llvm_get_block_for_offset(ctx, op_idx, 0); /* fallthrough */
        if (then_bb && else_bb) {
            LLVMBuildCondBr(ctx->builder, cmp, then_bb, else_bb);
        }
        break;
    }

    case OJEq:
        emit_cmp_jump(ctx, f, op, op_idx, LLVMIntEQ, LLVMRealOEQ);
        break;

    case OJNotEq:
        emit_cmp_jump(ctx, f, op, op_idx, LLVMIntNE, LLVMRealUNE);
        break;

    case OJSLt:
        emit_cmp_jump(ctx, f, op, op_idx, LLVMIntSLT, LLVMRealOLT);
        break;

    case OJSGte:
        emit_cmp_jump(ctx, f, op, op_idx, LLVMIntSGE, LLVMRealOGE);
        break;

    case OJSGt:
        emit_cmp_jump(ctx, f, op, op_idx, LLVMIntSGT, LLVMRealOGT);
        break;

    case OJSLte:
        emit_cmp_jump(ctx, f, op, op_idx, LLVMIntSLE, LLVMRealOLE);
        break;

    case OJULt:
        emit_cmp_jump(ctx, f, op, op_idx, LLVMIntULT, LLVMRealOLT);
        break;

    case OJUGte:
        emit_cmp_jump(ctx, f, op, op_idx, LLVMIntUGE, LLVMRealOGE);
        break;

    case OJNotLt:
        /* !(a < b) - NaN-aware: use unordered predicates for floats */
        emit_cmp_jump(ctx, f, op, op_idx, LLVMIntSGE, LLVMRealUGE);
        break;

    case OJNotGte:
        /* !(a >= b) - NaN-aware: use unordered predicates for floats */
        emit_cmp_jump(ctx, f, op, op_idx, LLVMIntSLT, LLVMRealULT);
        break;

    case OSwitch: {
        /* Multi-way branch
         * The JIT handles OSwitch by doing linear comparison for each case value (0..ncases-1)
         * and jumping to extra[i] if match. If no match, it FALLS THROUGH to the next instruction.
         * The p3 field (default offset) is NOT used by the JIT - the default is always next instruction.
         * We must match this behavior: default = next instruction (offset 0). */
        int src = op->p1;
        int ncases = op->p2;
        /* int default_offset = op->p3; -- NOT USED, default is next instruction */
        LLVMValueRef val = llvm_load_vreg(ctx, f, src);

        /* Ensure switch value is i32 */
        LLVMTypeRef val_type = LLVMTypeOf(val);
        if (LLVMGetTypeKind(val_type) != LLVMIntegerTypeKind ||
            LLVMGetIntTypeWidth(val_type) != 32) {
            val = LLVMBuildIntCast2(ctx->builder, val, ctx->i32_type, false, "");
        }

        /* Default is NEXT INSTRUCTION (offset 0), matching JIT fall-through behavior */
        LLVMBasicBlockRef default_bb = llvm_get_block_for_offset(ctx, op_idx, 0);
        if (!default_bb) {
            /* Create fallback default block with unreachable */
            default_bb = LLVMAppendBasicBlockInContext(ctx->context,
                ctx->current_function, "switch.default");
            LLVMBasicBlockRef current = LLVMGetInsertBlock(ctx->builder);
            LLVMPositionBuilderAtEnd(ctx->builder, default_bb);
            LLVMBuildUnreachable(ctx->builder);
            LLVMPositionBuilderAtEnd(ctx->builder, current);
        }

        LLVMValueRef switch_inst = LLVMBuildSwitch(ctx->builder, val, default_bb, ncases);
        for (int i = 0; i < ncases; i++) {
            int case_offset = op->extra[i];
            LLVMBasicBlockRef case_bb = llvm_get_block_for_offset(ctx, op_idx, case_offset);
            if (case_bb) {
                LLVMValueRef case_val = LLVMConstInt(ctx->i32_type, i, false);
                LLVMAddCase(switch_inst, case_val, case_bb);
            }
        }
        break;
    }

    case ONullCheck: {
        /* Null pointer check - call hl_null_access if null */
        int src = op->p1;
        LLVMValueRef val = llvm_load_vreg(ctx, f, src);
        LLVMValueRef null_val = LLVMConstNull(ctx->ptr_type);
        LLVMValueRef is_null = LLVMBuildICmp(ctx->builder, LLVMIntEQ, val, null_val, "");

        LLVMBasicBlockRef null_bb = LLVMAppendBasicBlockInContext(ctx->context,
            ctx->current_function, "nullcheck.fail");
        LLVMBasicBlockRef ok_bb = LLVMAppendBasicBlockInContext(ctx->context,
            ctx->current_function, "nullcheck.ok");

        LLVMBuildCondBr(ctx->builder, is_null, null_bb, ok_bb);

        LLVMPositionBuilderAtEnd(ctx->builder, null_bb);
        LLVMBuildCall2(ctx->builder,
            LLVMGlobalGetValueType(ctx->rt_null_access),
            ctx->rt_null_access, NULL, 0, "");
        LLVMBuildUnreachable(ctx->builder);

        LLVMPositionBuilderAtEnd(ctx->builder, ok_bb);
        break;
    }

    default:
        break;
    }
}
