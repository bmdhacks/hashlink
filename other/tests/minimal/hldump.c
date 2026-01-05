/*
 * Simple HashLink bytecode dumper
 * Dumps functions and opcodes from a .hl file
 */
#include <hl.h>
#include <hlmodule.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Opcode names from opcodes.h */
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

static const char *type_kind_name(hl_type_kind k) {
    switch (k) {
        case HVOID: return "void";
        case HUI8: return "u8";
        case HUI16: return "u16";
        case HI32: return "i32";
        case HI64: return "i64";
        case HF32: return "f32";
        case HF64: return "f64";
        case HBOOL: return "bool";
        case HBYTES: return "bytes";
        case HDYN: return "dyn";
        case HFUN: return "fun";
        case HOBJ: return "obj";
        case HARRAY: return "array";
        case HTYPE: return "type";
        case HREF: return "ref";
        case HVIRTUAL: return "virtual";
        case HDYNOBJ: return "dynobj";
        case HABSTRACT: return "abstract";
        case HENUM: return "enum";
        case HNULL: return "null";
        case HMETHOD: return "method";
        case HSTRUCT: return "struct";
        case HPACKED: return "packed";
        default: return "???";
    }
}

static int print_depth = 0;

/* Print uchar string (16-bit) as ASCII */
static void print_ustr(const uchar *s) {
    if (!s) return;
    while (*s) {
        putchar((char)*s);
        s++;
    }
}

static void print_type(hl_type *t) {
    if (!t) {
        printf("null");
        return;
    }
    if (print_depth > 3) {
        printf("%s(...)", type_kind_name(t->kind));
        return;
    }
    print_depth++;
    printf("%s", type_kind_name(t->kind));
    if ((t->kind == HOBJ || t->kind == HSTRUCT) && t->obj) {
        printf("(");
        if (t->obj->name) print_ustr(t->obj->name);
        printf(", %d fields", t->obj->nfields);
        if (t->obj->super) {
            printf(", super=");
            print_type(t->obj->super);
        }
        printf(")");
    } else if (t->kind == HVIRTUAL && t->virt) {
        printf("(%d fields: ", t->virt->nfields);
        for (int i = 0; i < t->virt->nfields && i < 4; i++) {
            if (i > 0) printf(", ");
            print_type(t->virt->fields[i].t);
        }
        if (t->virt->nfields > 4) printf(", ...");
        printf(")");
    } else if ((t->kind == HFUN || t->kind == HMETHOD) && t->fun) {
        printf("(");
        for (int i = 0; i < t->fun->nargs; i++) {
            if (i > 0) printf(",");
            print_type(t->fun->args[i]);
        }
        printf(")->");
        print_type(t->fun->ret);
    } else if (t->kind == HENUM && t->tenum) {
        printf("(");
        if (t->tenum->name) print_ustr(t->tenum->name);
        printf(", %d constructs)", t->tenum->nconstructs);
    } else if (t->kind == HNULL && t->tparam) {
        printf("(");
        print_type(t->tparam);
        printf(")");
    }
    print_depth--;
}

/* Print full type details with all indices */
static void print_type_full(hl_type *t, int indent) {
    if (!t) {
        printf("null\n");
        return;
    }
    printf("%s", type_kind_name(t->kind));

    if ((t->kind == HOBJ || t->kind == HSTRUCT) && t->obj) {
        printf("(");
        if (t->obj->name) print_ustr(t->obj->name);  /* Fully qualified: package.ClassName */
        printf(", %d fields, %d protos)\n", t->obj->nfields, t->obj->nproto);
        /* Print fields */
        for (int i = 0; i < t->obj->nfields; i++) {
            printf("%*s  F%d: ", indent, "", i);
            print_ustr(t->obj->fields[i].name);
            printf(" : ");
            print_type(t->obj->fields[i].t);
            printf("\n");
        }
        /* Print proto methods */
        for (int i = 0; i < t->obj->nproto; i++) {
            printf("%*s  P%d: ", indent, "", i);
            print_ustr(t->obj->proto[i].name);
            printf(" -> F%d\n", t->obj->proto[i].findex);
        }
        if (t->obj->super) {
            printf("%*s  super: ", indent, "");
            print_type(t->obj->super);
            printf("\n");
        }
    } else if (t->kind == HVIRTUAL && t->virt) {
        printf("(%d vfields)\n", t->virt->nfields);
        for (int i = 0; i < t->virt->nfields; i++) {
            printf("%*s  V%d: ", indent, "", i);
            print_ustr(t->virt->fields[i].name);
            printf(" : ");
            print_type(t->virt->fields[i].t);
            printf("\n");
        }
    } else if ((t->kind == HFUN || t->kind == HMETHOD) && t->fun) {
        printf("(");
        for (int i = 0; i < t->fun->nargs; i++) {
            if (i > 0) printf(", ");
            print_type(t->fun->args[i]);
        }
        printf(") -> ");
        print_type(t->fun->ret);
        printf("\n");
    } else if (t->kind == HENUM && t->tenum) {
        printf("(");
        if (t->tenum->name) print_ustr(t->tenum->name);  /* Fully qualified: package.EnumName */
        printf(", %d constructs)\n", t->tenum->nconstructs);
        for (int ci = 0; ci < t->tenum->nconstructs; ci++) {
            hl_enum_construct *c = &t->tenum->constructs[ci];
            printf("%*s  C%d: ", indent, "", ci);
            print_ustr(c->name);
            printf("(");
            for (int pi = 0; pi < c->nparams; pi++) {
                if (pi > 0) printf(", ");
                printf("P%d:", pi);
                print_type(c->params[pi]);
            }
            printf(") size=%d\n", c->size);
        }
    } else if (t->kind == HNULL && t->tparam) {
        printf("(");
        print_type(t->tparam);
        printf(")\n");
    } else if (t->kind == HABSTRACT && t->abs_name) {
        printf("(");
        print_ustr(t->abs_name);
        printf(")\n");
    } else if (t->kind == HREF && t->tparam) {
        printf("(");
        print_type(t->tparam);
        printf(")\n");
    } else if (t->kind == HARRAY) {
        printf("\n");
    } else {
        printf("\n");
    }
}

