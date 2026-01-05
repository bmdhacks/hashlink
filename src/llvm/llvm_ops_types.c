/*
 * Copyright (C)2005-2016 Haxe Foundation
 * LLVM Backend - Type Conversion and Casting Opcodes
 */
#include "llvm_codegen.h"

/*
 * Unbox a Null<T> wrapper to get the inner value.
 * Generates: if (wrapper == null) return default_value; else return wrapper->v;
 *
 * This matches the JIT's inline handling in op_safe_cast (jit_aarch64.c:3127-3184).
 * The Null wrapper layout is: { hl_type *t; T v; } where v is at offset 8.
 *
 * Supports primitive types: HUI8, HUI16, HI32, HBOOL, HI64, HGUID, HF32, HF64
 * Returns NULL for unsupported types (caller should fall back to runtime).
 */
LLVMValueRef llvm_unbox_null(llvm_ctx *ctx, LLVMValueRef wrapper_ptr, hl_type *inner_type) {
    /* Only handle primitive types inline */
    switch (inner_type->kind) {
    case HUI8:
    case HUI16:
    case HI32:
    case HBOOL:
    case HI64:
    case HGUID:
    case HF32:
    case HF64:
        break;
    default:
        return NULL;  /* Caller should use runtime */
    }

    LLVMTypeRef val_type = llvm_get_type(ctx, inner_type);

    /* Check if null */
    LLVMValueRef is_null = LLVMBuildICmp(ctx->builder, LLVMIntEQ, wrapper_ptr,
        LLVMConstNull(ctx->ptr_type), "is_null");

    /* Create basic blocks */
    LLVMBasicBlockRef not_null_bb = LLVMAppendBasicBlock(ctx->current_function, "unbox_not_null");
    LLVMBasicBlockRef null_bb = LLVMAppendBasicBlock(ctx->current_function, "unbox_null");
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlock(ctx->current_function, "unbox_merge");

    LLVMBuildCondBr(ctx->builder, is_null, null_bb, not_null_bb);

    /* Not null path: load value from offset 8 (the 'v' field after type pointer) */
    LLVMPositionBuilderAtEnd(ctx->builder, not_null_bb);
    LLVMValueRef offset_8 = LLVMConstInt(ctx->i64_type, 8, false);
    LLVMValueRef val_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
        wrapper_ptr, &offset_8, 1, "val_ptr");
    LLVMValueRef loaded_val = LLVMBuildLoad2(ctx->builder, val_type, val_ptr, "loaded_val");
    LLVMBuildBr(ctx->builder, merge_bb);
    LLVMBasicBlockRef not_null_end = LLVMGetInsertBlock(ctx->builder);

    /* Null path: use default value (0 or null pointer) */
    LLVMPositionBuilderAtEnd(ctx->builder, null_bb);
    LLVMValueRef default_val;
    if (inner_type->kind == HF32) {
        default_val = LLVMConstReal(ctx->f32_type, 0.0);
    } else if (inner_type->kind == HF64) {
        default_val = LLVMConstReal(ctx->f64_type, 0.0);
    } else if (LLVMGetTypeKind(val_type) == LLVMPointerTypeKind) {
        default_val = LLVMConstNull(val_type);
    } else {
        default_val = LLVMConstInt(val_type, 0, false);
    }
    LLVMBuildBr(ctx->builder, merge_bb);
    LLVMBasicBlockRef null_end = LLVMGetInsertBlock(ctx->builder);

    /* Merge with phi */
    LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);
    LLVMValueRef phi = LLVMBuildPhi(ctx->builder, val_type, "unboxed");
    LLVMValueRef incoming_vals[] = { loaded_val, default_val };
    LLVMBasicBlockRef incoming_bbs[] = { not_null_end, null_end };
    LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);

    return phi;
}

