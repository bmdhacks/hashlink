/*
 * Copyright (C)2005-2016 Haxe Foundation
 * LLVM Backend - Closure Opcodes
 */
#include "llvm_codegen.h"

void llvm_emit_closures(llvm_ctx *ctx, hl_function *f, hl_opcode *op, int op_idx) {
    switch (op->op) {
    case OStaticClosure: {
        /* dst = closure for function findex (no captured value) */
        int dst = op->p1;
        int findex = op->p2;

        /* Get function pointer */
        LLVMValueRef func = llvm_get_function_ptr(ctx, findex);

        /* Get type pointer from destination register - this is the closure's type
         * as determined by the Haxe compiler, not the function's internal type */
        hl_type *closure_type = f->regs[dst];
        int type_idx = -1;
        for (int i = 0; i < ctx->code->ntypes; i++) {
            if (ctx->code->types + i == closure_type) {
                type_idx = i;
                break;
            }
        }
        LLVMValueRef type_ptr = type_idx >= 0 ? llvm_get_type_ptr(ctx, type_idx) : LLVMConstNull(ctx->ptr_type);

        /* Call hl_alloc_closure_void(type, fun) */
        LLVMValueRef args[] = { type_ptr, func };
        LLVMValueRef result = LLVMBuildCall2(ctx->builder,
            LLVMGlobalGetValueType(ctx->rt_alloc_closure_void),
            ctx->rt_alloc_closure_void, args, 2, "");
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case OInstanceClosure: {
        /* dst = closure for function findex with captured object */
        int dst = op->p1;
        int findex = op->p2;
        int obj = op->p3;

        /* Get function pointer */
        LLVMValueRef func = llvm_get_function_ptr(ctx, findex);

        /* Get captured object */
        LLVMValueRef obj_val = llvm_load_vreg(ctx, f, obj);

        /* Get the FUNCTION's type (not the closure/destination type).
         * hl_alloc_closure_ptr needs the full function type to create closure type.
         * Find the function by findex to get its type. */
        hl_type *fun_type = NULL;
        for (int i = 0; i < ctx->code->nfunctions; i++) {
            if (ctx->code->functions[i].findex == findex) {
                fun_type = ctx->code->functions[i].type;
                break;
            }
        }
        if (!fun_type) {
            /* Check natives if not found in functions */
            for (int i = 0; i < ctx->code->nnatives; i++) {
                if (ctx->code->natives[i].findex == findex) {
                    fun_type = ctx->code->natives[i].t;
                    break;
                }
            }
        }

        LLVMValueRef type_ptr;
        if (fun_type) {
            int type_idx = -1;
            for (int i = 0; i < ctx->code->ntypes; i++) {
                if (ctx->code->types + i == fun_type) {
                    type_idx = i;
                    break;
                }
            }
            type_ptr = (type_idx >= 0) ? llvm_get_type_ptr(ctx, type_idx) : LLVMConstNull(ctx->ptr_type);
        } else {
            type_ptr = LLVMConstNull(ctx->ptr_type);
        }

        /* Call hl_alloc_closure_ptr(type, fun, obj) */
        LLVMValueRef args[] = { type_ptr, func, obj_val };
        LLVMValueRef result = LLVMBuildCall2(ctx->builder,
            LLVMGlobalGetValueType(ctx->rt_alloc_closure_ptr),
            ctx->rt_alloc_closure_ptr, args, 3, "");
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case OVirtualClosure: {
        /* dst = virtual method closure with captured object */
        int dst = op->p1;
        int obj = op->p2;
        int method_idx = op->p3;

        LLVMValueRef obj_val = llvm_load_vreg(ctx, f, obj);
        hl_type *obj_type = f->regs[obj];

        /* Load type pointer from object */
        LLVMValueRef type_ptr = LLVMBuildLoad2(ctx->builder, ctx->ptr_type, obj_val, "type");

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

        /* Find the original function type by walking prototype chain.
         * hl_alloc_closure_ptr needs the FULL function type (not the closure type).
         * Same logic as JIT: find proto entry where pindex == method_idx */
        hl_type *fun_type = NULL;
        hl_type *ot = obj_type;
        while (fun_type == NULL && ot != NULL && ot->kind == HOBJ && ot->obj) {
            for (int i = 0; i < ot->obj->nproto; i++) {
                hl_obj_proto *pp = &ot->obj->proto[i];
                if (pp->pindex == method_idx) {
                    int findex = pp->findex;
                    /* Search for function by findex (it's a global ID, not array index) */
                    for (int j = 0; j < ctx->code->nfunctions; j++) {
                        if (ctx->code->functions[j].findex == findex) {
                            fun_type = ctx->code->functions[j].type;
                            break;
                        }
                    }
                    break;
                }
            }
            ot = ot->obj->super;
        }

        /* Get type pointer for the function type */
        LLVMValueRef fun_type_ptr;
        if (fun_type) {
            int type_idx = -1;
            for (int i = 0; i < ctx->code->ntypes; i++) {
                if (ctx->code->types + i == fun_type) {
                    type_idx = i;
                    break;
                }
            }
            fun_type_ptr = (type_idx >= 0) ? llvm_get_type_ptr(ctx, type_idx) : type_ptr;
        } else {
            /* Fallback to object's type if we couldn't find function type */
            fun_type_ptr = type_ptr;
        }

        /* Call hl_alloc_closure_ptr(type, fun, obj) */
        LLVMValueRef args[] = { fun_type_ptr, fptr, obj_val };
        LLVMValueRef result = LLVMBuildCall2(ctx->builder,
            LLVMGlobalGetValueType(ctx->rt_alloc_closure_ptr),
            ctx->rt_alloc_closure_ptr, args, 3, "");
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    default:
        break;
    }
}