/* Find function name by searching type protos (reverse lookup) */
static void find_function_name(hl_code *c, int findex, const uchar **out_class, const uchar **out_method) {
    *out_class = NULL;
    *out_method = NULL;
    for (int i = 0; i < c->ntypes; i++) {
        hl_type *t = &c->types[i];
        if ((t->kind == HOBJ || t->kind == HSTRUCT) && t->obj) {
            for (int j = 0; j < t->obj->nproto; j++) {
                if (t->obj->proto[j].findex == findex) {
                    *out_class = t->obj->name;
                    *out_method = t->obj->proto[j].name;
                    return;
                }
            }
        }
    }
}

/* Find type index in code->types array */
static int find_type_index(hl_code *c, hl_type *t) {
    if (!t) return -1;
    for (int i = 0; i < c->ntypes; i++) {
        if (&c->types[i] == t) return i;
    }
    return -1;
}

/* Count total fields including inherited ones */
static int count_total_fields(hl_type *t) {
    if (!t || (t->kind != HOBJ && t->kind != HSTRUCT) || !t->obj)
        return 0;
    int count = t->obj->nfields;
    if (t->obj->super)
        count += count_total_fields(t->obj->super);
    return count;
}

/* Find field by runtime index (accounting for inheritance)
 * Returns the field info and sets *defining_type to the type that defines the field */
static hl_obj_field *find_field_by_runtime_index(hl_type *t, int runtime_idx, hl_type **defining_type) {
    if (!t || (t->kind != HOBJ && t->kind != HSTRUCT) || !t->obj) {
        if (defining_type) *defining_type = NULL;
        return NULL;
    }

    /* First count inherited fields */
    int inherited = 0;
    if (t->obj->super)
        inherited = count_total_fields(t->obj->super);

    if (runtime_idx < inherited) {
        /* Field is in a parent class */
        return find_field_by_runtime_index(t->obj->super, runtime_idx, defining_type);
    } else {
        /* Field is in this class */
        int local_idx = runtime_idx - inherited;
        if (local_idx < t->obj->nfields) {
            if (defining_type) *defining_type = t;
            return &t->obj->fields[local_idx];
        }
        if (defining_type) *defining_type = NULL;
        return NULL;
    }
}

/* Print type with T index prefix */
static void print_type_with_index(hl_code *c, hl_type *t) {
    int idx = find_type_index(c, t);
    if (idx >= 0) {
        printf("T%d ", idx);
    }
    print_type(t);
}

/* Print function name by index - always shows F<index>, plus name if found */
static void print_func_name(hl_code *c, int findex) {
    printf("F%d", findex);
    /* Check if it's a native */
    for (int i = 0; i < c->nnatives; i++) {
        if (c->natives[i].findex == findex) {
            printf(" %s@%s", c->natives[i].name, c->natives[i].lib);
            return;
        }
    }
    /* Search type protos */
    const uchar *cls = NULL, *method = NULL;
    find_function_name(c, findex, &cls, &method);
    if (cls && method) {
        printf(" ");
        print_ustr(cls);
        printf("::");
        print_ustr(method);
    }
}

