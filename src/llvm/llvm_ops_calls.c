/*
 * Copyright (C)2005-2016 Haxe Foundation
 * LLVM Backend - Function Call Opcodes
 */
#include "llvm_codegen.h"

/* Helper to coerce call result to destination register type.
 * This handles type mismatches where the function's return type differs
 * from the destination register type (e.g., function returns i32 but
 * register expects dyn/ptr due to type covariance). */
static LLVMValueRef coerce_call_result(llvm_ctx *ctx, hl_function *f, int dst,
                                        LLVMValueRef result, LLVMTypeRef result_type) {
    LLVMTypeRef dst_type = llvm_get_type(ctx, f->regs[dst]);
    LLVMTypeKind result_kind = LLVMGetTypeKind(result_type);
    LLVMTypeKind dst_kind = LLVMGetTypeKind(dst_type);

    if (dst_kind == LLVMPointerTypeKind && result_kind == LLVMIntegerTypeKind) {
        /* Integer to pointer: use inttoptr */
        return LLVMBuildIntToPtr(ctx->builder, result, dst_type, "");
    } else if (dst_kind == LLVMIntegerTypeKind && result_kind == LLVMPointerTypeKind) {
        /* Pointer to integer: use ptrtoint */
        return LLVMBuildPtrToInt(ctx->builder, result, dst_type, "");
    } else if (dst_kind == LLVMIntegerTypeKind && result_kind == LLVMIntegerTypeKind) {
        /* Integer to integer: may need extend or truncate */
        unsigned result_bits = LLVMGetIntTypeWidth(result_type);
        unsigned dst_bits = LLVMGetIntTypeWidth(dst_type);
        if (dst_bits > result_bits) {
            return LLVMBuildZExt(ctx->builder, result, dst_type, "");
        } else if (dst_bits < result_bits) {
            return LLVMBuildTrunc(ctx->builder, result, dst_type, "");
        }
    }
    return result;
}

/*
 * Extract a non-pointer result from a vdynamic return buffer.
 *
 * The runtime stores values in vdynamic using specific storage types:
 *   - HBOOL, HUI8, HUI16, HI32 -> stored as i32 in v.i
 *   - HI64 -> stored as i64 in v.i64
 *   - HF32 -> stored as f32 in v.f
 *   - HF64 -> stored as f64 in v.d
 *
 * We load using the storage type, then convert to the destination LLVM type.
 */
static LLVMValueRef extract_vdyn_result(llvm_ctx *ctx, hl_type *dst_hl_type,
                                         LLVMValueRef ret_ptr, LLVMTypeRef dst_llvm_type) {
    /* Load from ret_buf + 8 (HDYN_VALUE offset) */
    LLVMValueRef offset = LLVMConstInt(ctx->i64_type, 8, false);
    LLVMValueRef val_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type, ret_ptr, &offset, 1, "");

    /* Determine runtime storage type */
    LLVMTypeRef storage_type;
    switch (dst_hl_type->kind) {
    case HBOOL:
    case HUI8:
    case HUI16:
    case HI32:
        storage_type = ctx->i32_type;
        break;
    case HI64:
        storage_type = ctx->i64_type;
        break;
    case HF32:
        storage_type = ctx->f32_type;
        break;
    case HF64:
        storage_type = ctx->f64_type;
        break;
    default:
        storage_type = dst_llvm_type;
        break;
    }

    LLVMValueRef loaded = LLVMBuildLoad2(ctx->builder, storage_type, val_ptr, "dyn_raw");

    /* Convert to destination type if needed */
    if (storage_type == dst_llvm_type) {
        return loaded;
    } else if (dst_hl_type->kind == HBOOL) {
        /* i32 -> i8 (HBOOL is stored as i8): compare != 0, then zext to i8 */
        LLVMValueRef cmp = LLVMBuildICmp(ctx->builder, LLVMIntNE, loaded,
            LLVMConstInt(ctx->i32_type, 0, false), "to_bool");
        return LLVMBuildZExt(ctx->builder, cmp, dst_llvm_type, "bool_i8");
    } else if (dst_hl_type->kind == HUI8 || dst_hl_type->kind == HUI16) {
        /* i32 -> i8/i16: truncate */
        return LLVMBuildTrunc(ctx->builder, loaded, dst_llvm_type, "trunc");
    }
    return loaded;
}