void llvm_emit_types(llvm_ctx *ctx, hl_function *f, hl_opcode *op, int op_idx) {
    switch (op->op) {
    case OType: {
        /* dst = type pointer for type index p2 */
        int dst = op->p1;
        int type_idx = op->p2;
        LLVMValueRef type_ptr = llvm_get_type_ptr(ctx, type_idx);
        llvm_store_vreg(ctx, f, dst, type_ptr);
        break;
    }

    case OGetType: {
        /*
         * dst = obj->t (type of object)
         * If obj is NULL, return &hlt_void (matches JIT behavior)
         */
        int dst = op->p1;
        int src = op->p2;
        LLVMValueRef obj = llvm_load_vreg(ctx, f, src);

        /* Check if object is null */
        LLVMValueRef is_null = LLVMBuildICmp(ctx->builder, LLVMIntEQ, obj,
            LLVMConstNull(ctx->ptr_type), "is_null");

        /* Create basic blocks */
        LLVMBasicBlockRef not_null_bb = LLVMAppendBasicBlock(ctx->current_function, "gettype_not_null");
        LLVMBasicBlockRef null_bb = LLVMAppendBasicBlock(ctx->current_function, "gettype_null");
        LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlock(ctx->current_function, "gettype_merge");

        LLVMBuildCondBr(ctx->builder, is_null, null_bb, not_null_bb);

        /* Not null path: load type from object (first field at offset 0) */
        LLVMPositionBuilderAtEnd(ctx->builder, not_null_bb);
        LLVMValueRef type_from_obj = LLVMBuildLoad2(ctx->builder, ctx->ptr_type, obj, "obj_type");
        LLVMBuildBr(ctx->builder, merge_bb);
        LLVMBasicBlockRef not_null_end = LLVMGetInsertBlock(ctx->builder);

        /* Null path: return &hlt_void */
        LLVMPositionBuilderAtEnd(ctx->builder, null_bb);
        LLVMValueRef void_type = ctx->rt_hlt_void;
        LLVMBuildBr(ctx->builder, merge_bb);
        LLVMBasicBlockRef null_end = LLVMGetInsertBlock(ctx->builder);

        /* Merge with phi */
        LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);
        LLVMValueRef phi = LLVMBuildPhi(ctx->builder, ctx->ptr_type, "type_ptr");
        LLVMValueRef incoming_vals[] = { type_from_obj, void_type };
        LLVMBasicBlockRef incoming_bbs[] = { not_null_end, null_end };
        LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);

        llvm_store_vreg(ctx, f, dst, phi);
        break;
    }

    case OGetTID: {
        /* dst = type->kind */
        int dst = op->p1;
        int src = op->p2;
        LLVMValueRef type_ptr = llvm_load_vreg(ctx, f, src);
        /* hl_type has kind as first field (int) */
        LLVMValueRef kind = LLVMBuildLoad2(ctx->builder, ctx->i32_type, type_ptr, "");
        llvm_store_vreg(ctx, f, dst, kind);
        break;
    }

    case OToDyn: {
        /* dst = wrap value as dynamic */
        int dst = op->p1;
        int src = op->p2;
        hl_type *src_type = f->regs[src];

        LLVMValueRef val = llvm_load_vreg(ctx, f, src);

        /* For pointer types, just use the value directly */
        if (llvm_is_ptr_type(src_type)) {
            llvm_store_vreg(ctx, f, dst, val);
        } else {
            /* For value types, need to call hl_make_dyn */
            /* First store value to memory, then pass pointer */
            /* Use entry block alloca to avoid stack growth in loops */
            LLVMValueRef val_alloca = llvm_create_entry_alloca(ctx,
                llvm_get_type(ctx, src_type), "todyn_tmp");
            LLVMBuildStore(ctx->builder, val, val_alloca);

            /* Get type pointer for the source type.
             * Search for a type in the types array with matching kind.
             * We need to find a type whose kind matches src_type->kind, not just
             * any type that src_type happens to point to, because the register
             * type might be a global singleton that's not in the types array,
             * or might be aliased incorrectly.
             *
             * Basic types (HUI8, HUI16, HI32, HI64, HF32, HF64, HBOOL, HGUID) may be
             * global singletons rather than entries in the types array. */
            int type_idx = -1;
            hl_type_kind k = src_type->kind;
            for (int i = 0; i < ctx->code->ntypes; i++) {
                if (ctx->code->types[i].kind == k) {
                    type_idx = i;
                    break;
                }
            }
            LLVMValueRef type_ptr = type_idx >= 0 ? llvm_get_type_ptr(ctx, type_idx)
                : LLVMConstNull(ctx->ptr_type);

            LLVMValueRef args[] = { val_alloca, type_ptr };
            LLVMValueRef result = LLVMBuildCall2(ctx->builder,
                LLVMGlobalGetValueType(ctx->rt_make_dyn),
                ctx->rt_make_dyn, args, 2, "");
            llvm_store_vreg(ctx, f, dst, result);
        }
        break;
    }

    case OToSFloat: {
        /* dst = (float)src - convert to float type */
        int dst = op->p1;
        int src = op->p2;
        hl_type *src_type = f->regs[src];
        hl_type *dst_type = f->regs[dst];
        LLVMTypeRef target_type = llvm_get_type(ctx, dst_type);
        LLVMValueRef val = llvm_load_vreg(ctx, f, src);

        LLVMValueRef result;
        if (llvm_is_float_type(src_type)) {
            /* Float to float - use fptrunc or fpext */
            if (src_type->kind == HF64 && dst_type->kind == HF32) {
                result = LLVMBuildFPTrunc(ctx->builder, val, target_type, "");
            } else if (src_type->kind == HF32 && dst_type->kind == HF64) {
                result = LLVMBuildFPExt(ctx->builder, val, target_type, "");
            } else {
                result = val; /* Same type */
            }
        } else if (src_type->kind == HUI8 || src_type->kind == HUI16) {
            /* Unsigned integer to float - use uitofp */
            result = LLVMBuildUIToFP(ctx->builder, val, target_type, "");
        } else {
            /* Signed integer to float - use sitofp */
            result = LLVMBuildSIToFP(ctx->builder, val, target_type, "");
        }
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case OToUFloat: {
        /* dst = (float)src - convert to float type (unsigned source) */
        int dst = op->p1;
        int src = op->p2;
        hl_type *src_type = f->regs[src];
        hl_type *dst_type = f->regs[dst];
        LLVMTypeRef target_type = llvm_get_type(ctx, dst_type);
        LLVMValueRef val = llvm_load_vreg(ctx, f, src);

        LLVMValueRef result;
        if (llvm_is_float_type(src_type)) {
            /* Float to float - use fptrunc or fpext */
            if (src_type->kind == HF64 && dst_type->kind == HF32) {
                result = LLVMBuildFPTrunc(ctx->builder, val, target_type, "");
            } else if (src_type->kind == HF32 && dst_type->kind == HF64) {
                result = LLVMBuildFPExt(ctx->builder, val, target_type, "");
            } else {
                result = val; /* Same type */
            }
        } else {
            /* Unsigned integer to float - use uitofp */
            result = LLVMBuildUIToFP(ctx->builder, val, target_type, "");
        }
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case OToInt: {
        /* dst = (int)src - convert to integer type */
        int dst = op->p1;
        int src = op->p2;
        hl_type *src_type = f->regs[src];
        hl_type *dst_type = f->regs[dst];
        LLVMValueRef val = llvm_load_vreg(ctx, f, src);

        /* Determine target integer type based on destination kind */
        LLVMTypeRef target_type;
        switch (dst_type->kind) {
        case HI64:
        case HGUID:  /* HGUID is also 64-bit */
            target_type = ctx->i64_type; break;
        case HUI8: case HBOOL: target_type = ctx->i8_type; break;
        case HUI16: target_type = ctx->i16_type; break;
        default: target_type = ctx->i32_type; break;  /* HI32 and others */
        }

        LLVMValueRef result;
        LLVMTypeRef val_type = LLVMTypeOf(val);
        if (llvm_is_float_type(src_type)) {
            result = LLVMBuildFPToSI(ctx->builder, val, target_type, "");
        } else if (LLVMGetTypeKind(val_type) == LLVMPointerTypeKind) {
            /* Pointer to integer - use ptrtoint */
            result = LLVMBuildPtrToInt(ctx->builder, val, target_type, "");
        } else {
            /* Integer to integer - handle width differences */
            unsigned src_bits = LLVMGetIntTypeWidth(val_type);
            unsigned dst_bits = LLVMGetIntTypeWidth(target_type);
            if (src_bits > dst_bits) {
                result = LLVMBuildTrunc(ctx->builder, val, target_type, "");
            } else if (src_bits < dst_bits) {
                /* Use zero-extension for unsigned types, sign-extension for signed */
                if (src_type->kind == HUI8 || src_type->kind == HUI16) {
                    result = LLVMBuildZExt(ctx->builder, val, target_type, "");
                } else {
                    result = LLVMBuildSExt(ctx->builder, val, target_type, "");
                }
            } else {
                result = val;
            }
        }
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case OSafeCast: {
        /*
         * dst = safe_cast(src, target_type)
         *
         * Different cast functions are used based on destination type:
         * - hl_dyn_casti(ptr, src_type, dst_type) -> i32 for HBOOL, HI32, HUI8, HUI16
         * - hl_dyn_casti64(ptr, src_type) -> i64 for HI64
         * - hl_dyn_castf(ptr, src_type) -> f32 for HF32
         * - hl_dyn_castd(ptr, src_type) -> f64 for HF64
         * - hl_dyn_castp(ptr, src_type, dst_type) -> ptr for pointer types
         */
        int dst = op->p1;
        int src = op->p2;
        hl_type *src_type = f->regs[src];
        hl_type *dst_type = f->regs[dst];

        /* Get source type pointer (static compile-time type) */
        int src_type_idx = -1;
        for (int i = 0; i < ctx->code->ntypes; i++) {
            if (ctx->code->types + i == src_type) {
                src_type_idx = i;
                break;
            }
        }
        LLVMValueRef src_type_ptr = src_type_idx >= 0 ? llvm_get_type_ptr(ctx, src_type_idx)
            : LLVMConstNull(ctx->ptr_type);

        /* Get destination type pointer (needed for some cast functions) */
        int dst_type_idx = -1;
        for (int i = 0; i < ctx->code->ntypes; i++) {
            if (ctx->code->types + i == dst_type) {
                dst_type_idx = i;
                break;
            }
        }
        LLVMValueRef dst_type_ptr = dst_type_idx >= 0 ? llvm_get_type_ptr(ctx, dst_type_idx)
            : LLVMConstNull(ctx->ptr_type);

        /* Get address of source vreg (pointer to stack slot containing the value) */
        LLVMValueRef src_addr = ctx->vreg_allocs[src];

        LLVMValueRef result;

        /*
         * Compile-time optimization: same type, no cast needed
         */
        if (src_type == dst_type) {
            result = llvm_load_vreg(ctx, f, src);
            llvm_store_vreg(ctx, f, dst, result);
            break;
        }

        /*
         * Compile-time optimization: casting to HDYN from a dynamic type is identity
         */
        if (dst_type->kind == HDYN && hl_is_dynamic(src_type)) {
            result = llvm_load_vreg(ctx, f, src);
            llvm_store_vreg(ctx, f, dst, result);
            break;
        }

        /*
         * Compile-time optimization: use hl_safe_cast at compile time
         * This handles compatible object hierarchies, virtual types, etc.
         */
        if (hl_safe_cast(src_type, dst_type)) {
            result = llvm_load_vreg(ctx, f, src);
            llvm_store_vreg(ctx, f, dst, result);
            break;
        }

        /*
         * Special case: Null<T> -> T unboxing
         * The JIT handles this inline instead of calling runtime functions.
         * Use the llvm_unbox_null helper which handles primitive types.
         */
        if (src_type->kind == HNULL && src_type->tparam &&
            src_type->tparam->kind == dst_type->kind) {
            LLVMValueRef wrapper_ptr = llvm_load_vreg(ctx, f, src);
            LLVMValueRef unboxed = llvm_unbox_null(ctx, wrapper_ptr, dst_type);
            if (unboxed) {
                llvm_store_vreg(ctx, f, dst, unboxed);
                break;
            }
            /* Fall through to runtime for unsupported types */
        }

        /*
         * Inline fast path for HDYN -> pointer type casts.
         * The common case is when runtime type exactly matches destination type.
         * We inline: null check, type equality check, then fall back to runtime.
         *
         * This avoids the function call overhead for the most common case.
         */
        if (src_type->kind == HDYN && llvm_is_ptr_type(dst_type)) {
            LLVMValueRef val = llvm_load_vreg(ctx, f, src);

            /* Create basic blocks */
            LLVMBasicBlockRef null_bb = LLVMAppendBasicBlock(ctx->current_function, "dyn_null");
            LLVMBasicBlockRef check_type_bb = LLVMAppendBasicBlock(ctx->current_function, "dyn_check_type");
            LLVMBasicBlockRef fast_bb = LLVMAppendBasicBlock(ctx->current_function, "dyn_fast");
            LLVMBasicBlockRef slow_bb = LLVMAppendBasicBlock(ctx->current_function, "dyn_slow");
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlock(ctx->current_function, "dyn_merge");

            /* Null check: if val == null, go to null path */
            LLVMValueRef is_null = LLVMBuildICmp(ctx->builder, LLVMIntEQ, val,
                LLVMConstNull(ctx->ptr_type), "is_null");
            LLVMBuildCondBr(ctx->builder, is_null, null_bb, check_type_bb);

            /* Null path: return null */
            LLVMPositionBuilderAtEnd(ctx->builder, null_bb);
            LLVMBuildBr(ctx->builder, merge_bb);

            /* Check type: load v->t (at offset 0) and compare with dst_type */
            LLVMPositionBuilderAtEnd(ctx->builder, check_type_bb);
            LLVMValueRef runtime_type = LLVMBuildLoad2(ctx->builder, ctx->ptr_type, val, "runtime_type");
            LLVMValueRef types_match = LLVMBuildICmp(ctx->builder, LLVMIntEQ,
                runtime_type, dst_type_ptr, "types_match");
            LLVMBuildCondBr(ctx->builder, types_match, fast_bb, slow_bb);

            /* Fast path: types match exactly, return the value */
            LLVMPositionBuilderAtEnd(ctx->builder, fast_bb);
            LLVMBuildBr(ctx->builder, merge_bb);

            /* Slow path: call hl_dyn_castp for hierarchy checks, etc */
            LLVMPositionBuilderAtEnd(ctx->builder, slow_bb);
            LLVMValueRef slow_result = LLVMBuildCall2(ctx->builder,
                LLVMGlobalGetValueType(ctx->rt_dyn_castp),
                ctx->rt_dyn_castp, (LLVMValueRef[]){ src_addr, src_type_ptr, dst_type_ptr }, 3, "");
            LLVMBuildBr(ctx->builder, merge_bb);

            /* Merge with phi */
            LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);
            result = LLVMBuildPhi(ctx->builder, ctx->ptr_type, "cast_result");
            LLVMAddIncoming(result,
                (LLVMValueRef[]){ LLVMConstNull(ctx->ptr_type), val, slow_result },
                (LLVMBasicBlockRef[]){ null_bb, fast_bb, slow_bb }, 3);

            llvm_store_vreg(ctx, f, dst, result);
            break;
        }

        /* Runtime cast path - use appropriate hl_dyn_cast* function */
        switch (dst_type->kind) {
        case HF32: {
            /* hl_dyn_castf(ptr, src_type) -> float */
            LLVMValueRef args[] = { src_addr, src_type_ptr };
            result = LLVMBuildCall2(ctx->builder,
                LLVMGlobalGetValueType(ctx->rt_dyn_castf),
                ctx->rt_dyn_castf, args, 2, "");
            break;
        }
        case HF64: {
            /* hl_dyn_castd(ptr, src_type) -> double */
            LLVMValueRef args[] = { src_addr, src_type_ptr };
            result = LLVMBuildCall2(ctx->builder,
                LLVMGlobalGetValueType(ctx->rt_dyn_castd),
                ctx->rt_dyn_castd, args, 2, "");
            break;
        }
        case HI64:
        case HGUID: {
            /* hl_dyn_casti64(ptr, src_type) -> i64 */
            /* HGUID is 64-bit like HI64, uses same cast function */
            LLVMValueRef args[] = { src_addr, src_type_ptr };
            result = LLVMBuildCall2(ctx->builder,
                LLVMGlobalGetValueType(ctx->rt_dyn_casti64),
                ctx->rt_dyn_casti64, args, 2, "");
            break;
        }
        case HI32:
        case HUI16:
        case HUI8:
        case HBOOL: {
            /* hl_dyn_casti(ptr, src_type, dst_type) -> i32 */
            LLVMValueRef args[] = { src_addr, src_type_ptr, dst_type_ptr };
            LLVMValueRef i32_result = LLVMBuildCall2(ctx->builder,
                LLVMGlobalGetValueType(ctx->rt_dyn_casti),
                ctx->rt_dyn_casti, args, 3, "");
            /* Truncate to actual destination size */
            LLVMTypeRef target_type = llvm_get_type(ctx, dst_type);
            if (dst_type->kind == HBOOL || dst_type->kind == HUI8) {
                result = LLVMBuildTrunc(ctx->builder, i32_result, target_type, "");
            } else if (dst_type->kind == HUI16) {
                result = LLVMBuildTrunc(ctx->builder, i32_result, target_type, "");
            } else {
                result = i32_result;
            }
            break;
        }
        default: {
            /* hl_dyn_castp(ptr, src_type, dst_type) -> ptr */
            LLVMValueRef args[] = { src_addr, src_type_ptr, dst_type_ptr };
            result = LLVMBuildCall2(ctx->builder,
                LLVMGlobalGetValueType(ctx->rt_dyn_castp),
                ctx->rt_dyn_castp, args, 3, "");
            break;
        }
        }

        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case OUnsafeCast: {
        /* dst = (dst_type)src - no runtime check */
        int dst = op->p1;
        int src = op->p2;
        LLVMValueRef val = llvm_load_vreg(ctx, f, src);
        /* Just pass through for pointer types */
        llvm_store_vreg(ctx, f, dst, val);
        break;
    }

    case OToVirtual: {
        /* dst = to_virtual(src, virtual_type) */
        int dst = op->p1;
        int src = op->p2;
        hl_type *dst_type = f->regs[dst];

        LLVMValueRef val = llvm_load_vreg(ctx, f, src);

        /* Get destination type pointer */
        int type_idx = -1;
        for (int i = 0; i < ctx->code->ntypes; i++) {
            if (ctx->code->types + i == dst_type) {
                type_idx = i;
                break;
            }
        }
        LLVMValueRef type_ptr = type_idx >= 0 ? llvm_get_type_ptr(ctx, type_idx)
            : LLVMConstNull(ctx->ptr_type);

        /* Call hl_to_virtual */
        LLVMValueRef args[] = { type_ptr, val };
        LLVMValueRef result = LLVMBuildCall2(ctx->builder,
            LLVMGlobalGetValueType(ctx->rt_to_virtual),
            ctx->rt_to_virtual, args, 2, "");
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    default:
        break;
    }
}