static void dump_function(hl_code *c, hl_function *f, int verbose) {
    /* Get qualified name - try multiple sources */
    hl_type_obj *obj = fun_obj(f);
    const uchar *fname = fun_field_name(f);

    /* If not available via fun_obj/fun_field_name, search type protos */
    const uchar *lookup_class = NULL;
    const uchar *lookup_method = NULL;
    if (!obj && !fname) {
        find_function_name(c, f->findex, &lookup_class, &lookup_method);
    }

    printf("\n=== Function %d ===\n", f->findex);

    /* Show fully qualified name (package.Class::method) if available */
    printf("  Name: ");
    if (obj && obj->name) {
        print_ustr(obj->name);  /* Already includes package path */
        printf("::");
        if (fname) print_ustr(fname);
        printf("\n");
    } else if (fname) {
        print_ustr(fname);
        printf("\n");
    } else if (lookup_class && lookup_method) {
        /* Found via proto lookup */
        print_ustr(lookup_class);
        printf("::");
        print_ustr(lookup_method);
        printf("\n");
    } else {
        /* No name info - try to infer from type or show anonymous */
        if (f->type && (f->type->kind == HFUN || f->type->kind == HMETHOD) && f->type->fun) {
            /* If first arg is an object type, might be a method */
            if (f->type->fun->nargs > 0 && f->type->fun->args[0]) {
                hl_type *first_arg = f->type->fun->args[0];
                if ((first_arg->kind == HOBJ || first_arg->kind == HSTRUCT) && first_arg->obj && first_arg->obj->name) {
                    printf("(method of ");
                    print_ustr(first_arg->obj->name);
                    printf(")\n");
                } else {
                    printf("(anonymous)\n");
                }
            } else {
                printf("(anonymous)\n");
            }
        } else {
            printf("(anonymous)\n");
        }
    }

    printf("  Type: ");
    print_type_with_index(c, f->type);
    printf("\n");

    /* Show source file from debug info */
    if (c->hasdebug && f->debug && f->nops > 0) {
        int file_idx = f->debug[0] & 0x7FFFFFFF;
        int line = f->debug[1];
        if (file_idx < c->ndebugfiles) {
            printf("  Source: %s:%d\n", c->debugfiles[file_idx], line);
        }
    }

    printf("  Registers: %d\n", f->nregs);
    printf("  Opcodes: %d\n", f->nops);

    if (verbose) {
        printf("  Register types:\n");
        for (int i = 0; i < f->nregs; i++) {
            printf("    r%d: ", i);
            print_type_with_index(c, f->regs[i]);
            printf("\n");
        }
    }

    printf("  Code:\n");
    for (int i = 0; i < f->nops; i++) {
        hl_opcode *op = &f->ops[i];
        const char *name = (op->op < sizeof(opcode_names)/sizeof(opcode_names[0]))
                          ? opcode_names[op->op] : "???";
        printf("    %4d: %-16s %d, %d, %d", i, name, op->p1, op->p2, op->p3);

        /* Show extra info for some opcodes */
        switch (op->op) {
            case OMov:
                printf("  ; r%d = r%d", op->p1, op->p2);
                break;
            case ONull:
                printf("  ; r%d = null", op->p1);
                break;
            case OAdd:
                printf("  ; r%d = r%d + r%d", op->p1, op->p2, op->p3);
                break;
            case OSub:
                printf("  ; r%d = r%d - r%d", op->p1, op->p2, op->p3);
                break;
            case OMul:
                printf("  ; r%d = r%d * r%d", op->p1, op->p2, op->p3);
                break;
            case OSDiv:
                printf("  ; r%d = r%d / r%d (signed)", op->p1, op->p2, op->p3);
                break;
            case OUDiv:
                printf("  ; r%d = r%d / r%d (unsigned)", op->p1, op->p2, op->p3);
                break;
            case OSMod:
                printf("  ; r%d = r%d %% r%d (signed)", op->p1, op->p2, op->p3);
                break;
            case OUMod:
                printf("  ; r%d = r%d %% r%d (unsigned)", op->p1, op->p2, op->p3);
                break;
            case ONeg:
                printf("  ; r%d = -r%d", op->p1, op->p2);
                break;
            case ONot:
                printf("  ; r%d = !r%d", op->p1, op->p2);
                break;
            case OIncr:
                printf("  ; r%d++", op->p1);
                break;
            case ODecr:
                printf("  ; r%d--", op->p1);
                break;
            case OShl:
                printf("  ; r%d = r%d << r%d", op->p1, op->p2, op->p3);
                break;
            case OSShr:
                printf("  ; r%d = r%d >> r%d (signed)", op->p1, op->p2, op->p3);
                break;
            case OUShr:
                printf("  ; r%d = r%d >>> r%d (unsigned)", op->p1, op->p2, op->p3);
                break;
            case OAnd:
                printf("  ; r%d = r%d & r%d", op->p1, op->p2, op->p3);
                break;
            case OOr:
                printf("  ; r%d = r%d | r%d", op->p1, op->p2, op->p3);
                break;
            case OXor:
                printf("  ; r%d = r%d ^ r%d", op->p1, op->p2, op->p3);
                break;
            case OLabel:
                printf("  ; label");
                break;
            case OInt:
                if (op->p2 >= 0 && op->p2 < c->nints)
                    printf("  ; r%d = I%d (%d)", op->p1, op->p2, c->ints[op->p2]);
                else
                    printf("  ; r%d = I%d", op->p1, op->p2);
                break;
            case OString:
                if (op->p2 >= 0 && op->p2 < c->nstrings)
                    printf("  ; r%d = S%d \"%s\"", op->p1, op->p2, c->strings[op->p2]);
                else
                    printf("  ; r%d = S%d", op->p1, op->p2);
                break;
            case OBool:
                printf("  ; r%d = %s", op->p1, op->p2 ? "true" : "false");
                break;
            case OCall0:
                printf("  ; call ");
                print_func_name(c, op->p2);
                printf("()");
                break;
            case OCall1:
                printf("  ; call ");
                print_func_name(c, op->p2);
                printf("(r%d)", op->p3);
                break;
            case OCall2:
                /* extra is a direct int cast, not array */
                printf("  ; call ");
                print_func_name(c, op->p2);
                printf("(r%d, r%d)", op->p3, (int)(int_val)op->extra);
                break;
            case OCall3:
                printf("  ; call ");
                print_func_name(c, op->p2);
                printf("(r%d, r%d, r%d)", op->p3, op->extra[0], op->extra[1]);
                break;
            case OCall4:
                printf("  ; call ");
                print_func_name(c, op->p2);
                printf("(r%d, r%d, r%d, r%d)", op->p3, op->extra[0], op->extra[1], op->extra[2]);
                break;
            case OCallN:
                printf("  ; call ");
                print_func_name(c, op->p2);
                break;
            case OCallMethod:
            case OCallThis:
                /* p1=dst, p2=method_idx, p3=nargs, extra[0..nargs-1]=arg regs */
                printf("  ; method[%d] args=[", op->p2);
                for (int j = 0; j < op->p3; j++) {
                    printf("r%d%s", op->extra[j], j < op->p3-1 ? "," : "");
                }
                printf("]");
                break;
            case OCallClosure:
                /* p1=dst, p2=closure_reg, p3=nargs, extra[0..nargs-1]=arg regs */
                printf("  ; call r%d args=[", op->p2);
                for (int j = 0; j < op->p3; j++) {
                    printf("r%d%s", op->extra[j], j < op->p3-1 ? "," : "");
                }
                printf("]");
                break;
            case OJAlways:
                printf("  ; goto %d", (i + 1) + op->p1);
                break;
            case OJTrue:
            case OJFalse:
            case OJNull:
            case OJNotNull:
                printf("  ; if r%d goto %d", op->p1, (i + 1) + op->p2);
                break;
            case OJSLt:
            case OJSGte:
            case OJEq:
            case OJNotEq:
                printf("  ; if r%d,r%d goto %d", op->p1, op->p2, (i + 1) + op->p3);
                break;
            case ORet:
                printf("  ; return r%d", op->p1);
                break;
            case OGetGlobal:
                printf("  ; r%d = G%d", op->p1, op->p2);
                break;
            case OSetGlobal:
                printf("  ; G%d = r%d", op->p2, op->p1);
                break;
            case OField:
                {
                    hl_type *ot = f->regs[op->p2];
                    int tidx = find_type_index(c, ot);
                    printf("  ; r%d = r%d T%d .F%d", op->p1, op->p2, tidx, op->p3);
                    /* Try to show field name (accounting for inheritance) */
                    if (ot && (ot->kind == HOBJ || ot->kind == HSTRUCT) && ot->obj) {
                        hl_type *def_type = NULL;
                        hl_obj_field *fld = find_field_by_runtime_index(ot, op->p3, &def_type);
                        if (fld) {
                            printf(" ");
                            print_ustr(fld->name);
                            if (def_type && def_type != ot && def_type->obj && def_type->obj->name) {
                                printf(" (from ");
                                print_ustr(def_type->obj->name);
                                printf(")");
                            }
                        }
                    } else if (ot && ot->kind == HVIRTUAL && ot->virt && op->p3 < ot->virt->nfields) {
                        printf(" ");
                        print_ustr(ot->virt->fields[op->p3].name);
                    }
                }
                break;
            case OSetField:
                {
                    hl_type *ot = f->regs[op->p1];
                    int tidx = find_type_index(c, ot);
                    printf("  ; r%d T%d .F%d", op->p1, tidx, op->p2);
                    /* Try to show field name (accounting for inheritance) */
                    if (ot && (ot->kind == HOBJ || ot->kind == HSTRUCT) && ot->obj) {
                        hl_type *def_type = NULL;
                        hl_obj_field *fld = find_field_by_runtime_index(ot, op->p2, &def_type);
                        if (fld) {
                            printf(" ");
                            print_ustr(fld->name);
                            if (def_type && def_type != ot && def_type->obj && def_type->obj->name) {
                                printf(" (from ");
                                print_ustr(def_type->obj->name);
                                printf(")");
                            }
                        }
                    } else if (ot && ot->kind == HVIRTUAL && ot->virt && op->p2 < ot->virt->nfields) {
                        printf(" ");
                        print_ustr(ot->virt->fields[op->p2].name);
                    }
                    printf(" = r%d", op->p3);
                }
                break;
            case OGetThis:
                /* p1=dst, p2=field_idx - gets field from r0 (this) */
                {
                    hl_type *ot = f->regs[0];
                    int tidx = find_type_index(c, ot);
                    printf("  ; r%d = r0 T%d .F%d", op->p1, tidx, op->p2);
                    if (ot && (ot->kind == HOBJ || ot->kind == HSTRUCT) && ot->obj) {
                        hl_type *def_type = NULL;
                        hl_obj_field *fld = find_field_by_runtime_index(ot, op->p2, &def_type);
                        if (fld) {
                            printf(" ");
                            print_ustr(fld->name);
                            if (def_type && def_type != ot && def_type->obj && def_type->obj->name) {
                                printf(" (from ");
                                print_ustr(def_type->obj->name);
                                printf(")");
                            }
                        }
                    }
                }
                break;
            case OSetThis:
                /* p1=field_idx, p2=value_reg - sets field on r0 (this) */
                {
                    hl_type *ot = f->regs[0];
                    int tidx = find_type_index(c, ot);
                    printf("  ; r0 T%d .F%d", tidx, op->p1);
                    if (ot && (ot->kind == HOBJ || ot->kind == HSTRUCT) && ot->obj) {
                        hl_type *def_type = NULL;
                        hl_obj_field *fld = find_field_by_runtime_index(ot, op->p1, &def_type);
                        if (fld) {
                            printf(" ");
                            print_ustr(fld->name);
                            if (def_type && def_type != ot && def_type->obj && def_type->obj->name) {
                                printf(" (from ");
                                print_ustr(def_type->obj->name);
                                printf(")");
                            }
                        }
                    }
                    printf(" = r%d", op->p2);
                }
                break;
            case ONew:
                {
                    hl_type *t = f->regs[op->p1];
                    int tidx = find_type_index(c, t);
                    printf("  ; r%d = new T%d ", op->p1, tidx);
                    print_type(t);
                }
                break;
            case OType:
                /* p1=dst, p2=type_index */
                {
                    hl_type *t = (op->p2 >= 0 && op->p2 < c->ntypes) ? &c->types[op->p2] : NULL;
                    printf("  ; r%d = T%d ", op->p1, op->p2);
                    if (t) print_type(t);
                }
                break;
            case OGetType:
                /* p1=dst, p2=src_reg - gets type from object */
                {
                    hl_type *src_t = f->regs[op->p2];
                    int tidx = find_type_index(c, src_t);
                    printf("  ; r%d = typeof(r%d) T%d ", op->p1, op->p2, tidx);
                    print_type(src_t);
                }
                break;
            case OGetTID:
                /* p1=dst, p2=src_reg - gets type ID */
                printf("  ; r%d = typeid(r%d)", op->p1, op->p2);
                break;
            case ORef:
                /* p1=dst, p2=src - create reference */
                {
                    hl_type *dst_t = f->regs[op->p1];
                    int tidx = find_type_index(c, dst_t);
                    printf("  ; r%d = &r%d T%d ", op->p1, op->p2, tidx);
                    print_type(dst_t);
                }
                break;
            case OUnref:
                /* p1=dst, p2=ref - dereference */
                {
                    hl_type *dst_t = f->regs[op->p1];
                    int tidx = find_type_index(c, dst_t);
                    printf("  ; r%d = *r%d T%d ", op->p1, op->p2, tidx);
                    print_type(dst_t);
                }
                break;
            case OSetref:
                /* p1=ref, p2=value */
                printf("  ; *r%d = r%d", op->p1, op->p2);
                break;
            case OGetArray:
                /* p1=dst, p2=array, p3=index */
                printf("  ; r%d = r%d[r%d]", op->p1, op->p2, op->p3);
                break;
            case OSetArray:
                /* p1=array, p2=index, p3=value */
                printf("  ; r%d[r%d] = r%d", op->p1, op->p2, op->p3);
                break;
            case OArraySize:
                /* p1=dst, p2=array */
                printf("  ; r%d = r%d.length", op->p1, op->p2);
                break;
            case OSwitch:
                /* p1=value, p2=ncases, p3=end_offset, extra=case_offsets */
                printf("  ; switch r%d [", op->p1);
                for (int j = 0; j < op->p2 && j < 8; j++) {
                    printf("%d:%d", j, (i + 1) + op->extra[j]);
                    if (j < op->p2 - 1) printf(", ");
                }
                if (op->p2 > 8) printf(", ...");
                printf("] default:%d", (i + 1) + op->p3);
                break;
            case ONullCheck:
                printf("  ; nullcheck r%d", op->p1);
                break;
            case OThrow:
                printf("  ; throw r%d", op->p1);
                break;
            case ORethrow:
                printf("  ; rethrow r%d", op->p1);
                break;
            case OTrap:
                /* p1=dst (exception reg), p2=offset to end of try block */
                printf("  ; try { -> catch at %d }", (i + 1) + op->p2);
                break;
            case OEndTrap:
                /* p1=1 if trap was triggered */
                printf("  ; } endtrap (triggered=%d)", op->p1);
                break;
            case OCatch:
                /* p1=global_idx for typing (doesn't do anything at runtime) */
                printf("  ; catch (G%d for type)", op->p1);
                break;
            case OToDyn:
            case OToVirtual:
            case OSafeCast:
            case OUnsafeCast:
                /* p1=dst, p2=src */
                {
                    hl_type *dst_t = f->regs[op->p1];
                    int tidx = find_type_index(c, dst_t);
                    printf("  ; r%d = (T%d ", op->p1, tidx);
                    print_type(dst_t);
                    printf(") r%d", op->p2);
                }
                break;
            case ODynGet:
                /* p1=dst, p2=obj, p3=string_idx (field name) */
                printf("  ; r%d = r%d.S%d", op->p1, op->p2, op->p3);
                if (op->p3 >= 0 && op->p3 < c->nstrings)
                    printf(" \"%s\"", c->strings[op->p3]);
                break;
            case ODynSet:
                /* p1=obj, p2=string_idx (field name), p3=value */
                printf("  ; r%d.S%d", op->p1, op->p2);
                if (op->p2 >= 0 && op->p2 < c->nstrings)
                    printf(" \"%s\"", c->strings[op->p2]);
                printf(" = r%d", op->p3);
                break;
            case OStaticClosure:
                /* p1=dst, p2=findex */
                printf("  ; r%d = closure(", op->p1);
                print_func_name(c, op->p2);
                printf(")");
                break;
            case OInstanceClosure:
                /* p1=dst, p2=findex, p3=obj_reg */
                printf("  ; r%d = closure(", op->p1);
                print_func_name(c, op->p2);
                printf(", r%d)", op->p3);
                break;
            case OVirtualClosure:
                /* p1=dst, p2=obj_reg, p3=proto_idx */
                printf("  ; r%d = vclosure(r%d, proto[%d])", op->p1, op->p2, op->p3);
                break;
            case OFloat:
                if (op->p2 >= 0 && op->p2 < c->nfloats)
                    printf("  ; r%d = FL%d (%g)", op->p1, op->p2, c->floats[op->p2]);
                else
                    printf("  ; r%d = FL%d", op->p1, op->p2);
                break;
            case OBytes:
                printf("  ; r%d = B%d", op->p1, op->p2);
                break;
            case OMakeEnum:
                /* p1=dst, p2=construct_idx, p3=nargs */
                {
                    hl_type *et = f->regs[op->p1];
                    int tidx = find_type_index(c, et);
                    printf("  ; r%d = T%d ", op->p1, tidx);
                    if (et && et->kind == HENUM && et->tenum && op->p2 < et->tenum->nconstructs) {
                        hl_enum_construct *ec = &et->tenum->constructs[op->p2];
                        print_ustr(et->tenum->name);
                        printf("::");
                        print_ustr(ec->name);
                        printf("[C%d]", op->p2);
                    }
                }
                break;
            case OEnumAlloc:
                /* p1=dst, p2=construct_idx */
                {
                    hl_type *et = f->regs[op->p1];
                    int tidx = find_type_index(c, et);
                    printf("  ; r%d = alloc T%d ", op->p1, tidx);
                    if (et && et->kind == HENUM && et->tenum) {
                        print_ustr(et->tenum->name);
                        if (op->p2 < et->tenum->nconstructs) {
                            printf("::");
                            print_ustr(et->tenum->constructs[op->p2].name);
                            printf("[C%d]", op->p2);
                        }
                    }
                }
                break;
            case OEnumIndex:
                printf("  ; r%d = r%d.index", op->p1, op->p2);
                break;
            case OEnumField:
                /* p1=dst, p2=enum_reg, p3=construct_idx, extra=field_idx */
                {
                    hl_type *et = f->regs[op->p2];
                    int tidx = find_type_index(c, et);
                    int construct_idx = op->p3;
                    int field_idx = (int)(int_val)op->extra;
                    printf("  ; r%d = r%d T%d ", op->p1, op->p2, tidx);
                    if (et && et->kind == HENUM && et->tenum && construct_idx < et->tenum->nconstructs) {
                        hl_enum_construct *ec = &et->tenum->constructs[construct_idx];
                        print_ustr(et->tenum->name);
                        printf("::");
                        print_ustr(ec->name);
                        printf("[C%d].P%d", construct_idx, field_idx);
                    }
                }
                break;
            case OSetEnumField:
                /* p1=enum_reg, p2=field_idx, p3=value_reg - always construct 0 */
                {
                    hl_type *et = f->regs[op->p1];
                    int tidx = find_type_index(c, et);
                    printf("  ; r%d T%d [C0].P%d = r%d", op->p1, tidx, op->p2, op->p3);
                }
                break;
            default:
                break;
        }
        /* Print debug info (file:line) */
        if (c->hasdebug && f->debug) {
            int file_idx = f->debug[i * 2] & 0x7FFFFFFF;
            int line = f->debug[i * 2 + 1];
            if (file_idx < c->ndebugfiles && line > 0) {
                printf("  @ %s:%d", c->debugfiles[file_idx], line);
            }
        }
        printf("\n");
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file.hl> [function_index | -a] [-v]\n", argv[0]);
        fprintf(stderr, "  -a: dump all functions\n");
        fprintf(stderr, "  -v: verbose (show register types)\n");
        return 1;
    }

    const char *filename = argv[1];
    int target_func = -1;  /* -1 means entrypoint only */
    int dump_all = 0;
    int verbose = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-a") == 0) {
            dump_all = 1;
        } else {
            target_func = atoi(argv[i]);
        }
    }

    /* Initialize HL */
    hl_global_init();

    /* Load the bytecode */
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", filename);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    int size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *data = malloc(size);
    fread(data, 1, size, f);
    fclose(f);

    /* Parse bytecode */
    char *error_msg = NULL;
    hl_code *code = hl_code_read((unsigned char*)data, size, &error_msg);
    free(data);

    if (!code) {
        fprintf(stderr, "Failed to parse bytecode: %s\n", error_msg ? error_msg : "unknown error");
        return 1;
    }

    /* Print summary */
    printf("HashLink Bytecode: %s\n", filename);
    printf("  Version: %d\n", code->version);
    printf("  Entrypoint: F%d\n", code->entrypoint);
    printf("  Types: %d\n", code->ntypes);
    printf("  Globals: %d\n", code->nglobals);
    printf("  Natives: %d\n", code->nnatives);
    printf("  Functions: %d\n", code->nfunctions);
    printf("  Strings: %d\n", code->nstrings);
    printf("  Ints: %d\n", code->nints);
    printf("  Floats: %d\n", code->nfloats);

    /* Print natives */
    if (code->nnatives > 0) {
        printf("\n=== Natives ===\n");
        for (int i = 0; i < code->nnatives; i++) {
            hl_native *n = &code->natives[i];
            printf("  F%d: %s@%s ", n->findex, n->name, n->lib);
            print_type_with_index(code, n->t);
            printf("\n");
        }
    }

    /* Comprehensive dump when -a flag is set */
    if (dump_all) {
        /* String literals */
        printf("\n=== String Literals (%d) ===\n", code->nstrings);
        for (int i = 0; i < code->nstrings; i++) {
            printf("  S%d: \"%s\"\n", i, code->strings[i]);
        }

        /* Integer constants */
        printf("\n=== Integer Constants (%d) ===\n", code->nints);
        for (int i = 0; i < code->nints; i++) {
            printf("  I%d: %d (0x%08x)\n", i, code->ints[i], code->ints[i]);
        }

        /* Float constants */
        printf("\n=== Float Constants (%d) ===\n", code->nfloats);
        for (int i = 0; i < code->nfloats; i++) {
            printf("  FL%d: %g\n", i, code->floats[i]);
        }

        /* Bytes constants */
        if (code->nbytes > 0) {
            printf("\n=== Bytes Constants (%d) ===\n", code->nbytes);
            for (int i = 0; i < code->nbytes; i++) {
                int pos = code->bytes_pos[i];
                int end = (i + 1 < code->nbytes) ? code->bytes_pos[i + 1] : code->bytes_size;
                int len = end - pos;
                printf("  B%d: [%d bytes] ", i, len);
                /* Print first 32 bytes as hex */
                for (int j = pos; j < end && j < pos + 32; j++) {
                    printf("%02x ", (unsigned char)code->bytes[j]);
                }
                if (len > 32) printf("...");
                printf("\n");
            }
        }

        /* Debug source files */
        if (code->hasdebug && code->ndebugfiles > 0) {
            printf("\n=== Debug Source Files (%d) ===\n", code->ndebugfiles);
            for (int i = 0; i < code->ndebugfiles; i++) {
                printf("  D%d: %s\n", i, code->debugfiles[i]);
            }
        }

        /* Types (full details) */
        printf("\n=== Types (%d) ===\n", code->ntypes);
        for (int i = 0; i < code->ntypes; i++) {
            printf("  T%d: ", i);
            print_type_full(&code->types[i], 4);
        }

        /* Global variables */
        printf("\n=== Global Variables (%d) ===\n", code->nglobals);
        for (int i = 0; i < code->nglobals; i++) {
            printf("  G%d: ", i);
            print_type_with_index(code, code->globals[i]);
            printf("\n");
        }

        /* Constants (pre-initialized globals) */
        if (code->nconstants > 0) {
            printf("\n=== Constants (%d) ===\n", code->nconstants);
            for (int i = 0; i < code->nconstants; i++) {
                hl_constant *cst = &code->constants[i];
                hl_type *gt = code->globals[cst->global];
                printf("  C%d: G%d ", i, cst->global);
                print_type_with_index(code, gt);
                printf("\n");
                /* Show field initializers */
                if (gt && (gt->kind == HOBJ || gt->kind == HSTRUCT) && gt->obj) {
                    for (int j = 0; j < cst->nfields && j < gt->obj->nfields; j++) {
                        int idx = cst->fields[j];
                        hl_type *ft = gt->obj->fields[j].t;
                        printf("       F%d ", j);
                        print_ustr(gt->obj->fields[j].name);
                        printf(" = ");
                        /* Decode value based on field type */
                        if (ft) {
                            switch (ft->kind) {
                                case HI32:
                                    if (idx < code->nints)
                                        printf("I%d (%d)", idx, code->ints[idx]);
                                    else
                                        printf("I%d", idx);
                                    break;
                                case HF64:
                                    if (idx < code->nfloats)
                                        printf("FL%d (%g)", idx, code->floats[idx]);
                                    else
                                        printf("FL%d", idx);
                                    break;
                                case HBYTES:
                                    printf("S%d", idx);
                                    if (idx < code->nstrings)
                                        printf(" \"%s\"", code->strings[idx]);
                                    break;
                                default:
                                    printf("[%d]", idx);
                                    break;
                            }
                        } else {
                            printf("[%d]", idx);
                        }
                        printf("\n");
                    }
                }
            }
        }
    }

    /* Dump functions */
    if (dump_all) {
        /* Dump all functions */
        printf("\n--- All Functions ---\n");
        for (int i = 0; i < code->nfunctions; i++) {
            dump_function(code, &code->functions[i], verbose);
        }
    } else if (target_func >= 0) {
        int found = 0;
        /* Check if it's a native function first */
        for (int i = 0; i < code->nnatives; i++) {
            if (code->natives[i].findex == target_func) {
                hl_native *n = &code->natives[i];
                printf("\n=== Native %d ===\n", n->findex);
                printf("  Library: %s\n", n->lib);
                printf("  Name: %s\n", n->name);
                printf("  Type: ");
                print_type_with_index(code, n->t);
                printf("\n");
                found = 1;
                break;
            }
        }
        /* Find and dump specific function */
        for (int i = 0; i < code->nfunctions; i++) {
            if (code->functions[i].findex == target_func) {
                dump_function(code, &code->functions[i], verbose);
                found = 1;
                break;
            }
        }
        if (!found) {
            printf("\nFunction F%d not found\n", target_func);
        }
    } else {
        /* Dump entrypoint function */
        printf("\n--- Entrypoint Function ---\n");
        for (int i = 0; i < code->nfunctions; i++) {
            if (code->functions[i].findex == code->entrypoint) {
                dump_function(code, &code->functions[i], verbose);
                break;
            }
        }
    }

    return 0;
}