void llvm_emit_calls(llvm_ctx *ctx, hl_function *f, hl_opcode *op, int op_idx) {
    switch (op->op) {
    case OCall0: {
        /* dst = func() */
        int dst = op->p1;
        int findex = op->p2;
        LLVMValueRef func = llvm_get_function_ptr(ctx, findex);
        LLVMTypeRef fn_type = ctx->function_types[findex];
        LLVMTypeRef ret_type = LLVMGetReturnType(fn_type);
        bool returns_void = (LLVMGetTypeKind(ret_type) == LLVMVoidTypeKind);
        LLVMValueRef result = LLVMBuildCall2(ctx->builder, fn_type, func, NULL, 0, "");
        if (f->regs[dst]->kind != HVOID && !returns_void) {
            result = coerce_call_result(ctx, f, dst, result, ret_type);
            llvm_store_vreg(ctx, f, dst, result);
        }
        break;
    }

    case OCall1: {
        /* dst = func(arg0) */
        int dst = op->p1;
        int findex = op->p2;
        int arg0 = op->p3;
        LLVMValueRef func = llvm_get_function_ptr(ctx, findex);
        LLVMTypeRef fn_type = ctx->function_types[findex];
        LLVMTypeRef ret_type = LLVMGetReturnType(fn_type);
        bool returns_void = (LLVMGetTypeKind(ret_type) == LLVMVoidTypeKind);
        LLVMValueRef args[] = { llvm_load_vreg(ctx, f, arg0) };
        LLVMValueRef result = LLVMBuildCall2(ctx->builder, fn_type, func, args, 1, "");
        if (f->regs[dst]->kind != HVOID && !returns_void) {
            result = coerce_call_result(ctx, f, dst, result, ret_type);
            llvm_store_vreg(ctx, f, dst, result);
        }
        break;
    }

    case OCall2: {
        /* dst = func(arg0, arg1) */
        int dst = op->p1;
        int findex = op->p2;
        int arg0 = op->p3;
        int arg1 = (int)(int_val)op->extra; /* extra is direct int, not array */
        LLVMValueRef func = llvm_get_function_ptr(ctx, findex);
        LLVMTypeRef fn_type = ctx->function_types[findex];
        LLVMTypeRef ret_type = LLVMGetReturnType(fn_type);
        bool returns_void = (LLVMGetTypeKind(ret_type) == LLVMVoidTypeKind);
        LLVMValueRef args[] = {
            llvm_load_vreg(ctx, f, arg0),
            llvm_load_vreg(ctx, f, arg1)
        };
        LLVMValueRef result = LLVMBuildCall2(ctx->builder, fn_type, func, args, 2, "");
        if (f->regs[dst]->kind != HVOID && !returns_void) {
            result = coerce_call_result(ctx, f, dst, result, ret_type);
            llvm_store_vreg(ctx, f, dst, result);
        }
        break;
    }

    case OCall3: {
        /* dst = func(arg0, arg1, arg2) */
        int dst = op->p1;
        int findex = op->p2;
        int arg0 = op->p3;
        int arg1 = op->extra[0];
        int arg2 = op->extra[1];
        LLVMValueRef func = llvm_get_function_ptr(ctx, findex);
        LLVMTypeRef fn_type = ctx->function_types[findex];
        LLVMTypeRef ret_type = LLVMGetReturnType(fn_type);
        bool returns_void = (LLVMGetTypeKind(ret_type) == LLVMVoidTypeKind);
        LLVMValueRef args[] = {
            llvm_load_vreg(ctx, f, arg0),
            llvm_load_vreg(ctx, f, arg1),
            llvm_load_vreg(ctx, f, arg2)
        };
        LLVMValueRef result = LLVMBuildCall2(ctx->builder, fn_type, func, args, 3, "");
        if (f->regs[dst]->kind != HVOID && !returns_void) {
            result = coerce_call_result(ctx, f, dst, result, ret_type);
            llvm_store_vreg(ctx, f, dst, result);
        }
        break;
    }

    case OCall4: {
        /* dst = func(arg0, arg1, arg2, arg3) */
        int dst = op->p1;
        int findex = op->p2;
        int arg0 = op->p3;
        int arg1 = op->extra[0];
        int arg2 = op->extra[1];
        int arg3 = op->extra[2];
        LLVMValueRef func = llvm_get_function_ptr(ctx, findex);
        LLVMTypeRef fn_type = ctx->function_types[findex];
        LLVMTypeRef ret_type = LLVMGetReturnType(fn_type);
        bool returns_void = (LLVMGetTypeKind(ret_type) == LLVMVoidTypeKind);
        LLVMValueRef args[] = {
            llvm_load_vreg(ctx, f, arg0),
            llvm_load_vreg(ctx, f, arg1),
            llvm_load_vreg(ctx, f, arg2),
            llvm_load_vreg(ctx, f, arg3)
        };
        LLVMValueRef result = LLVMBuildCall2(ctx->builder, fn_type, func, args, 4, "");
        if (f->regs[dst]->kind != HVOID && !returns_void) {
            result = coerce_call_result(ctx, f, dst, result, ret_type);
            llvm_store_vreg(ctx, f, dst, result);
        }
        break;
    }

    case OCallN: {
        /* dst = func(args...) */
        int dst = op->p1;
        int findex = op->p2;
        int nargs = op->p3;
        LLVMValueRef func = llvm_get_function_ptr(ctx, findex);
        LLVMTypeRef fn_type = ctx->function_types[findex];
        LLVMTypeRef ret_type = LLVMGetReturnType(fn_type);
        bool returns_void = (LLVMGetTypeKind(ret_type) == LLVMVoidTypeKind);

        LLVMValueRef *args = NULL;
        if (nargs > 0) {
            args = (LLVMValueRef *)malloc(sizeof(LLVMValueRef) * nargs);
            for (int i = 0; i < nargs; i++) {
                args[i] = llvm_load_vreg(ctx, f, op->extra[i]);
            }
        }

        LLVMValueRef result = LLVMBuildCall2(ctx->builder, fn_type, func, args, nargs, "");
        if (f->regs[dst]->kind != HVOID && !returns_void) {
            result = coerce_call_result(ctx, f, dst, result, ret_type);
            llvm_store_vreg(ctx, f, dst, result);
        }

        if (args) free(args);
        break;
    }

    case OCallMethod: {
        /* dst = obj.method(args...) via vtable */
        int dst = op->p1;
        int method_idx = op->p2;
        int nargs = op->p3;
        int obj_reg = op->extra[0];

        hl_type *obj_type = f->regs[obj_reg];
        LLVMValueRef obj = llvm_load_vreg(ctx, f, obj_reg);

        /* Get method return type if possible */
        hl_type *method_type = NULL;
        if (obj_type->kind == HOBJ && obj_type->obj) {
            if (method_idx < obj_type->obj->nproto) {
                int findex = obj_type->obj->proto[method_idx].findex;
                if (findex >= 0 && findex < ctx->code->nfunctions) {
                    method_type = ctx->code->functions[findex].type;
                }
            }
        } else if (obj_type->kind == HVIRTUAL && obj_type->virt) {
            if (method_idx < obj_type->virt->nfields) {
                method_type = obj_type->virt->fields[method_idx].t;
            }
        }

        /* Build function type matching actual call arguments */
        LLVMTypeRef ret_llvm_type = ctx->ptr_type; /* Default return type */
        if (method_type && (method_type->kind == HFUN || method_type->kind == HMETHOD) && method_type->fun) {
            ret_llvm_type = llvm_get_type(ctx, method_type->fun->ret);
        }

        /* Determine if destination needs a result - use this instead of method's
         * declared return type, since methods may be declared void but bytecode
         * expects a result (covariant returns) */
        bool dst_needs_result = (f->regs[dst]->kind != HVOID);

        if (obj_type->kind == HVIRTUAL) {
            /*
             * HVIRTUAL method call:
             * vvirtual layout: hl_type* t (0), vdynamic* value (8), vvirtual* next (16), vfields[...] (24+)
             * vfield[method_idx] is at offset 24 + method_idx * 8
             * If vfield is not NULL, call it with obj->value as first arg
             * If vfield is NULL, call hl_dyn_call_obj for dynamic dispatch
             */
            int vfield_offset = 24 + method_idx * 8;
            LLVMValueRef vfield_off_val = LLVMConstInt(ctx->i64_type, vfield_offset, false);
            LLVMValueRef vfield_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
                obj, &vfield_off_val, 1, "");
            LLVMValueRef vfield = LLVMBuildLoad2(ctx->builder, ctx->ptr_type, vfield_ptr, "vfield");

            /* Load obj->value (at offset 8) - this is the actual object to pass */
            LLVMValueRef value_off = LLVMConstInt(ctx->i64_type, 8, false);
            LLVMValueRef value_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
                obj, &value_off, 1, "");
            LLVMValueRef obj_value = LLVMBuildLoad2(ctx->builder, ctx->ptr_type, value_ptr, "obj_value");

            /* Check if vfield is NULL */
            LLVMValueRef is_null = LLVMBuildICmp(ctx->builder, LLVMIntEQ, vfield,
                LLVMConstNull(ctx->ptr_type), "vfield_is_null");

            /* Create basic blocks for branching */
            LLVMBasicBlockRef direct_bb = LLVMAppendBasicBlock(ctx->current_function, "vmethod_direct");
            LLVMBasicBlockRef fallback_bb = LLVMAppendBasicBlock(ctx->current_function, "vmethod_fallback");
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlock(ctx->current_function, "vmethod_merge");

            /* Branch on NULL check */
            LLVMBuildCondBr(ctx->builder, is_null, fallback_bb, direct_bb);

            /* ---- Direct path (vfield NOT NULL) ---- */
            LLVMPositionBuilderAtEnd(ctx->builder, direct_bb);

            /* Build args array with obj->value as first arg */
            LLVMValueRef *args = (LLVMValueRef *)malloc(sizeof(LLVMValueRef) * nargs);
            args[0] = obj_value;
            for (int i = 1; i < nargs; i++) {
                args[i] = llvm_load_vreg(ctx, f, op->extra[i]);
            }

            /* Build param types - first is ptr (obj->value), rest from vregs */
            LLVMTypeRef *param_types = (LLVMTypeRef *)malloc(sizeof(LLVMTypeRef) * nargs);
            param_types[0] = ctx->ptr_type;
            for (int i = 1; i < nargs; i++) {
                param_types[i] = llvm_get_type(ctx, f->regs[op->extra[i]]);
            }

            /*
             * For virtual method calls, the actual implementation might return a different
             * (larger) type than declared due to type erasure. For example, a method declared
             * as returning Bool might be implemented by a function returning Int.
             *
             * To handle this safely:
             * - Pointer returns: use ptr (handles covariance)
             * - Integer returns: use i64 (captures all int sizes), then truncate
             * - Float returns: use f64, then truncate if needed
             * - Void: use void
             */
            hl_type *dst_hl_type = f->regs[dst];
            bool dst_is_ptr = llvm_is_ptr_type(dst_hl_type);
            LLVMTypeRef call_ret_type;
            bool need_int_trunc = false;
            bool need_float_trunc = false;

            if (!dst_needs_result) {
                call_ret_type = LLVMVoidTypeInContext(ctx->context);
            } else if (dst_is_ptr) {
                call_ret_type = ctx->ptr_type;
            } else if (llvm_is_float_type(dst_hl_type)) {
                /* Use f64 for the call to handle potential f32->f64 covariance */
                call_ret_type = ctx->f64_type;
                need_float_trunc = (dst_hl_type->kind == HF32);
            } else {
                /* Use i64 for all integer types to handle covariance (e.g., bool->i32) */
                call_ret_type = ctx->i64_type;
                need_int_trunc = true;
            }

            LLVMTypeRef fn_type = LLVMFunctionType(call_ret_type, param_types, nargs, false);
            free(param_types);

            LLVMValueRef raw_direct = LLVMBuildCall2(ctx->builder, fn_type, vfield, args, nargs, "");
            free(args);

            /* Truncate/convert the result to destination type */
            LLVMValueRef direct_result;
            if (!dst_needs_result) {
                direct_result = NULL;
            } else if (need_int_trunc) {
                LLVMTypeRef dst_llvm_type = llvm_get_type(ctx, dst_hl_type);
                if (dst_hl_type->kind == HBOOL) {
                    /* i64 -> i8: truncate then compare != 0 isn't needed, just truncate */
                    direct_result = LLVMBuildTrunc(ctx->builder, raw_direct, dst_llvm_type, "trunc_int");
                } else {
                    direct_result = LLVMBuildTrunc(ctx->builder, raw_direct, dst_llvm_type, "trunc_int");
                }
            } else if (need_float_trunc) {
                direct_result = LLVMBuildFPTrunc(ctx->builder, raw_direct,
                    llvm_get_type(ctx, dst_hl_type), "trunc_float");
            } else {
                direct_result = raw_direct;
            }

            LLVMBuildBr(ctx->builder, merge_bb);
            LLVMBasicBlockRef direct_end = LLVMGetInsertBlock(ctx->builder);

            /* ---- Fallback path (vfield IS NULL) - call hl_dyn_call_obj ---- */
            LLVMPositionBuilderAtEnd(ctx->builder, fallback_bb);

            /* Get field type pointer for hl_dyn_call_obj
             * Use direct pointer arithmetic: if field_type is in the types array,
             * field_type - ctx->code->types gives the index directly.
             */
            hl_type *field_type = (obj_type->virt && method_idx < obj_type->virt->nfields)
                ? obj_type->virt->fields[method_idx].t : NULL;
            int type_idx = -1;
            if (field_type) {
                /* Direct pointer arithmetic - more efficient than loop */
                ptrdiff_t idx = field_type - ctx->code->types;
                if (idx >= 0 && idx < ctx->code->ntypes &&
                    ctx->code->types + idx == field_type) {
                    type_idx = (int)idx;
                }
            }
            LLVMValueRef ft_ptr = (type_idx >= 0) ? llvm_get_type_ptr(ctx, type_idx)
                : LLVMConstNull(ctx->ptr_type);

            /* Get hashed field name */
            int hfield = (obj_type->virt && method_idx < obj_type->virt->nfields)
                ? obj_type->virt->fields[method_idx].hashed_name : 0;
            LLVMValueRef hfield_val = LLVMConstInt(ctx->i32_type, hfield, true);

            /* Build args array for extra arguments (excluding obj->value which is passed separately) */
            int extra_args = nargs - 1;
            LLVMValueRef args_ptr;
            if (extra_args > 0) {
                /* Allocate args array in entry block to avoid stack growth in loops */
                LLVMTypeRef args_array_type = LLVMArrayType(ctx->ptr_type, extra_args);
                LLVMValueRef args_alloca = llvm_create_entry_alloca(ctx, args_array_type, "dyn_args");

                for (int i = 0; i < extra_args; i++) {
                    int reg_idx = op->extra[i + 1];
                    hl_type *arg_type = f->regs[reg_idx];
                    LLVMValueRef idx[] = { LLVMConstInt(ctx->i32_type, 0, false),
                                           LLVMConstInt(ctx->i32_type, i, false) };
                    LLVMValueRef slot = LLVMBuildGEP2(ctx->builder, args_array_type, args_alloca, idx, 2, "");

                    if (llvm_is_ptr_type(arg_type)) {
                        /* Pointer arg: store value directly */
                        LLVMValueRef arg_val = llvm_load_vreg(ctx, f, reg_idx);
                        LLVMBuildStore(ctx->builder, arg_val, slot);
                    } else {
                        /* Non-pointer arg: store pointer to vreg's stack location */
                        LLVMValueRef vreg_ptr = ctx->vreg_allocs[reg_idx];
                        LLVMBuildStore(ctx->builder, vreg_ptr, slot);
                    }
                }
                args_ptr = LLVMBuildBitCast(ctx->builder, args_alloca, ctx->ptr_type, "");
            } else {
                args_ptr = LLVMConstNull(ctx->ptr_type);
            }

            /* Allocate ret buffer for hl_dyn_call_obj if dst needs a result.
             * Use destination type to determine if we need a buffer and its semantics.
             * Pointer returns come back in x0, non-pointer in ret_buf+8.
             * Note: dst_is_ptr was already computed above in the direct path.
             */
            bool need_ret_buffer = dst_needs_result && !dst_is_ptr;

            LLVMValueRef ret_ptr;
            if (need_ret_buffer) {
                /* Allocate vdynamic buffer in entry block to avoid stack growth in loops */
                LLVMTypeRef vdyn_type = LLVMArrayType(ctx->i8_type, 24);
                LLVMValueRef ret_alloca = llvm_create_entry_alloca(ctx, vdyn_type, "ret_buf");
                ret_ptr = ret_alloca;
            } else {
                ret_ptr = LLVMConstNull(ctx->ptr_type);
            }

            /* Call hl_dyn_call_obj(obj->value, field_type, hashed_name, args, ret) */
            LLVMValueRef call_args[] = { obj_value, ft_ptr, hfield_val, args_ptr, ret_ptr };
            LLVMValueRef raw_result = LLVMBuildCall2(ctx->builder,
                LLVMGlobalGetValueType(ctx->rt_dyn_call_obj),
                ctx->rt_dyn_call_obj, call_args, 5, "");

            /* Extract result based on destination type:
             * - If dst is void: no result needed
             * - If dst is pointer: result is in raw_result (x0)
             * - If dst is non-pointer: result is in ret_buf via extract_vdyn_result */
            LLVMTypeRef dst_llvm_type = llvm_get_type(ctx, dst_hl_type);
            LLVMValueRef fallback_result;
            if (!dst_needs_result) {
                fallback_result = NULL;
            } else if (dst_is_ptr) {
                fallback_result = raw_result;
            } else {
                fallback_result = extract_vdyn_result(ctx, dst_hl_type, ret_ptr, dst_llvm_type);
            }

            LLVMBuildBr(ctx->builder, merge_bb);
            LLVMBasicBlockRef fallback_end = LLVMGetInsertBlock(ctx->builder);

            /* ---- Merge block ---- */
            LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);

            if (dst_needs_result) {
                /* Both paths now produce dst_llvm_type after truncation/conversion */
                LLVMValueRef phi = LLVMBuildPhi(ctx->builder, dst_llvm_type, "vmethod_result");
                LLVMValueRef incoming_vals[] = { direct_result, fallback_result };
                LLVMBasicBlockRef incoming_bbs[] = { direct_end, fallback_end };
                LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);
                llvm_store_vreg(ctx, f, dst, phi);
            }
        } else {
            /* HOBJ method call via type->vobj_proto
             * vobj_proto is an array of function pointers in hl_type at offset 16
             * Same approach as JIT: obj->type->vobj_proto[method_idx]
             *
             * IMPORTANT: For virtual calls, the actual method implementation might
             * return a different (covariant) type than declared. To avoid truncation
             * issues (e.g., declared i32 but actual returns ptr), we always use ptr
             * as the call's return type, then coerce the result.
             */
            LLVMValueRef *args = (LLVMValueRef *)malloc(sizeof(LLVMValueRef) * nargs);
            for (int i = 0; i < nargs; i++) {
                args[i] = llvm_load_vreg(ctx, f, op->extra[i]);
            }

            /* Build param types from actual argument vregs */
            LLVMTypeRef *param_types = (LLVMTypeRef *)malloc(sizeof(LLVMTypeRef) * nargs);
            for (int i = 0; i < nargs; i++) {
                param_types[i] = llvm_get_type(ctx, f->regs[op->extra[i]]);
            }

            /* Use destination register type to determine call return type.
             * Use i64 for integers, f64 for floats, ptr for pointers to handle
             * covariance where actual implementation returns a different size. */
            bool dst_needs_result = (f->regs[dst]->kind != HVOID);
            hl_type *dst_hl_type = f->regs[dst];
            bool dst_is_ptr = llvm_is_ptr_type(dst_hl_type);
            LLVMTypeRef call_ret_type;
            bool need_int_trunc = false;
            bool need_float_trunc = false;

            if (!dst_needs_result) {
                call_ret_type = LLVMVoidTypeInContext(ctx->context);
            } else if (dst_is_ptr) {
                call_ret_type = ctx->ptr_type;
            } else if (llvm_is_float_type(dst_hl_type)) {
                call_ret_type = ctx->f64_type;
                need_float_trunc = (dst_hl_type->kind == HF32);
            } else {
                call_ret_type = ctx->i64_type;
                need_int_trunc = true;
            }

            LLVMTypeRef fn_type = LLVMFunctionType(call_ret_type, param_types, nargs, false);
            free(param_types);

            /* Load type pointer from obj[0] */
            LLVMValueRef type_ptr = LLVMBuildLoad2(ctx->builder, ctx->ptr_type, obj, "type");

            /* Load vobj_proto from type[16] (hl_type: kind(4) + pad(4) + union(8) + vobj_proto) */
            LLVMValueRef vobj_proto_off = LLVMConstInt(ctx->i64_type, 16, false);
            LLVMValueRef vobj_proto_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
                type_ptr, &vobj_proto_off, 1, "");
            LLVMValueRef vobj_proto = LLVMBuildLoad2(ctx->builder, ctx->ptr_type, vobj_proto_ptr, "vobj_proto");

            /* Load method pointer from vobj_proto[method_idx] */
            LLVMValueRef method_off = LLVMConstInt(ctx->i64_type, method_idx * 8, false);
            LLVMValueRef method_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
                vobj_proto, &method_off, 1, "");
            LLVMValueRef fptr = LLVMBuildLoad2(ctx->builder, ctx->ptr_type, method_ptr, "method");

            LLVMValueRef raw_result = LLVMBuildCall2(ctx->builder, fn_type, fptr, args, nargs, "");
            free(args);

            if (dst_needs_result) {
                LLVMValueRef result;
                if (need_int_trunc) {
                    result = LLVMBuildTrunc(ctx->builder, raw_result,
                        llvm_get_type(ctx, dst_hl_type), "trunc_int");
                } else if (need_float_trunc) {
                    result = LLVMBuildFPTrunc(ctx->builder, raw_result,
                        llvm_get_type(ctx, dst_hl_type), "trunc_float");
                } else {
                    result = raw_result;
                }
                llvm_store_vreg(ctx, f, dst, result);
            }
        }
        break;
    }

    case OCallThis: {
        /* dst = this.method(args...) where this is R(0) */
        int dst = op->p1;
        int method_idx = op->p2;
        int extra_nargs = op->p3; /* Args beyond 'this' */
        int nargs = extra_nargs + 1; /* Include 'this' */

        hl_type *this_type = f->regs[0];
        LLVMValueRef this_obj = llvm_load_vreg(ctx, f, 0);

        /* Get method return type */
        hl_type *method_type = NULL;
        if (this_type->kind == HOBJ && this_type->obj) {
            if (method_idx < this_type->obj->nproto) {
                int findex = this_type->obj->proto[method_idx].findex;
                if (findex >= 0 && findex < ctx->code->nfunctions) {
                    method_type = ctx->code->functions[findex].type;
                }
            }
        } else if (this_type->kind == HVIRTUAL && this_type->virt) {
            if (method_idx < this_type->virt->nfields) {
                method_type = this_type->virt->fields[method_idx].t;
            }
        }

        /* Build function type matching actual call arguments */
        LLVMTypeRef ret_llvm_type = ctx->ptr_type;
        if (method_type && (method_type->kind == HFUN || method_type->kind == HMETHOD) && method_type->fun) {
            ret_llvm_type = llvm_get_type(ctx, method_type->fun->ret);
        }

        /* Determine if destination needs a result - use this instead of method's
         * declared return type, since methods may be declared void but bytecode
         * expects a result (covariant returns) */
        bool dst_needs_result = (f->regs[dst]->kind != HVOID);

        if (this_type->kind == HVIRTUAL) {
            /*
             * HVIRTUAL method call (same as OCallMethod HVIRTUAL path):
             * vvirtual layout: hl_type* t (0), vdynamic* value (8), vvirtual* next (16), vfields[...] (24+)
             * vfield[method_idx] is at offset 24 + method_idx * 8
             * If vfield is not NULL, call it with obj->value as first arg
             * If vfield is NULL, call hl_dyn_call_obj for dynamic dispatch
             */
            int vfield_offset = 24 + method_idx * 8;
            LLVMValueRef vfield_off_val = LLVMConstInt(ctx->i64_type, vfield_offset, false);
            LLVMValueRef vfield_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
                this_obj, &vfield_off_val, 1, "");
            LLVMValueRef vfield = LLVMBuildLoad2(ctx->builder, ctx->ptr_type, vfield_ptr, "vfield");

            /* Load obj->value (at offset 8) - this is the actual object to pass */
            LLVMValueRef value_off = LLVMConstInt(ctx->i64_type, 8, false);
            LLVMValueRef value_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
                this_obj, &value_off, 1, "");
            LLVMValueRef obj_value = LLVMBuildLoad2(ctx->builder, ctx->ptr_type, value_ptr, "obj_value");

            /* Check if vfield is NULL */
            LLVMValueRef is_null = LLVMBuildICmp(ctx->builder, LLVMIntEQ, vfield,
                LLVMConstNull(ctx->ptr_type), "vfield_is_null");

            /* Create basic blocks for branching */
            LLVMBasicBlockRef direct_bb = LLVMAppendBasicBlock(ctx->current_function, "vmethod_direct");
            LLVMBasicBlockRef fallback_bb = LLVMAppendBasicBlock(ctx->current_function, "vmethod_fallback");
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlock(ctx->current_function, "vmethod_merge");

            /* Branch on NULL check */
            LLVMBuildCondBr(ctx->builder, is_null, fallback_bb, direct_bb);

            /* ---- Direct path (vfield NOT NULL) ---- */
            LLVMPositionBuilderAtEnd(ctx->builder, direct_bb);

            /* Build args array with obj->value as first arg */
            LLVMValueRef *args = (LLVMValueRef *)malloc(sizeof(LLVMValueRef) * nargs);
            args[0] = obj_value;
            for (int i = 0; i < extra_nargs; i++) {
                args[i + 1] = llvm_load_vreg(ctx, f, op->extra[i]);
            }

            /* Build param types - first is ptr (obj->value), rest from vregs */
            LLVMTypeRef *param_types = (LLVMTypeRef *)malloc(sizeof(LLVMTypeRef) * nargs);
            param_types[0] = ctx->ptr_type;
            for (int i = 0; i < extra_nargs; i++) {
                param_types[i + 1] = llvm_get_type(ctx, f->regs[op->extra[i]]);
            }

            /*
             * For virtual method calls, the actual implementation might return a different
             * (larger) type than declared due to type erasure. For example, a method declared
             * as returning Bool might be implemented by a function returning Int.
             *
             * To handle this safely:
             * - Pointer returns: use ptr (handles covariance)
             * - Integer returns: use i64 (captures all int sizes), then truncate
             * - Float returns: use f64, then truncate if needed
             * - Void: use void
             */
            hl_type *dst_hl_type = f->regs[dst];
            bool dst_is_ptr = llvm_is_ptr_type(dst_hl_type);
            LLVMTypeRef call_ret_type;
            bool need_int_trunc = false;
            bool need_float_trunc = false;

            if (!dst_needs_result) {
                call_ret_type = LLVMVoidTypeInContext(ctx->context);
            } else if (dst_is_ptr) {
                call_ret_type = ctx->ptr_type;
            } else if (llvm_is_float_type(dst_hl_type)) {
                /* Use f64 for the call to handle potential f32->f64 covariance */
                call_ret_type = ctx->f64_type;
                need_float_trunc = (dst_hl_type->kind == HF32);
            } else {
                /* Use i64 for all integer types to handle covariance (e.g., bool->i32) */
                call_ret_type = ctx->i64_type;
                need_int_trunc = true;
            }

            LLVMTypeRef fn_type = LLVMFunctionType(call_ret_type, param_types, nargs, false);
            free(param_types);

            LLVMValueRef raw_direct = LLVMBuildCall2(ctx->builder, fn_type, vfield, args, nargs, "");
            free(args);

            /* Truncate/convert the result to destination type */
            LLVMValueRef direct_result;
            if (!dst_needs_result) {
                direct_result = NULL;
            } else if (need_int_trunc) {
                LLVMTypeRef dst_llvm_type = llvm_get_type(ctx, dst_hl_type);
                direct_result = LLVMBuildTrunc(ctx->builder, raw_direct, dst_llvm_type, "trunc_int");
            } else if (need_float_trunc) {
                direct_result = LLVMBuildFPTrunc(ctx->builder, raw_direct,
                    llvm_get_type(ctx, dst_hl_type), "trunc_float");
            } else {
                direct_result = raw_direct;
            }

            LLVMBuildBr(ctx->builder, merge_bb);
            LLVMBasicBlockRef direct_end = LLVMGetInsertBlock(ctx->builder);

            /* ---- Fallback path (vfield IS NULL) - call hl_dyn_call_obj ---- */
            LLVMPositionBuilderAtEnd(ctx->builder, fallback_bb);

            /* Get field type pointer for hl_dyn_call_obj
             * Use direct pointer arithmetic: if field_type is in the types array,
             * field_type - ctx->code->types gives the index directly.
             */
            hl_type *field_type = (this_type->virt && method_idx < this_type->virt->nfields)
                ? this_type->virt->fields[method_idx].t : NULL;
            int type_idx = -1;
            if (field_type) {
                /* Direct pointer arithmetic - more efficient than loop */
                ptrdiff_t idx = field_type - ctx->code->types;
                if (idx >= 0 && idx < ctx->code->ntypes &&
                    ctx->code->types + idx == field_type) {
                    type_idx = (int)idx;
                }
            }
            LLVMValueRef ft_ptr = (type_idx >= 0) ? llvm_get_type_ptr(ctx, type_idx)
                : LLVMConstNull(ctx->ptr_type);

            /* Get hashed field name */
            int hfield = (this_type->virt && method_idx < this_type->virt->nfields)
                ? this_type->virt->fields[method_idx].hashed_name : 0;
            LLVMValueRef hfield_val = LLVMConstInt(ctx->i32_type, hfield, true);

            /* Build args array for extra arguments (excluding obj->value which is passed separately) */
            LLVMValueRef args_ptr;
            if (extra_nargs > 0) {
                /* Allocate args array in entry block to avoid stack growth in loops */
                LLVMTypeRef args_array_type = LLVMArrayType(ctx->ptr_type, extra_nargs);
                LLVMValueRef args_alloca = llvm_create_entry_alloca(ctx, args_array_type, "dyn_args");

                for (int i = 0; i < extra_nargs; i++) {
                    int reg_idx = op->extra[i];
                    hl_type *arg_type = f->regs[reg_idx];
                    LLVMValueRef idx[] = { LLVMConstInt(ctx->i32_type, 0, false),
                                           LLVMConstInt(ctx->i32_type, i, false) };
                    LLVMValueRef slot = LLVMBuildGEP2(ctx->builder, args_array_type, args_alloca, idx, 2, "");

                    if (llvm_is_ptr_type(arg_type)) {
                        /* Pointer arg: store value directly */
                        LLVMValueRef arg_val = llvm_load_vreg(ctx, f, reg_idx);
                        LLVMBuildStore(ctx->builder, arg_val, slot);
                    } else {
                        /* Non-pointer arg: store pointer to vreg's stack location */
                        LLVMValueRef vreg_ptr = ctx->vreg_allocs[reg_idx];
                        LLVMBuildStore(ctx->builder, vreg_ptr, slot);
                    }
                }
                args_ptr = LLVMBuildBitCast(ctx->builder, args_alloca, ctx->ptr_type, "");
            } else {
                args_ptr = LLVMConstNull(ctx->ptr_type);
            }

            /* Allocate ret buffer for hl_dyn_call_obj if dst needs a result.
             * Use destination type to determine if we need a buffer and its semantics.
             * Pointer returns come back in x0, non-pointer in ret_buf+8.
             * Note: dst_is_ptr was already computed above in the direct path.
             */
            bool need_ret_buffer = dst_needs_result && !dst_is_ptr;

            LLVMValueRef ret_ptr;
            if (need_ret_buffer) {
                /* Allocate vdynamic buffer in entry block to avoid stack growth in loops */
                LLVMTypeRef vdyn_type = LLVMArrayType(ctx->i8_type, 24);
                LLVMValueRef ret_alloca = llvm_create_entry_alloca(ctx, vdyn_type, "ret_buf");
                ret_ptr = ret_alloca;
            } else {
                ret_ptr = LLVMConstNull(ctx->ptr_type);
            }

            /* Call hl_dyn_call_obj(obj->value, field_type, hashed_name, args, ret) */
            LLVMValueRef call_args[] = { obj_value, ft_ptr, hfield_val, args_ptr, ret_ptr };
            LLVMValueRef raw_result = LLVMBuildCall2(ctx->builder,
                LLVMGlobalGetValueType(ctx->rt_dyn_call_obj),
                ctx->rt_dyn_call_obj, call_args, 5, "");

            /* Extract result based on destination type:
             * - If dst is void: no result needed
             * - If dst is pointer: result is in raw_result (x0)
             * - If dst is non-pointer: result is in ret_buf via extract_vdyn_result */
            LLVMTypeRef dst_llvm_type = llvm_get_type(ctx, dst_hl_type);
            LLVMValueRef fallback_result;
            if (!dst_needs_result) {
                fallback_result = NULL;
            } else if (dst_is_ptr) {
                fallback_result = raw_result;
            } else {
                fallback_result = extract_vdyn_result(ctx, dst_hl_type, ret_ptr, dst_llvm_type);
            }

            LLVMBuildBr(ctx->builder, merge_bb);
            LLVMBasicBlockRef fallback_end = LLVMGetInsertBlock(ctx->builder);

            /* ---- Merge block ---- */
            LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);

            if (dst_needs_result) {
                /* Both paths now produce dst_llvm_type after truncation/conversion */
                LLVMValueRef phi = LLVMBuildPhi(ctx->builder, dst_llvm_type, "vmethod_result");
                LLVMValueRef incoming_vals[] = { direct_result, fallback_result };
                LLVMBasicBlockRef incoming_bbs[] = { direct_end, fallback_end };
                LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);
                llvm_store_vreg(ctx, f, dst, phi);
            }
        } else {
            /* HOBJ method call via type->vobj_proto
             *
             * IMPORTANT: For virtual calls, the actual method implementation might
             * return a different (covariant) type than declared. To avoid truncation
             * issues (e.g., declared i32 but actual returns ptr), we always use ptr
             * as the call's return type, then coerce the result.
             */
            LLVMValueRef *args = (LLVMValueRef *)malloc(sizeof(LLVMValueRef) * nargs);
            args[0] = this_obj;
            for (int i = 0; i < extra_nargs; i++) {
                args[i + 1] = llvm_load_vreg(ctx, f, op->extra[i]);
            }

            /* Build param types from actual argument vregs */
            LLVMTypeRef *param_types = (LLVMTypeRef *)malloc(sizeof(LLVMTypeRef) * nargs);
            param_types[0] = llvm_get_type(ctx, f->regs[0]); /* this */
            for (int i = 0; i < extra_nargs; i++) {
                param_types[i + 1] = llvm_get_type(ctx, f->regs[op->extra[i]]);
            }

            /* Use destination register type to determine call return type.
             * Use i64 for integers, f64 for floats, ptr for pointers to handle
             * covariance where actual implementation returns a different size. */
            hl_type *dst_hl_type = f->regs[dst];
            bool dst_is_ptr = llvm_is_ptr_type(dst_hl_type);
            LLVMTypeRef call_ret_type;
            bool need_int_trunc = false;
            bool need_float_trunc = false;

            if (!dst_needs_result) {
                call_ret_type = LLVMVoidTypeInContext(ctx->context);
            } else if (dst_is_ptr) {
                call_ret_type = ctx->ptr_type;
            } else if (llvm_is_float_type(dst_hl_type)) {
                call_ret_type = ctx->f64_type;
                need_float_trunc = (dst_hl_type->kind == HF32);
            } else {
                call_ret_type = ctx->i64_type;
                need_int_trunc = true;
            }

            LLVMTypeRef fn_type = LLVMFunctionType(call_ret_type, param_types, nargs, false);
            free(param_types);

            /* Load method pointer via type->vobj_proto */
            LLVMValueRef type_ptr = LLVMBuildLoad2(ctx->builder, ctx->ptr_type, this_obj, "type");

            /* Load vobj_proto from type[16] */
            LLVMValueRef vobj_proto_off = LLVMConstInt(ctx->i64_type, 16, false);
            LLVMValueRef vobj_proto_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
                type_ptr, &vobj_proto_off, 1, "");
            LLVMValueRef vobj_proto = LLVMBuildLoad2(ctx->builder, ctx->ptr_type, vobj_proto_ptr, "vobj_proto");

            /* Load method pointer from vobj_proto[method_idx] */
            LLVMValueRef method_off = LLVMConstInt(ctx->i64_type, method_idx * 8, false);
            LLVMValueRef method_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
                vobj_proto, &method_off, 1, "");
            LLVMValueRef fptr = LLVMBuildLoad2(ctx->builder, ctx->ptr_type, method_ptr, "method");

            LLVMValueRef raw_result = LLVMBuildCall2(ctx->builder, fn_type, fptr, args, nargs, "");
            free(args);

            if (dst_needs_result) {
                LLVMValueRef result;
                if (need_int_trunc) {
                    result = LLVMBuildTrunc(ctx->builder, raw_result,
                        llvm_get_type(ctx, dst_hl_type), "trunc_int");
                } else if (need_float_trunc) {
                    result = LLVMBuildFPTrunc(ctx->builder, raw_result,
                        llvm_get_type(ctx, dst_hl_type), "trunc_float");
                } else {
                    result = raw_result;
                }
                llvm_store_vreg(ctx, f, dst, result);
            }
        }
        break;
    }

    case OCallClosure: {
        /* dst = closure(args...) */
        int dst = op->p1;
        int closure_reg = op->p2;
        int nargs = op->p3;

        LLVMValueRef closure = llvm_load_vreg(ctx, f, closure_reg);
        hl_type *closure_type = f->regs[closure_reg];

        if (closure_type->kind == HDYN) {
            /* Dynamic closure: use hl_dyn_call
             * For HDYN closures, the bytecode ensures all args are already vdynamic*
             * (via OToDyn or already HDYN type). We just store them directly into
             * the args array - no need to create vdynamic wrappers.
             * This matches the JIT behavior which stores arg values directly. */
            LLVMTypeRef args_array_type = LLVMArrayType(ctx->ptr_type, nargs > 0 ? nargs : 1);
            LLVMValueRef args_array = llvm_create_entry_alloca(ctx, args_array_type, "args_array");

            for (int i = 0; i < nargs; i++) {
                int arg_reg = op->extra[i];
                LLVMValueRef arg = llvm_load_vreg(ctx, f, arg_reg);

                /* Store arg (already a vdynamic*) directly into args array */
                LLVMValueRef args_indices[] = {
                    LLVMConstInt(ctx->i32_type, 0, false),
                    LLVMConstInt(ctx->i32_type, i, false)
                };
                LLVMValueRef args_slot = LLVMBuildGEP2(ctx->builder, args_array_type, args_array, args_indices, 2, "");
                LLVMBuildStore(ctx->builder, arg, args_slot);
            }

            LLVMValueRef call_args[] = { closure, args_array, LLVMConstInt(ctx->i32_type, nargs, false) };
            LLVMValueRef result = LLVMBuildCall2(ctx->builder,
                LLVMGlobalGetValueType(ctx->rt_dyn_call),
                ctx->rt_dyn_call, call_args, 3, "");

            if (f->regs[dst]->kind != HVOID) {
                result = coerce_call_result(ctx, f, dst, result, ctx->ptr_type);
                llvm_store_vreg(ctx, f, dst, result);
            }
        } else {
            /* Typed closure: direct call via closure->fun
             * vclosure structure: { hl_type* t (0), void* fun (8), int hasValue (16), pad (20), void* value (24) }
             * If hasValue, prepend value as first argument */

            /* Load function pointer */
            LLVMValueRef fun_offset = LLVMConstInt(ctx->i64_type, 8, false);
            LLVMValueRef fun_ptr = LLVMBuildLoad2(ctx->builder, ctx->ptr_type,
                LLVMBuildGEP2(ctx->builder, ctx->i8_type, closure, &fun_offset, 1, ""), "fun");

            /* Load hasValue */
            LLVMValueRef has_val_offset = LLVMConstInt(ctx->i64_type, 16, false);
            LLVMValueRef has_val = LLVMBuildLoad2(ctx->builder, ctx->i32_type,
                LLVMBuildGEP2(ctx->builder, ctx->i8_type, closure, &has_val_offset, 1, ""), "hasValue");

            /* Load value pointer */
            LLVMValueRef val_ptr_offset = LLVMConstInt(ctx->i64_type, 24, false);
            LLVMValueRef closure_val = LLVMBuildLoad2(ctx->builder, ctx->ptr_type,
                LLVMBuildGEP2(ctx->builder, ctx->i8_type, closure, &val_ptr_offset, 1, ""), "value");

            /* Build return type - for indirect calls, we need to balance two concerns:
             * 1. Float/double MUST use the correct type (calling convention uses FP registers)
             * 2. Integer/pointer types might benefit from ptr to handle covariant returns
             *    (declared i32 but actual returns ptr) */
            LLVMTypeRef ret_type = ctx->ptr_type;
            if ((closure_type->kind == HFUN || closure_type->kind == HMETHOD) && closure_type->fun) {
                LLVMTypeRef declared_ret = llvm_get_type(ctx, closure_type->fun->ret);
                LLVMTypeKind kind = LLVMGetTypeKind(declared_ret);
                /* Use declared type for void and floating point types (calling convention matters)
                 * For integers/pointers, use ptr to handle covariant returns */
                if (kind == LLVMVoidTypeKind || kind == LLVMFloatTypeKind || kind == LLVMDoubleTypeKind) {
                    ret_type = declared_ret;
                }
            }

            /* Load all arguments BEFORE the branch (so they're available in both paths)
             * Skip void-typed arguments - they are placeholders in HashLink bytecode
             * (e.g., fun(void)->X is encoded with nargs=1 where arg is HVOID) */
            LLVMValueRef *arg_vals = (LLVMValueRef *)malloc(sizeof(LLVMValueRef) * (nargs > 0 ? nargs : 1));
            LLVMTypeRef *arg_types = (LLVMTypeRef *)malloc(sizeof(LLVMTypeRef) * (nargs > 0 ? nargs : 1));
            int actual_nargs = 0;
            for (int i = 0; i < nargs; i++) {
                hl_type *arg_t = f->regs[op->extra[i]];
                if (arg_t->kind == HVOID) continue;  /* Skip void args */
                arg_vals[actual_nargs] = llvm_load_vreg(ctx, f, op->extra[i]);
                arg_types[actual_nargs] = llvm_get_type(ctx, arg_t);
                actual_nargs++;
            }

            /* Check hasValue to determine call path */
            LLVMValueRef zero = LLVMConstInt(ctx->i32_type, 0, false);
            LLVMValueRef has_value_cond = LLVMBuildICmp(ctx->builder, LLVMIntNE, has_val, zero, "");

            LLVMBasicBlockRef has_val_bb = LLVMAppendBasicBlock(ctx->current_function, "has_value");
            LLVMBasicBlockRef no_val_bb = LLVMAppendBasicBlock(ctx->current_function, "no_value");
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlock(ctx->current_function, "closure_merge");

            LLVMBuildCondBr(ctx->builder, has_value_cond, has_val_bb, no_val_bb);

            /* Has-value path: call with value as first arg */
            LLVMPositionBuilderAtEnd(ctx->builder, has_val_bb);
            LLVMValueRef result_has;
            {
                int total_args = actual_nargs + 1;
                LLVMValueRef *args = (LLVMValueRef *)malloc(sizeof(LLVMValueRef) * total_args);
                LLVMTypeRef *param_types = (LLVMTypeRef *)malloc(sizeof(LLVMTypeRef) * total_args);

                args[0] = closure_val;
                param_types[0] = ctx->ptr_type;
                for (int i = 0; i < actual_nargs; i++) {
                    args[i + 1] = arg_vals[i];
                    param_types[i + 1] = arg_types[i];
                }

                LLVMTypeRef fn_type = LLVMFunctionType(ret_type, param_types, total_args, false);
                result_has = LLVMBuildCall2(ctx->builder, fn_type, fun_ptr, args, total_args, "");

                free(args);
                free(param_types);

                LLVMBuildBr(ctx->builder, merge_bb);
                has_val_bb = LLVMGetInsertBlock(ctx->builder);
            }

            /* No-value path: call without extra arg */
            LLVMPositionBuilderAtEnd(ctx->builder, no_val_bb);
            LLVMValueRef result_no;
            {
                LLVMTypeRef fn_type = LLVMFunctionType(ret_type, arg_types, actual_nargs, false);
                result_no = LLVMBuildCall2(ctx->builder, fn_type, fun_ptr, arg_vals, actual_nargs, "");

                LLVMBuildBr(ctx->builder, merge_bb);
                no_val_bb = LLVMGetInsertBlock(ctx->builder);
            }

            free(arg_vals);
            free(arg_types);

            /* Merge and phi */
            LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);
            if (f->regs[dst]->kind != HVOID && LLVMGetTypeKind(ret_type) != LLVMVoidTypeKind) {
                LLVMValueRef phi = LLVMBuildPhi(ctx->builder, ret_type, "closure_result");
                LLVMValueRef incoming_vals[] = { result_has, result_no };
                LLVMBasicBlockRef incoming_blocks[] = { has_val_bb, no_val_bb };
                LLVMAddIncoming(phi, incoming_vals, incoming_blocks, 2);

                LLVMValueRef result = coerce_call_result(ctx, f, dst, phi, ret_type);
                llvm_store_vreg(ctx, f, dst, result);
            }
        }
        break;
    }

    default:
        break;
    }
}
