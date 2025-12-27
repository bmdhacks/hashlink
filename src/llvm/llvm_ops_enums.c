/*
 * Copyright (C)2005-2016 Haxe Foundation
 * LLVM Backend - Enum Opcodes
 */
#include "llvm_codegen.h"

void llvm_emit_enums(llvm_ctx *ctx, hl_function *f, hl_opcode *op, int op_idx) {
    switch (op->op) {
    case OMakeEnum: {
        /* dst = make enum with constructor and args
         * Use pre-computed offsets from c->offsets[i] like the JIT does */
        int dst = op->p1;
        int construct_idx = op->p2;
        int nargs = op->p3;
        hl_type *enum_type = f->regs[dst];

        /* Get type pointer */
        int type_idx = -1;
        for (int i = 0; i < ctx->code->ntypes; i++) {
            if (ctx->code->types + i == enum_type) {
                type_idx = i;
                break;
            }
        }
        LLVMValueRef type_ptr = type_idx >= 0 ? llvm_get_type_ptr(ctx, type_idx)
            : LLVMConstNull(ctx->ptr_type);

        /* Call hl_alloc_enum */
        LLVMValueRef alloc_args[] = { type_ptr, LLVMConstInt(ctx->i32_type, construct_idx, false) };
        LLVMValueRef enum_obj = LLVMBuildCall2(ctx->builder,
            LLVMGlobalGetValueType(ctx->rt_alloc_enum),
            ctx->rt_alloc_enum, alloc_args, 2, "");

        /* Set constructor args using pre-computed offsets */
        if (enum_type->tenum && construct_idx < enum_type->tenum->nconstructs) {
            hl_enum_construct *c = &enum_type->tenum->constructs[construct_idx];
            for (int i = 0; i < nargs && i < c->nparams; i++) {
                int offset = c->offsets[i];
                LLVMValueRef arg = llvm_load_vreg(ctx, f, op->extra[i]);
                LLVMValueRef off_val = LLVMConstInt(ctx->i64_type, offset, false);
                LLVMValueRef field_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
                    enum_obj, &off_val, 1, "");
                LLVMBuildStore(ctx->builder, arg, field_ptr);
            }
        }

        llvm_store_vreg(ctx, f, dst, enum_obj);
        break;
    }

    case OEnumAlloc: {
        /* dst = allocate enum with constructor index */
        int dst = op->p1;
        int construct_idx = op->p2;
        hl_type *enum_type = f->regs[dst];

        /* Get type pointer */
        int type_idx = -1;
        for (int i = 0; i < ctx->code->ntypes; i++) {
            if (ctx->code->types + i == enum_type) {
                type_idx = i;
                break;
            }
        }
        LLVMValueRef type_ptr = type_idx >= 0 ? llvm_get_type_ptr(ctx, type_idx)
            : LLVMConstNull(ctx->ptr_type);

        /* Call hl_alloc_enum */
        LLVMValueRef args[] = { type_ptr, LLVMConstInt(ctx->i32_type, construct_idx, false) };
        LLVMValueRef result = LLVMBuildCall2(ctx->builder,
            LLVMGlobalGetValueType(ctx->rt_alloc_enum),
            ctx->rt_alloc_enum, args, 2, "");
        llvm_store_vreg(ctx, f, dst, result);
        break;
    }

    case OEnumIndex: {
        /* dst = enum.index */
        int dst = op->p1;
        int src = op->p2;
        LLVMValueRef enum_obj = llvm_load_vreg(ctx, f, src);

        /* venum layout: hl_type* t (8 bytes), int index (4 bytes) */
        LLVMValueRef idx_offset = LLVMConstInt(ctx->i64_type, 8, false);
        LLVMValueRef idx_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
            enum_obj, &idx_offset, 1, "");
        LLVMValueRef idx = LLVMBuildLoad2(ctx->builder, ctx->i32_type, idx_ptr, "");
        llvm_store_vreg(ctx, f, dst, idx);
        break;
    }

    case OEnumField: {
        /* dst = enum.field[field_idx] for constructor construct_idx
         * Use pre-computed offsets from c->offsets[field_idx] like the JIT does */
        /* NOTE: extra is used as a direct integer value, not an array pointer */
        int dst = op->p1;
        int src = op->p2;
        int construct_idx = op->p3;
        int field_idx = (int)(int_val)op->extra;
        hl_type *enum_type = f->regs[src];
        hl_type *dst_type = f->regs[dst];

        LLVMValueRef enum_obj = llvm_load_vreg(ctx, f, src);
        LLVMTypeRef field_llvm_type = llvm_get_type(ctx, dst_type);

        /* Get field offset from constructor's pre-computed offsets array */
        int offset = 0;
        if (enum_type->tenum && construct_idx < enum_type->tenum->nconstructs) {
            hl_enum_construct *c = &enum_type->tenum->constructs[construct_idx];
            if (field_idx < c->nparams) {
                offset = c->offsets[field_idx];
            }
        }

        LLVMValueRef off_val = LLVMConstInt(ctx->i64_type, offset, false);
        LLVMValueRef field_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
            enum_obj, &off_val, 1, "");
        LLVMValueRef val = LLVMBuildLoad2(ctx->builder, field_llvm_type, field_ptr, "");
        llvm_store_vreg(ctx, f, dst, val);
        break;
    }

    case OSetEnumField: {
        /* enum.field[field_idx] = val
         * Use pre-computed offsets from c->offsets[field_idx] like the JIT does */
        int enum_reg = op->p1;
        int field_idx = op->p2;
        int src = op->p3;
        hl_type *enum_type = f->regs[enum_reg];

        LLVMValueRef enum_obj = llvm_load_vreg(ctx, f, enum_reg);
        LLVMValueRef val = llvm_load_vreg(ctx, f, src);

        /* Get field offset from constructor 0's pre-computed offsets array */
        int offset = 0;
        if (enum_type->tenum && enum_type->tenum->nconstructs > 0) {
            hl_enum_construct *c = &enum_type->tenum->constructs[0];
            if (field_idx < c->nparams) {
                offset = c->offsets[field_idx];
            }
        }

        LLVMValueRef off_val = LLVMConstInt(ctx->i64_type, offset, false);
        LLVMValueRef field_ptr = LLVMBuildGEP2(ctx->builder, ctx->i8_type,
            enum_obj, &off_val, 1, "");
        LLVMBuildStore(ctx->builder, val, field_ptr);
        break;
    }

    default:
        break;
    }
}
