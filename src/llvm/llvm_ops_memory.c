/*
 * Copyright (C)2005-2016 Haxe Foundation
 * LLVM Backend - Memory Access Opcodes
 */
#include "llvm_codegen.h"

void llvm_emit_memory(llvm_ctx *ctx, hl_function *f, hl_opcode *op, int op_idx) {
    switch (op->op) {
    case OGetGlobal: {
        /*
         * dst = globals[idx]
         *
         * Call aot_get_global(idx) to get pointer to the global's storage.
         * This returns &aot_globals[idx] where aot_globals points to the
         * module's globals array (initialized by aot_init_module_data).
         */
        int dst = op->p1;
        int idx = op->p2;
        hl_type *t = f->regs[dst];
        LLVMTypeRef llvm_type = llvm_get_type(ctx, t);

        /* Call aot_get_global(idx) to get pointer to global slot */
        LLVMValueRef idx_val = LLVMConstInt(ctx->i32_type, idx, false);
        LLVMValueRef args[] = { idx_val };
        LLVMTypeRef fn_type = LLVMFunctionType(ctx->ptr_type, (LLVMTypeRef[]){ ctx->i32_type }, 1, false);
        LLVMValueRef global_ptr = LLVMBuildCall2(ctx->builder, fn_type,
            ctx->rt_aot_get_global, args, 1, "global_ptr");

        /* Load the value from the global slot */
        LLVMValueRef val = LLVMBuildLoad2(ctx->builder, llvm_type, global_ptr, "");
        llvm_store_vreg(ctx, f, dst, val);
        break;
    }

    case OSetGlobal: {
        /*
         * globals[idx] = src
         *
         * Call aot_get_global(idx) to get pointer to the global's storage,
         * then store the value there.
         */
        int idx = op->p1;
        int src = op->p2;
        LLVMValueRef val = llvm_load_vreg(ctx, f, src);

        /* Call aot_get_global(idx) to get pointer to global slot */
        LLVMValueRef idx_val = LLVMConstInt(ctx->i32_type, idx, false);
        LLVMValueRef args[] = { idx_val };
        LLVMTypeRef fn_type = LLVMFunctionType(ctx->ptr_type, (LLVMTypeRef[]){ ctx->i32_type }, 1, false);
        LLVMValueRef global_ptr = LLVMBuildCall2(ctx->builder, fn_type,
            ctx->rt_aot_get_global, args, 1, "global_ptr");

        LLVMBuildStore(ctx->builder, val, global_ptr);
        break;
    }

    case OField: {
        /* dst = obj.field[idx] */
        int dst = op->p1;
        int obj = op->p2;
        int field_idx = op->p3;
        hl_type *obj_type = f->regs[obj];
        hl_type *dst_type = f->regs[dst];

        LLVMValueRef obj_ptr = llvm_load_vreg(ctx, f, obj);
        LLVMTypeRef field_type = llvm_get_type(ctx, dst_type);

        if (obj_type->kind == HOBJ || obj_type->kind == HSTRUCT) {
            /* For objects/structs, get runtime field offsets */
            int offset = 0;
            hl_runtime_obj *rt = hl_get_obj_rt(obj_type);
            if (rt && field_idx < rt->nfields) {
                offset = rt->fields_indexes[field_idx];
            }
            LLVMValueRef offset_val = LLVMConstInt(ctx->i64_type, offset, false);
            LLVMValueRef field_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
                obj_ptr, &offset_val, 1, "");

            /* Check for packed field -> struct destination (LEA semantics)
             * If dst is HSTRUCT and field is HPACKED, return pointer to inline storage */
            hl_obj_field *fld = hl_obj_field_fetch(obj_type, field_idx);
            if (dst_type->kind == HSTRUCT && fld && fld->t->kind == HPACKED) {
                /* Return address of inline storage: dst = &obj->field */
                llvm_store_vreg(ctx, f, dst, field_ptr);
            } else {
                LLVMValueRef val = LLVMBuildLoad2(ctx->builder, field_type, field_ptr, "");
                llvm_store_vreg(ctx, f, dst, val);
            }
        } else if (obj_type->kind == HVIRTUAL && obj_type->virt && field_idx < obj_type->virt->nfields) {
            /*
             * HVIRTUAL field access:
             * vvirtual layout: hl_type* t (0), vdynamic* value (8), vvirtual* next (16), void* vfields[] (24+)
             * vfields[field_idx] is a POINTER to where the field data is stored
             * If vfield != NULL: result = *vfield (dereference)
             * If vfield == NULL: result = hl_dyn_get*(obj, hashed_name, ...)
             */
            int vfield_offset = 24 + field_idx * 8;  /* sizeof(vvirtual) = 24 */
            LLVMValueRef vfield_off_val = LLVMConstInt(ctx->i64_type, vfield_offset, false);
            LLVMValueRef vfield_ptr_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
                obj_ptr, &vfield_off_val, 1, "");
            LLVMValueRef vfield_ptr = LLVMBuildLoad2(ctx->builder, ctx->ptr_type, vfield_ptr_ptr, "vfield");

            /* Check if vfield is NULL */
            LLVMValueRef is_null = LLVMBuildICmp(ctx->builder, LLVMIntEQ, vfield_ptr,
                LLVMConstNull(ctx->ptr_type), "");

            /* Create basic blocks for branching */
            LLVMBasicBlockRef has_vfield_bb = LLVMAppendBasicBlock(ctx->current_function, "has_vfield");
            LLVMBasicBlockRef null_vfield_bb = LLVMAppendBasicBlock(ctx->current_function, "null_vfield");
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlock(ctx->current_function, "field_merge");

            LLVMBuildCondBr(ctx->builder, is_null, null_vfield_bb, has_vfield_bb);

            /* has_vfield path: dereference vfield pointer */
            LLVMPositionBuilderAtEnd(ctx->builder, has_vfield_bb);
            LLVMValueRef direct_val = LLVMBuildLoad2(ctx->builder, field_type, vfield_ptr, "");
            LLVMBuildBr(ctx->builder, merge_bb);
            LLVMBasicBlockRef has_vfield_end = LLVMGetInsertBlock(ctx->builder);

            /* null_vfield path: call hl_dyn_get* */
            LLVMPositionBuilderAtEnd(ctx->builder, null_vfield_bb);
            int hashed_name = obj_type->virt->fields[field_idx].hashed_name;
            LLVMValueRef hash_val = LLVMConstInt(ctx->i32_type, hashed_name, false);

            LLVMValueRef dyn_result;
            switch (dst_type->kind) {
            case HF32: {
                LLVMValueRef args[] = { obj_ptr, hash_val };
                dyn_result = LLVMBuildCall2(ctx->builder,
                    LLVMGlobalGetValueType(ctx->rt_dyn_getf),
                    ctx->rt_dyn_getf, args, 2, "");
                break;
            }
            case HF64: {
                LLVMValueRef args[] = { obj_ptr, hash_val };
                dyn_result = LLVMBuildCall2(ctx->builder,
                    LLVMGlobalGetValueType(ctx->rt_dyn_getd),
                    ctx->rt_dyn_getd, args, 2, "");
                break;
            }
            case HI64: {
                LLVMValueRef args[] = { obj_ptr, hash_val };
                dyn_result = LLVMBuildCall2(ctx->builder,
                    LLVMGlobalGetValueType(ctx->rt_dyn_geti64),
                    ctx->rt_dyn_geti64, args, 2, "");
                break;
            }
            case HI32:
            case HUI8:
            case HUI16:
            case HBOOL: {
                /* Get type pointer for dst_type */
                int type_idx = -1;
                for (int i = 0; i < ctx->code->ntypes; i++) {
                    if (ctx->code->types + i == dst_type) {
                        type_idx = i;
                        break;
                    }
                }
                LLVMValueRef type_ptr = type_idx >= 0 ? llvm_get_type_ptr(ctx, type_idx)
                    : LLVMConstNull(ctx->ptr_type);
                LLVMValueRef args[] = { obj_ptr, hash_val, type_ptr };
                LLVMValueRef i32_result = LLVMBuildCall2(ctx->builder,
                    LLVMGlobalGetValueType(ctx->rt_dyn_geti),
                    ctx->rt_dyn_geti, args, 3, "");
                /* Truncate if needed */
                if (dst_type->kind == HUI8 || dst_type->kind == HBOOL) {
                    dyn_result = LLVMBuildTrunc(ctx->builder, i32_result, ctx->i8_type, "");
                } else if (dst_type->kind == HUI16) {
                    dyn_result = LLVMBuildTrunc(ctx->builder, i32_result, ctx->i16_type, "");
                } else {
                    dyn_result = i32_result;
                }
                break;
            }
            default: {
                /* Pointer types - use hl_dyn_getp */
                int type_idx = -1;
                for (int i = 0; i < ctx->code->ntypes; i++) {
                    if (ctx->code->types + i == dst_type) {
                        type_idx = i;
                        break;
                    }
                }
                LLVMValueRef type_ptr = type_idx >= 0 ? llvm_get_type_ptr(ctx, type_idx)
                    : LLVMConstNull(ctx->ptr_type);
                LLVMValueRef args[] = { obj_ptr, hash_val, type_ptr };
                dyn_result = LLVMBuildCall2(ctx->builder,
                    LLVMGlobalGetValueType(ctx->rt_dyn_getp),
                    ctx->rt_dyn_getp, args, 3, "");
                break;
            }
            }
            LLVMBuildBr(ctx->builder, merge_bb);
            LLVMBasicBlockRef null_vfield_end = LLVMGetInsertBlock(ctx->builder);

            /* Merge: use phi node to get result */
            LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);
            LLVMValueRef phi = LLVMBuildPhi(ctx->builder, field_type, "field_result");
            LLVMValueRef incoming_vals[] = { direct_val, dyn_result };
            LLVMBasicBlockRef incoming_bbs[] = { has_vfield_end, null_vfield_end };
            LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);

            llvm_store_vreg(ctx, f, dst, phi);
        } else {
            /* Unknown type - just load from offset 0 */
            LLVMValueRef val = LLVMBuildLoad2(ctx->builder, field_type, obj_ptr, "");
            llvm_store_vreg(ctx, f, dst, val);
        }
        break;
    }

    case OSetField: {
        /* obj.field[idx] = src */
        int obj = op->p1;
        int field_idx = op->p2;
        int src = op->p3;
        hl_type *obj_type = f->regs[obj];
        hl_type *src_type = f->regs[src];

        LLVMValueRef obj_ptr = llvm_load_vreg(ctx, f, obj);
        LLVMValueRef val = llvm_load_vreg(ctx, f, src);

        if (obj_type->kind == HOBJ || obj_type->kind == HSTRUCT) {
            int offset = 0;
            hl_runtime_obj *rt = hl_get_obj_rt(obj_type);
            if (rt && field_idx < rt->nfields) {
                offset = rt->fields_indexes[field_idx];
            }
            LLVMValueRef offset_val = LLVMConstInt(ctx->i64_type, offset, false);
            LLVMValueRef field_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
                obj_ptr, &offset_val, 1, "");

            /* Check for struct-to-packed-field assignment
             * If src is HSTRUCT and field is HPACKED, copy entire struct */
            hl_obj_field *fld = hl_obj_field_fetch(obj_type, field_idx);
            if (src_type->kind == HSTRUCT && fld && fld->t->kind == HPACKED) {
                /* Copy struct byte-by-byte using memcpy
                 * val is a pointer to the source struct (HSTRUCT is passed by pointer)
                 * field_ptr is the destination (inline storage in the object) */
                hl_runtime_obj *src_rt = hl_get_obj_rt(fld->t->tparam);
                if (src_rt) {
                    LLVMValueRef size = LLVMConstInt(ctx->i64_type, src_rt->size, false);
                    LLVMBuildMemCpy(ctx->builder, field_ptr, 1, val, 1, size);
                }
            } else {
                LLVMBuildStore(ctx->builder, val, field_ptr);
            }
        } else if (obj_type->kind == HVIRTUAL && obj_type->virt && field_idx < obj_type->virt->nfields) {
            /*
             * HVIRTUAL field set:
             * vvirtual layout: hl_type* t (0), vdynamic* value (8), vvirtual* next (16), void* vfields[] (24+)
             * vfields[field_idx] is a POINTER to where the field data is stored
             * If vfield != NULL: *vfield = val (store through pointer)
             * If vfield == NULL: hl_dyn_set*(obj, hashed_name, ..., val)
             */
            int vfield_offset = 24 + field_idx * 8;  /* sizeof(vvirtual) = 24 */
            LLVMValueRef vfield_off_val = LLVMConstInt(ctx->i64_type, vfield_offset, false);
            LLVMValueRef vfield_ptr_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
                obj_ptr, &vfield_off_val, 1, "");
            LLVMValueRef vfield_ptr = LLVMBuildLoad2(ctx->builder, ctx->ptr_type, vfield_ptr_ptr, "vfield");

            /* Check if vfield is NULL */
            LLVMValueRef is_null = LLVMBuildICmp(ctx->builder, LLVMIntEQ, vfield_ptr,
                LLVMConstNull(ctx->ptr_type), "");

            /* Create basic blocks for branching */
            LLVMBasicBlockRef has_vfield_bb = LLVMAppendBasicBlock(ctx->current_function, "has_vfield_set");
            LLVMBasicBlockRef null_vfield_bb = LLVMAppendBasicBlock(ctx->current_function, "null_vfield_set");
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlock(ctx->current_function, "setfield_merge");

            LLVMBuildCondBr(ctx->builder, is_null, null_vfield_bb, has_vfield_bb);

            /* has_vfield path: store through vfield pointer */
            LLVMPositionBuilderAtEnd(ctx->builder, has_vfield_bb);
            LLVMBuildStore(ctx->builder, val, vfield_ptr);
            LLVMBuildBr(ctx->builder, merge_bb);

            /* null_vfield path: call hl_dyn_set* */
            LLVMPositionBuilderAtEnd(ctx->builder, null_vfield_bb);
            int hashed_name = obj_type->virt->fields[field_idx].hashed_name;
            LLVMValueRef hash_val = LLVMConstInt(ctx->i32_type, hashed_name, false);

            switch (src_type->kind) {
            case HF32: {
                LLVMValueRef args[] = { obj_ptr, hash_val, val };
                LLVMBuildCall2(ctx->builder,
                    LLVMGlobalGetValueType(ctx->rt_dyn_setf),
                    ctx->rt_dyn_setf, args, 3, "");
                break;
            }
            case HF64: {
                LLVMValueRef args[] = { obj_ptr, hash_val, val };
                LLVMBuildCall2(ctx->builder,
                    LLVMGlobalGetValueType(ctx->rt_dyn_setd),
                    ctx->rt_dyn_setd, args, 3, "");
                break;
            }
            case HI64: {
                LLVMValueRef args[] = { obj_ptr, hash_val, val };
                LLVMBuildCall2(ctx->builder,
                    LLVMGlobalGetValueType(ctx->rt_dyn_seti64),
                    ctx->rt_dyn_seti64, args, 3, "");
                break;
            }
            case HI32:
            case HUI8:
            case HUI16:
            case HBOOL: {
                /* Get type pointer for src_type */
                int type_idx = -1;
                for (int i = 0; i < ctx->code->ntypes; i++) {
                    if (ctx->code->types + i == src_type) {
                        type_idx = i;
                        break;
                    }
                }
                LLVMValueRef type_ptr = type_idx >= 0 ? llvm_get_type_ptr(ctx, type_idx)
                    : LLVMConstNull(ctx->ptr_type);
                /* Extend smaller types to i32 for hl_dyn_seti */
                LLVMValueRef i32_val = val;
                if (src_type->kind == HUI8 || src_type->kind == HBOOL) {
                    i32_val = LLVMBuildZExt(ctx->builder, val, ctx->i32_type, "");
                } else if (src_type->kind == HUI16) {
                    i32_val = LLVMBuildZExt(ctx->builder, val, ctx->i32_type, "");
                }
                LLVMValueRef args[] = { obj_ptr, hash_val, type_ptr, i32_val };
                LLVMBuildCall2(ctx->builder,
                    LLVMGlobalGetValueType(ctx->rt_dyn_seti),
                    ctx->rt_dyn_seti, args, 4, "");
                break;
            }
            default: {
                /* Pointer types - use hl_dyn_setp */
                int type_idx = -1;
                for (int i = 0; i < ctx->code->ntypes; i++) {
                    if (ctx->code->types + i == src_type) {
                        type_idx = i;
                        break;
                    }
                }
                LLVMValueRef type_ptr = type_idx >= 0 ? llvm_get_type_ptr(ctx, type_idx)
                    : LLVMConstNull(ctx->ptr_type);
                LLVMValueRef args[] = { obj_ptr, hash_val, type_ptr, val };
                LLVMBuildCall2(ctx->builder,
                    LLVMGlobalGetValueType(ctx->rt_dyn_setp),
                    ctx->rt_dyn_setp, args, 4, "");
                break;
            }
            }
            LLVMBuildBr(ctx->builder, merge_bb);

            /* Continue after merge */
            LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);
        } else {
            /* Unknown type - just store at offset 0 */
            LLVMBuildStore(ctx->builder, val, obj_ptr);
        }
        break;
    }

    case OGetThis: {
        /* dst = this.field[idx] (this is R(0)) */
        int dst = op->p1;
        int field_idx = op->p2;
        hl_type *this_type = f->regs[0];
        hl_type *dst_type = f->regs[dst];

        LLVMValueRef this_ptr = llvm_load_vreg(ctx, f, 0);
        LLVMTypeRef field_type = llvm_get_type(ctx, dst_type);

        if (this_type->kind == HOBJ || this_type->kind == HSTRUCT) {
            int offset = 0;
            hl_runtime_obj *rt = hl_get_obj_rt(this_type);
            if (rt && field_idx < rt->nfields) {
                offset = rt->fields_indexes[field_idx];
            }
            LLVMValueRef offset_val = LLVMConstInt(ctx->i64_type, offset, false);
            LLVMValueRef field_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
                this_ptr, &offset_val, 1, "");
            LLVMValueRef val = LLVMBuildLoad2(ctx->builder, field_type, field_ptr, "");
            llvm_store_vreg(ctx, f, dst, val);
        } else if (this_type->kind == HVIRTUAL && this_type->virt && field_idx < this_type->virt->nfields) {
            /* HVIRTUAL field access - same as OField HVIRTUAL case */
            int vfield_offset = 24 + field_idx * 8;
            LLVMValueRef vfield_off_val = LLVMConstInt(ctx->i64_type, vfield_offset, false);
            LLVMValueRef vfield_ptr_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
                this_ptr, &vfield_off_val, 1, "");
            LLVMValueRef vfield_ptr = LLVMBuildLoad2(ctx->builder, ctx->ptr_type, vfield_ptr_ptr, "vfield");

            LLVMValueRef is_null = LLVMBuildICmp(ctx->builder, LLVMIntEQ, vfield_ptr,
                LLVMConstNull(ctx->ptr_type), "");

            LLVMBasicBlockRef has_vfield_bb = LLVMAppendBasicBlock(ctx->current_function, "this_has_vfield");
            LLVMBasicBlockRef null_vfield_bb = LLVMAppendBasicBlock(ctx->current_function, "this_null_vfield");
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlock(ctx->current_function, "this_field_merge");

            LLVMBuildCondBr(ctx->builder, is_null, null_vfield_bb, has_vfield_bb);

            /* has_vfield path */
            LLVMPositionBuilderAtEnd(ctx->builder, has_vfield_bb);
            LLVMValueRef direct_val = LLVMBuildLoad2(ctx->builder, field_type, vfield_ptr, "");
            LLVMBuildBr(ctx->builder, merge_bb);
            LLVMBasicBlockRef has_vfield_end = LLVMGetInsertBlock(ctx->builder);

            /* null_vfield path */
            LLVMPositionBuilderAtEnd(ctx->builder, null_vfield_bb);
            int hashed_name = this_type->virt->fields[field_idx].hashed_name;
            LLVMValueRef hash_val = LLVMConstInt(ctx->i32_type, hashed_name, false);

            LLVMValueRef dyn_result;
            switch (dst_type->kind) {
            case HF32: {
                LLVMValueRef args[] = { this_ptr, hash_val };
                dyn_result = LLVMBuildCall2(ctx->builder,
                    LLVMGlobalGetValueType(ctx->rt_dyn_getf),
                    ctx->rt_dyn_getf, args, 2, "");
                break;
            }
            case HF64: {
                LLVMValueRef args[] = { this_ptr, hash_val };
                dyn_result = LLVMBuildCall2(ctx->builder,
                    LLVMGlobalGetValueType(ctx->rt_dyn_getd),
                    ctx->rt_dyn_getd, args, 2, "");
                break;
            }
            case HI64: {
                LLVMValueRef args[] = { this_ptr, hash_val };
                dyn_result = LLVMBuildCall2(ctx->builder,
                    LLVMGlobalGetValueType(ctx->rt_dyn_geti64),
                    ctx->rt_dyn_geti64, args, 2, "");
                break;
            }
            default: {
                int type_idx = -1;
                for (int i = 0; i < ctx->code->ntypes; i++) {
                    if (ctx->code->types + i == dst_type) {
                        type_idx = i;
                        break;
                    }
                }
                LLVMValueRef type_ptr = type_idx >= 0 ? llvm_get_type_ptr(ctx, type_idx)
                    : LLVMConstNull(ctx->ptr_type);
                if (dst_type->kind == HI32 || dst_type->kind == HUI8 ||
                    dst_type->kind == HUI16 || dst_type->kind == HBOOL) {
                    LLVMValueRef args[] = { this_ptr, hash_val, type_ptr };
                    LLVMValueRef i32_result = LLVMBuildCall2(ctx->builder,
                        LLVMGlobalGetValueType(ctx->rt_dyn_geti),
                        ctx->rt_dyn_geti, args, 3, "");
                    if (dst_type->kind == HUI8 || dst_type->kind == HBOOL) {
                        dyn_result = LLVMBuildTrunc(ctx->builder, i32_result, ctx->i8_type, "");
                    } else if (dst_type->kind == HUI16) {
                        dyn_result = LLVMBuildTrunc(ctx->builder, i32_result, ctx->i16_type, "");
                    } else {
                        dyn_result = i32_result;
                    }
                } else {
                    LLVMValueRef args[] = { this_ptr, hash_val, type_ptr };
                    dyn_result = LLVMBuildCall2(ctx->builder,
                        LLVMGlobalGetValueType(ctx->rt_dyn_getp),
                        ctx->rt_dyn_getp, args, 3, "");
                }
                break;
            }
            }
            LLVMBuildBr(ctx->builder, merge_bb);
            LLVMBasicBlockRef null_vfield_end = LLVMGetInsertBlock(ctx->builder);

            LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);
            LLVMValueRef phi = LLVMBuildPhi(ctx->builder, field_type, "this_field_result");
            LLVMValueRef incoming_vals[] = { direct_val, dyn_result };
            LLVMBasicBlockRef incoming_bbs[] = { has_vfield_end, null_vfield_end };
            LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);

            llvm_store_vreg(ctx, f, dst, phi);
        } else {
            LLVMValueRef val = LLVMBuildLoad2(ctx->builder, field_type, this_ptr, "");
            llvm_store_vreg(ctx, f, dst, val);
        }
        break;
    }

    case OSetThis: {
        /* this.field[idx] = src (this is R(0)) */
        int field_idx = op->p1;
        int src = op->p2;
        hl_type *this_type = f->regs[0];
        hl_type *src_type = f->regs[src];

        LLVMValueRef this_ptr = llvm_load_vreg(ctx, f, 0);
        LLVMValueRef val = llvm_load_vreg(ctx, f, src);

        if (this_type->kind == HOBJ || this_type->kind == HSTRUCT) {
            int offset = 0;
            hl_runtime_obj *rt = hl_get_obj_rt(this_type);
            if (rt && field_idx < rt->nfields) {
                offset = rt->fields_indexes[field_idx];
            }
            LLVMValueRef offset_val = LLVMConstInt(ctx->i64_type, offset, false);
            LLVMValueRef field_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
                this_ptr, &offset_val, 1, "");
            LLVMBuildStore(ctx->builder, val, field_ptr);
        } else if (this_type->kind == HVIRTUAL && this_type->virt && field_idx < this_type->virt->nfields) {
            /* HVIRTUAL field set - same as OSetField HVIRTUAL case */
            int vfield_offset = 24 + field_idx * 8;
            LLVMValueRef vfield_off_val = LLVMConstInt(ctx->i64_type, vfield_offset, false);
            LLVMValueRef vfield_ptr_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
                this_ptr, &vfield_off_val, 1, "");
            LLVMValueRef vfield_ptr = LLVMBuildLoad2(ctx->builder, ctx->ptr_type, vfield_ptr_ptr, "vfield");

            LLVMValueRef is_null = LLVMBuildICmp(ctx->builder, LLVMIntEQ, vfield_ptr,
                LLVMConstNull(ctx->ptr_type), "");

            LLVMBasicBlockRef has_vfield_bb = LLVMAppendBasicBlock(ctx->current_function, "this_has_vfield_set");
            LLVMBasicBlockRef null_vfield_bb = LLVMAppendBasicBlock(ctx->current_function, "this_null_vfield_set");
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlock(ctx->current_function, "this_setfield_merge");

            LLVMBuildCondBr(ctx->builder, is_null, null_vfield_bb, has_vfield_bb);

            /* has_vfield path */
            LLVMPositionBuilderAtEnd(ctx->builder, has_vfield_bb);
            LLVMBuildStore(ctx->builder, val, vfield_ptr);
            LLVMBuildBr(ctx->builder, merge_bb);

            /* null_vfield path */
            LLVMPositionBuilderAtEnd(ctx->builder, null_vfield_bb);
            int hashed_name = this_type->virt->fields[field_idx].hashed_name;
            LLVMValueRef hash_val = LLVMConstInt(ctx->i32_type, hashed_name, false);

            switch (src_type->kind) {
            case HF32: {
                LLVMValueRef args[] = { this_ptr, hash_val, val };
                LLVMBuildCall2(ctx->builder,
                    LLVMGlobalGetValueType(ctx->rt_dyn_setf),
                    ctx->rt_dyn_setf, args, 3, "");
                break;
            }
            case HF64: {
                LLVMValueRef args[] = { this_ptr, hash_val, val };
                LLVMBuildCall2(ctx->builder,
                    LLVMGlobalGetValueType(ctx->rt_dyn_setd),
                    ctx->rt_dyn_setd, args, 3, "");
                break;
            }
            case HI64: {
                LLVMValueRef args[] = { this_ptr, hash_val, val };
                LLVMBuildCall2(ctx->builder,
                    LLVMGlobalGetValueType(ctx->rt_dyn_seti64),
                    ctx->rt_dyn_seti64, args, 3, "");
                break;
            }
            default: {
                int type_idx = -1;
                for (int i = 0; i < ctx->code->ntypes; i++) {
                    if (ctx->code->types + i == src_type) {
                        type_idx = i;
                        break;
                    }
                }
                LLVMValueRef type_ptr = type_idx >= 0 ? llvm_get_type_ptr(ctx, type_idx)
                    : LLVMConstNull(ctx->ptr_type);
                if (src_type->kind == HI32 || src_type->kind == HUI8 ||
                    src_type->kind == HUI16 || src_type->kind == HBOOL) {
                    LLVMValueRef i32_val = val;
                    if (src_type->kind == HUI8 || src_type->kind == HBOOL) {
                        i32_val = LLVMBuildZExt(ctx->builder, val, ctx->i32_type, "");
                    } else if (src_type->kind == HUI16) {
                        i32_val = LLVMBuildZExt(ctx->builder, val, ctx->i32_type, "");
                    }
                    LLVMValueRef args[] = { this_ptr, hash_val, type_ptr, i32_val };
                    LLVMBuildCall2(ctx->builder,
                        LLVMGlobalGetValueType(ctx->rt_dyn_seti),
                        ctx->rt_dyn_seti, args, 4, "");
                } else {
                    LLVMValueRef args[] = { this_ptr, hash_val, type_ptr, val };
                    LLVMBuildCall2(ctx->builder,
                        LLVMGlobalGetValueType(ctx->rt_dyn_setp),
                        ctx->rt_dyn_setp, args, 4, "");
                }
                break;
            }
            }
            LLVMBuildBr(ctx->builder, merge_bb);

            LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);
        } else {
            LLVMBuildStore(ctx->builder, val, this_ptr);
        }
        break;
    }

    case OGetI8: {
        /* dst = bytes[offset] as i8 */
        int dst = op->p1;
        int bytes = op->p2;
        int offset = op->p3;
        LLVMValueRef base = llvm_load_vreg(ctx, f, bytes);
        LLVMValueRef off = llvm_load_vreg(ctx, f, offset);
        LLVMValueRef ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type, base, &off, 1, "");
        LLVMValueRef val = LLVMBuildLoad2(ctx->builder, ctx->i8_type, ptr, "");
        /* Zero-extend to i32 */
        LLVMValueRef ext = LLVMBuildZExt(ctx->builder, val, ctx->i32_type, "");
        llvm_store_vreg(ctx, f, dst, ext);
        break;
    }

    case OGetI16: {
        /* dst = bytes[offset] as i16 */
        int dst = op->p1;
        int bytes = op->p2;
        int offset = op->p3;
        LLVMValueRef base = llvm_load_vreg(ctx, f, bytes);
        LLVMValueRef off = llvm_load_vreg(ctx, f, offset);
        LLVMValueRef ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type, base, &off, 1, "");
        LLVMValueRef val = LLVMBuildLoad2(ctx->builder, ctx->i16_type, ptr, "");
        /* Zero-extend to i32 */
        LLVMValueRef ext = LLVMBuildZExt(ctx->builder, val, ctx->i32_type, "");
        llvm_store_vreg(ctx, f, dst, ext);
        break;
    }

    case OGetMem: {
        /* dst = *(type*)(bytes + offset) */
        int dst = op->p1;
        int bytes = op->p2;
        int offset = op->p3;
        hl_type *dst_type = f->regs[dst];
        LLVMTypeRef val_type = llvm_get_type(ctx, dst_type);
        LLVMValueRef base = llvm_load_vreg(ctx, f, bytes);
        LLVMValueRef off = llvm_load_vreg(ctx, f, offset);
        LLVMValueRef ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type, base, &off, 1, "");
        LLVMValueRef val = LLVMBuildLoad2(ctx->builder, val_type, ptr, "");
        llvm_store_vreg(ctx, f, dst, val);
        break;
    }

    case OGetArray: {
        /*
         * dst = array[index]
         *
         * For varray (HARRAY): data starts after sizeof(varray) header
         * For CArray (HABSTRACT): no header, data starts at offset 0
         *   - For HOBJ/HSTRUCT elements: return address (LEA), don't load
         *   - For other types: load value as pointer
         */
        int dst = op->p1;
        int arr = op->p2;
        int idx = op->p3;
        hl_type *arr_type = f->regs[arr];
        hl_type *dst_type = f->regs[dst];

        /* Check if this is a CArray (HABSTRACT) */
        bool is_carray = (arr_type->kind == HABSTRACT);
        bool is_lea = is_carray && (dst_type->kind == HOBJ || dst_type->kind == HSTRUCT);

        /* Determine element type and size */
        hl_type *elem_type;
        int elem_size;
        if (is_carray) {
            elem_type = dst_type;
            if (is_lea) {
                /* For HOBJ/HSTRUCT in CArray, element size is the runtime object size */
                hl_runtime_obj *rt = hl_get_obj_rt(dst_type);
                elem_size = rt ? rt->size : sizeof(void*);
            } else {
                /* For other types in CArray, element size is pointer size */
                elem_size = sizeof(void*);
            }
        } else {
            /* Regular array - use destination type for element size/type.
             * The JIT always uses dst->t for non-CArray (jit_aarch64.c:2587).
             * This is important because the array's tparam may differ from
             * the destination type (e.g., array stores i32 but dst is dyn). */
            elem_type = dst_type;
            elem_size = llvm_type_size(ctx, elem_type);
        }

        LLVMTypeRef val_type = llvm_get_type(ctx, elem_type);
        LLVMValueRef arr_ptr = llvm_load_vreg(ctx, f, arr);
        LLVMValueRef index = llvm_load_vreg(ctx, f, idx);

        /* Calculate data start pointer */
        LLVMValueRef data_ptr;
        if (is_carray) {
            /* CArray: no header, data starts at offset 0 */
            data_ptr = arr_ptr;
        } else {
            /* varray: data starts after sizeof(varray) header */
            LLVMValueRef data_offset = LLVMConstInt(ctx->i64_type, sizeof(varray), false);
            data_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
                arr_ptr, &data_offset, 1, "");
        }

        /* Calculate element offset */
        LLVMValueRef elem_size_val = LLVMConstInt(ctx->i32_type, elem_size, false);
        LLVMValueRef byte_offset = LLVMBuildMul(ctx->builder, index, elem_size_val, "");
        LLVMValueRef byte_offset64 = LLVMBuildZExt(ctx->builder, byte_offset, ctx->i64_type, "");
        LLVMValueRef elem_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
            data_ptr, &byte_offset64, 1, "");

        if (is_lea) {
            /* LEA: return the element address, don't load */
            llvm_store_vreg(ctx, f, dst, elem_ptr);
        } else {
            /* Load the value */
            LLVMValueRef val = LLVMBuildLoad2(ctx->builder, val_type, elem_ptr, "");
            llvm_store_vreg(ctx, f, dst, val);
        }
        break;
    }

    case OSetI8: {
        /* bytes[offset] = val as i8 */
        int bytes = op->p1;
        int offset = op->p2;
        int src = op->p3;
        LLVMValueRef base = llvm_load_vreg(ctx, f, bytes);
        LLVMValueRef off = llvm_load_vreg(ctx, f, offset);
        LLVMValueRef val = llvm_load_vreg(ctx, f, src);
        LLVMValueRef ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type, base, &off, 1, "");
        /* Truncate to i8 */
        LLVMValueRef trunc = LLVMBuildTrunc(ctx->builder, val, ctx->i8_type, "");
        LLVMBuildStore(ctx->builder, trunc, ptr);
        break;
    }

    case OSetI16: {
        /* bytes[offset] = val as i16 */
        int bytes = op->p1;
        int offset = op->p2;
        int src = op->p3;
        LLVMValueRef base = llvm_load_vreg(ctx, f, bytes);
        LLVMValueRef off = llvm_load_vreg(ctx, f, offset);
        LLVMValueRef val = llvm_load_vreg(ctx, f, src);
        LLVMValueRef ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type, base, &off, 1, "");
        /* Truncate to i16 */
        LLVMValueRef trunc = LLVMBuildTrunc(ctx->builder, val, ctx->i16_type, "");
        LLVMBuildStore(ctx->builder, trunc, ptr);
        break;
    }

    case OSetMem: {
        /* *(type*)(bytes + offset) = val */
        int bytes = op->p1;
        int offset = op->p2;
        int src = op->p3;
        LLVMValueRef base = llvm_load_vreg(ctx, f, bytes);
        LLVMValueRef off = llvm_load_vreg(ctx, f, offset);
        LLVMValueRef val = llvm_load_vreg(ctx, f, src);
        LLVMValueRef ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type, base, &off, 1, "");
        LLVMBuildStore(ctx->builder, val, ptr);
        break;
    }

    case OSetArray: {
        /*
         * array[index] = val
         *
         * For varray (HARRAY): data starts after sizeof(varray) header
         * For CArray (HABSTRACT): no header, data starts at offset 0
         *   - For HOBJ/HSTRUCT elements: memcpy from source pointer to destination
         *   - For other types: store value
         */
        int arr = op->p1;
        int idx = op->p2;
        int src = op->p3;
        hl_type *arr_type = f->regs[arr];
        hl_type *src_type = f->regs[src];

        /* Check if this is a CArray (HABSTRACT) */
        bool is_carray = (arr_type->kind == HABSTRACT);
        bool is_struct_copy = is_carray && (src_type->kind == HOBJ || src_type->kind == HSTRUCT);

        /* Determine element size */
        int elem_size;
        if (is_carray) {
            if (is_struct_copy) {
                /* For HOBJ/HSTRUCT in CArray, element size is the runtime object size */
                hl_runtime_obj *rt = hl_get_obj_rt(src_type);
                elem_size = rt ? rt->size : sizeof(void*);
            } else {
                /* For other types in CArray, element size is pointer size */
                elem_size = sizeof(void*);
            }
        } else {
            /* Regular array - use source type for element size.
             * The JIT always uses value->t for non-CArray (jit_aarch64.c:2707). */
            elem_size = llvm_type_size(ctx, src_type);
        }

        LLVMValueRef arr_ptr = llvm_load_vreg(ctx, f, arr);
        LLVMValueRef index = llvm_load_vreg(ctx, f, idx);
        LLVMValueRef val = llvm_load_vreg(ctx, f, src);

        /* Calculate data start pointer */
        LLVMValueRef data_ptr;
        if (is_carray) {
            /* CArray: no header, data starts at offset 0 */
            data_ptr = arr_ptr;
        } else {
            /* varray: data starts after sizeof(varray) header */
            LLVMValueRef data_offset = LLVMConstInt(ctx->i64_type, sizeof(varray), false);
            data_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
                arr_ptr, &data_offset, 1, "");
        }

        LLVMValueRef elem_size_val = LLVMConstInt(ctx->i32_type, elem_size, false);
        LLVMValueRef byte_offset = LLVMBuildMul(ctx->builder, index, elem_size_val, "");
        LLVMValueRef byte_offset64 = LLVMBuildZExt(ctx->builder, byte_offset, ctx->i64_type, "");
        LLVMValueRef elem_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
            data_ptr, &byte_offset64, 1, "");

        if (is_struct_copy) {
            /* Struct copy: val is a pointer to source struct, elem_ptr is destination */
            LLVMValueRef size_val = LLVMConstInt(ctx->i64_type, elem_size, false);
            LLVMBuildMemCpy(ctx->builder, elem_ptr, 0, val, 0, size_val);
        } else {
            LLVMBuildStore(ctx->builder, val, elem_ptr);
        }
        break;
    }

    default:
        break;
    }
}
