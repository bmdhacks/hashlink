/*
 * Copyright (C)2005-2016 Haxe Foundation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * hl2llvm - HashLink bytecode to LLVM IR AOT compiler
 *
 * Usage: hl2llvm [options] input.hl -o output
 *
 * Options:
 *   -o <file>      Output file (required)
 *   --emit-llvm    Output LLVM IR text (.ll)
 *   --emit-bc      Output LLVM bitcode (.bc)
 *   --emit-asm     Output native assembly (.s)
 *   --emit-obj     Output object file (.o) [default]
 *   -O0            No optimization
 *   -O1            Light optimization
 *   -O2            Default optimization [default]
 *   -O3            Aggressive optimization
 *   --inline-threshold=N  Set inliner threshold (default: use LLVM's, try 5000 for aggressive)
 *   --fast-math[=MODE]    Enable fast-math (MODE: safe or full, default=safe)
 *   --mcpu=X       Override target CPU (default: auto-detect)
 *   --mattr=X      Override/set target features (e.g., "+v8.4a,+crypto")
 *   -g             Emit debug info
 *   -v             Verbose output
 *   --rss          Report memory usage (RSS) per batch
 *   --batch        Batched compilation (~200 functions/batch)
 *   --batch-size=N Custom batch size
 *   --threads=N    Parallel processes for batch compilation (default: auto)
 *   --link         Link object files into executable (requires --batch)
 *   -L <dir>       Add library search path (for --link)
 *   --help         Show this help
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <libgen.h>
#include <errno.h>
#include <unistd.h>
#include "llvm_codegen.h"

/* From llvm_link.cpp — LLD-based ELF linker */
extern int llvm_lld_link_elf(const char *manifest_path, const char *output_path,
                             const char *lib_dirs, int verbose);

/* Get current RSS in KB from /proc/self/status */
static long get_rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;

    char line[256];
    long rss = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%ld", &rss);
            break;
        }
    }
    fclose(f);
    return rss;
}

static void print_usage(const char *prog) {
    printf("HashLink bytecode to LLVM IR AOT compiler\n\n");
    printf("Usage: %s [options] input.hl -o output\n\n", prog);
    printf("Options:\n");
    printf("  -o <file>      Output file (required)\n");
    printf("  --emit-llvm    Output LLVM IR text (.ll)\n");
    printf("  --emit-bc      Output LLVM bitcode (.bc)\n");
    printf("  --emit-asm     Output native assembly (.s)\n");
    printf("  --emit-obj     Output object file (.o) [default]\n");
    printf("  -O0            No optimization\n");
    printf("  -O1            Light optimization\n");
    printf("  -O2            Default optimization [default]\n");
    printf("  -O3            Aggressive optimization\n");
    printf("  --inline-threshold=N  Set inliner threshold (try 5000 for aggressive)\n");
    printf("  --fast-math[=MODE]    Fast-math: 'safe' (default) or 'full' (breaks NaN/Inf)\n");
    printf("  --mcpu=X       Override target CPU (default: auto-detect, use -v to see)\n");
    printf("  --mattr=X      Set target features (e.g., \"+v8.4a,+crypto,+fullfp16\")\n");
    printf("  -g             Emit debug info\n");
    printf("  -v             Verbose output (shows detected CPU/features)\n");
    printf("  --rss          Report memory usage (RSS) per batch\n");
    printf("  --batch        Batched compilation (~200 functions/batch)\n");
    printf("  --batch-size=N Custom batch size (minimum 10)\n");
    printf("  --threads=N    Parallel processes for batch compilation (0=auto)\n");
    printf("  --link         Link object files into executable (requires --batch)\n");
    printf("  -L <dir>       Add library search path (for --link)\n");
    printf("  --help         Show this help\n");
}

int main(int argc, char **argv) {
    const char *input_file = NULL;
    const char *output_file = NULL;
    llvm_output_format format = LLVM_OUTPUT_OBJECT;
    llvm_opt_level opt_level = LLVM_OPT_DEFAULT;
    bool emit_debug = false;
    bool verbose = false;
    bool report_rss = false;
    int batch_size = 0;  /* 0 = single file, >0 = batched mode */
    int thread_count = 0;  /* 0 = auto-detect CPU count */
    int inline_threshold = 0;  /* 0 = use LLVM default */
    int fast_math = 0;         /* 0=off, 1=safe, 2=full */
    bool do_link = false;
    const char *target_cpu = NULL;
    const char *target_features = NULL;
    /* Library search paths for --link (colon-separated) */
    char lib_dirs[4096] = "";
    int lib_dirs_len = 0;

    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            fflush(stdout);
            _exit(0);
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -o requires an argument\n");
                _exit(1);
            }
            output_file = argv[++i];
        } else if (strcmp(argv[i], "--emit-llvm") == 0) {
            format = LLVM_OUTPUT_LLVM_IR;
        } else if (strcmp(argv[i], "--emit-bc") == 0) {
            format = LLVM_OUTPUT_BITCODE;
        } else if (strcmp(argv[i], "--emit-asm") == 0) {
            format = LLVM_OUTPUT_ASSEMBLY;
        } else if (strcmp(argv[i], "--emit-obj") == 0) {
            format = LLVM_OUTPUT_OBJECT;
        } else if (strcmp(argv[i], "-O0") == 0) {
            opt_level = LLVM_OPT_NONE;
        } else if (strcmp(argv[i], "-O1") == 0) {
            opt_level = LLVM_OPT_LESS;
        } else if (strcmp(argv[i], "-O2") == 0) {
            opt_level = LLVM_OPT_DEFAULT;
        } else if (strcmp(argv[i], "-O3") == 0) {
            opt_level = LLVM_OPT_AGGRESSIVE;
        } else if (strcmp(argv[i], "-g") == 0) {
            emit_debug = true;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--rss") == 0) {
            report_rss = true;
        } else if (strcmp(argv[i], "--batch") == 0) {
            batch_size = 200;  /* Default batch size */
        } else if (strncmp(argv[i], "--batch-size=", 13) == 0) {
            batch_size = atoi(argv[i] + 13);
            if (batch_size < 10) batch_size = 10;  /* Minimum batch size */
        } else if (strncmp(argv[i], "--threads=", 10) == 0) {
            thread_count = atoi(argv[i] + 10);
            if (thread_count < 0) thread_count = 0;
        } else if (strncmp(argv[i], "--inline-threshold=", 19) == 0) {
            inline_threshold = atoi(argv[i] + 19);
            if (inline_threshold < 0) inline_threshold = 0;
        } else if (strcmp(argv[i], "--fast-math") == 0 ||
                   strcmp(argv[i], "--fast-math=safe") == 0) {
            fast_math = 1;  /* safe mode */
        } else if (strcmp(argv[i], "--fast-math=full") == 0) {
            fast_math = 2;  /* full mode (breaks NaN/Inf) */
        } else if (strncmp(argv[i], "--mcpu=", 7) == 0) {
            target_cpu = argv[i] + 7;
        } else if (strncmp(argv[i], "--mattr=", 8) == 0) {
            target_features = argv[i] + 8;
        } else if (strcmp(argv[i], "--link") == 0) {
            do_link = true;
        } else if (strcmp(argv[i], "-L") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -L requires an argument\n");
                _exit(1);
            }
            const char *dir = argv[++i];
            if (lib_dirs_len > 0) {
                lib_dirs[lib_dirs_len++] = ':';
            }
            int dlen = strlen(dir);
            memcpy(lib_dirs + lib_dirs_len, dir, dlen);
            lib_dirs_len += dlen;
            lib_dirs[lib_dirs_len] = '\0';
        } else if (strncmp(argv[i], "-L", 2) == 0 && argv[i][2] != '\0') {
            /* -L/path/to/dir (no space) */
            const char *dir = argv[i] + 2;
            if (lib_dirs_len > 0) {
                lib_dirs[lib_dirs_len++] = ':';
            }
            int dlen = strlen(dir);
            memcpy(lib_dirs + lib_dirs_len, dir, dlen);
            lib_dirs_len += dlen;
            lib_dirs[lib_dirs_len] = '\0';
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            _exit(1);
        } else {
            if (input_file != NULL) {
                fprintf(stderr, "Error: Multiple input files specified\n");
                _exit(1);
            }
            input_file = argv[i];
        }
    }

    if (input_file == NULL) {
        fprintf(stderr, "Error: No input file specified\n");
        print_usage(argv[0]);
        _exit(1);
    }

    if (output_file == NULL) {
        fprintf(stderr, "Error: No output file specified (use -o)\n");
        _exit(1);
    }

    if (do_link && batch_size == 0) {
        fprintf(stderr, "Error: --link requires --batch mode\n");
        _exit(1);
    }

    /* Initialize HashLink runtime (needed for hl_code_read) */
    hl_global_init();

    /* Load the HashLink bytecode file */
    if (verbose) {
        printf("Loading %s...\n", input_file);
    }

    FILE *f = fopen(input_file, "rb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open file %s\n", input_file);
        _exit(1);
    }

    fseek(f, 0, SEEK_END);
    int size = (int)ftell(f);
    fseek(f, 0, SEEK_SET);

    char *fdata = (char *)malloc(size);
    if (!fdata) {
        fprintf(stderr, "Error: Out of memory\n");
        fclose(f);
        _exit(1);
    }

    if (fread(fdata, 1, size, f) != (size_t)size) {
        fprintf(stderr, "Error: Failed to read %s\n", input_file);
        free(fdata);
        fclose(f);
        _exit(1);
    }
    fclose(f);

    char *error_msg = NULL;
    hl_code *code = hl_code_read((unsigned char *)fdata, size, &error_msg);
    /* Keep fdata around - we'll embed it in the binary */

    if (code == NULL) {
        free(fdata);
        fprintf(stderr, "Error: Failed to load %s: %s\n", input_file,
                error_msg ? error_msg : "unknown error");
        _exit(1);
    }

    if (verbose) {
        printf("Loaded bytecode: %d functions, %d types, %d globals\n",
               code->nfunctions, code->ntypes, code->nglobals);

        /* Show target CPU/features info */
        char *detected_cpu = LLVMGetHostCPUName();
        char *detected_features = LLVMGetHostCPUFeatures();
        char *triple = LLVMGetDefaultTargetTriple();
        printf("Target triple: %s\n", triple);
        printf("Detected CPU: %s%s\n", detected_cpu,
               target_cpu ? " (overridden)" : "");
        if (target_cpu) {
            printf("Using CPU: %s\n", target_cpu);
        }
        printf("Detected features: %s%s\n", detected_features,
               target_features ? " (overridden)" : "");
        if (target_features) {
            printf("Using features: %s\n", target_features);
        }
        LLVMDisposeMessage(detected_cpu);
        LLVMDisposeMessage(detected_features);
        LLVMDisposeMessage(triple);
    }

    /* Create minimal module context - needed for hl_get_obj_rt() during compilation.
     * We don't use hl_module_alloc() because it's not exported from libhl and
     * pulls in JIT dependencies we don't need. */
    static hl_module_context module_ctx;
    memset(&module_ctx, 0, sizeof(module_ctx));
    hl_alloc_init(&module_ctx.alloc);

    /* Set up functions_types array - needed by hl_get_obj_rt() for method lookups */
    int total_functions = code->nfunctions + code->nnatives;
    module_ctx.functions_types = (hl_type **)malloc(sizeof(hl_type *) * total_functions);
    memset(module_ctx.functions_types, 0, sizeof(hl_type *) * total_functions);
    for (int i = 0; i < code->nfunctions; i++) {
        hl_function *fn = &code->functions[i];
        module_ctx.functions_types[fn->findex] = fn->type;
    }
    for (int i = 0; i < code->nnatives; i++) {
        hl_native *n = &code->natives[i];
        module_ctx.functions_types[n->findex] = n->t;
    }

    /* Set module context on object types so hl_get_obj_rt() can compute field offsets */
    for (int i = 0; i < code->ntypes; i++) {
        hl_type *t = &code->types[i];
        if ((t->kind == HOBJ || t->kind == HSTRUCT) && t->obj) {
            t->obj->m = &module_ctx;
        }
    }

    /* Initialize enum types so construct offsets are computed */
    for (int i = 0; i < code->ntypes; i++) {
        hl_type *t = &code->types[i];
        if (t->kind == HENUM && t->tenum) {
            hl_init_enum(t, &module_ctx);
        }
    }

    /* Batch compilation mode */
    if (batch_size > 0) {
        int num_batches = (code->nfunctions + batch_size - 1) / batch_size;

        /* Create output directory: foo.o.d/ */
        char output_dir[4096];
        snprintf(output_dir, sizeof(output_dir), "%s.d", output_file);
        if (mkdir(output_dir, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "Error: Cannot create output directory %s\n", output_dir);
            hl_free(&module_ctx.alloc);
            free(module_ctx.functions_types);
            free(fdata);
            hl_code_free(code);
            _exit(1);
        }

        /* Create manifest file */
        char manifest_path[4096];
        snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.txt", output_dir);
        FILE *manifest = fopen(manifest_path, "w");
        if (!manifest) {
            fprintf(stderr, "Error: Cannot create manifest file %s\n", manifest_path);
            hl_free(&module_ctx.alloc);
            free(module_ctx.functions_types);
            free(fdata);
            hl_code_free(code);
            _exit(1);
        }
        fprintf(manifest, "# hl2llvm batch compilation manifest\n");
        fprintf(manifest, "# Link with: clang @%s -lhl -o output\n", manifest_path);

        /* Pre-compute all shared lazy state before threading.
         * These functions cache results in shared structures without locking,
         * so we must populate caches before any parallel access. */

        /* hl_get_ustring: caches UTF-16 conversions in code->ustrings[] */
        for (int i = 0; i < code->nstrings; i++) {
            hl_get_ustring(code, i);
        }

        /* hl_get_obj_rt: caches runtime object info (field offsets, sizes).
         * While it uses hl_global_lock internally, pre-computing avoids lock
         * contention and shared allocator access during threaded compilation. */
        for (int i = 0; i < code->ntypes; i++) {
            hl_type *t = &code->types[i];
            if ((t->kind == HOBJ || t->kind == HSTRUCT) && t->obj) {
                hl_get_obj_rt(t);
            }
        }

        /* Determine process count for parallel compilation.
         * We use fork() rather than threads because LLVM's pass managers
         * (both legacy and new) have global state that is not thread-safe.
         * fork() gives each child its own address space, completely isolating
         * LLVM state, while sharing read-only data (hl_code, pre-computed
         * caches) via copy-on-write pages at no extra memory cost. */
        int total_work = num_batches + 1;  /* function batches + final */
        int max_procs = thread_count;
        if (max_procs <= 0) {
            long nproc = sysconf(_SC_NPROCESSORS_ONLN);
            max_procs = nproc > 0 ? (int)nproc : 1;
        }
        if (max_procs > total_work) max_procs = total_work;

        if (verbose) {
            printf("Batch compilation: %d functions, %d batches of %d, %d process%s\n",
                   code->nfunctions, num_batches, batch_size,
                   max_procs, max_procs > 1 ? "es" : "");
        }

        /* Fork child processes to compile batches in parallel */
        int running = 0;
        bool any_failed = false;

        for (int batch = 0; batch < total_work; batch++) {
            /* Wait for a slot if at capacity */
            while (running >= max_procs) {
                int status;
                pid_t done = waitpid(-1, &status, 0);
                if (done > 0) {
                    running--;
                    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                        if (WIFSIGNALED(status)) {
                            fprintf(stderr, "Error: Child killed by signal %d\n",
                                    WTERMSIG(status));
                        }
                        any_failed = true;
                    }
                } else if (done == -1 && errno != EINTR) {
                    /* ECHILD: no children to wait for (shouldn't happen) */
                    break;
                }
                /* EINTR: retry */
            }

            if (any_failed) break;

            pid_t pid = fork();
            if (pid < 0) {
                fprintf(stderr, "Error: fork() failed for batch %d: %s\n",
                        batch, strerror(errno));
                any_failed = true;
                break;
            }

            if (pid == 0) {
                /* ---- Child process: compile one batch ---- */
                fclose(manifest);  /* Child doesn't write manifest */

                llvm_ctx *ctx = llvm_create_context();
                if (!ctx) {
                    fprintf(stderr, "Error: Failed to create LLVM context for batch %d\n", batch);
                    _exit(1);
                }

                ctx->opt_level = opt_level;
                ctx->emit_debug_info = emit_debug;
                ctx->inline_threshold = inline_threshold;
                ctx->fast_math = fast_math;
                ctx->target_cpu = target_cpu;
                ctx->target_features = target_features;
                ctx->bytecode_data = (unsigned char *)fdata;
                ctx->bytecode_size = size;

                if (batch < num_batches) {
                    ctx->batch_mode = (batch == 0) ? LLVM_BATCH_FIRST : LLVM_BATCH_SUBSEQUENT;
                    ctx->batch_start = batch * batch_size;
                    ctx->batch_end = (batch + 1) * batch_size;
                    if (ctx->batch_end > code->nfunctions)
                        ctx->batch_end = code->nfunctions;
                } else {
                    ctx->batch_mode = LLVM_BATCH_FINAL;
                    ctx->batch_start = 0;
                    ctx->batch_end = 0;
                }

                char batch_name[64];
                snprintf(batch_name, sizeof(batch_name), "batch_%d", batch);

                if (verbose) {
                    if (ctx->batch_mode == LLVM_BATCH_FINAL) {
                        printf("  Batch %d/%d: entry point (final)\n",
                               batch + 1, total_work);
                    } else {
                        printf("  Batch %d/%d: functions %d-%d\n",
                               batch + 1, total_work,
                               ctx->batch_start, ctx->batch_end - 1);
                    }
                }

                if (!llvm_init_module(ctx, code, batch_name)) {
                    fprintf(stderr, "Error: Batch %d init failed: %s\n",
                            batch, ctx->error_msg ? ctx->error_msg : "unknown");
                    _exit(1);
                }

                if (ctx->batch_mode != LLVM_BATCH_FINAL) {
                    for (int i = ctx->batch_start; i < ctx->batch_end; i++) {
                        if (!llvm_compile_function(ctx, &code->functions[i])) {
                            fprintf(stderr, "Error: Function %d failed: %s\n",
                                    i, ctx->error_msg ? ctx->error_msg : "unknown");
                            _exit(1);
                        }
                    }
                } else {
                    if (!llvm_generate_entry_point(ctx, code->entrypoint)) {
                        fprintf(stderr, "Error: Entry point failed: %s\n",
                                ctx->error_msg ? ctx->error_msg : "unknown");
                        _exit(1);
                    }
                }

                llvm_finalize_module(ctx);

                if (!llvm_verify(ctx)) {
                    fprintf(stderr, "Error: Batch %d verify failed: %s\n",
                            batch, ctx->error_msg ? ctx->error_msg : "unknown");
                    _exit(1);
                }

                if (opt_level > LLVM_OPT_NONE) {
                    llvm_optimize(ctx);
                }

                char batch_file[4096];
                if (ctx->batch_mode == LLVM_BATCH_FINAL) {
                    snprintf(batch_file, sizeof(batch_file), "%s/final.o", output_dir);
                } else {
                    snprintf(batch_file, sizeof(batch_file), "%s/batch.%d.o",
                             output_dir, batch + 1);
                }

                if (!llvm_output(ctx, batch_file, format)) {
                    fprintf(stderr, "Error: Batch %d output failed: %s\n",
                            batch, ctx->error_msg ? ctx->error_msg : "unknown");
                    _exit(1);
                }

                if (report_rss) {
                    long rss = get_rss_kb();
                    if (rss > 0) {
                        printf("  Batch %d RSS: %ld MB\n", batch + 1, rss / 1024);
                    }
                }

                _exit(0);
            }

            /* Parent: track child */
            running++;
        }

        /* Wait for remaining children */
        while (running > 0) {
            int status;
            pid_t done = waitpid(-1, &status, 0);
            if (done > 0) {
                running--;
                if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                    if (WIFSIGNALED(status)) {
                        fprintf(stderr, "Error: Child killed by signal %d\n",
                                WTERMSIG(status));
                    }
                    any_failed = true;
                }
            } else if (done == -1 && errno != EINTR) {
                /* ECHILD: no children left despite running > 0.
                 * Can happen if SIGCHLD is SIG_IGN (auto-reap). */
                break;
            }
            /* EINTR: retry */
        }

        if (any_failed) {
            fprintf(stderr, "Error: Batch compilation failed\n");
            fclose(manifest);
            hl_free(&module_ctx.alloc);
            free(module_ctx.functions_types);
            free(fdata);
            hl_code_free(code);
            _exit(1);
        }

        /* Write manifest — filenames are deterministic */
        for (int batch = 0; batch < num_batches; batch++) {
            fprintf(manifest, "%s/batch.%d.o\n", output_dir, batch + 1);
        }
        fprintf(manifest, "%s/final.o\n", output_dir);
        fclose(manifest);

        if (verbose) {
            printf("Batch compilation complete: %d batches + final\n", num_batches);
        }

        if (do_link) {
            if (verbose) {
                printf("Linking %s...\n", output_file);
            }
            int link_result = llvm_lld_link_elf(manifest_path, output_file,
                                                 lib_dirs[0] ? lib_dirs : NULL,
                                                 verbose);
            if (link_result != 0) {
                fprintf(stderr, "Error: Linking failed\n");
                hl_free(&module_ctx.alloc);
                free(module_ctx.functions_types);
                free(fdata);
                hl_code_free(code);
                _exit(1);
            }
            if (verbose) {
                printf("Linked: %s\n", output_file);
            }
        } else if (verbose) {
            printf("Link with: clang @%s -lhl -o output\n", manifest_path);
        }

        /* Cleanup and exit — use _exit() to avoid LLVM/LLD static
         * destructor double-free when both shared libraries are loaded */
        hl_free(&module_ctx.alloc);
        free(module_ctx.functions_types);
        free(fdata);
        hl_code_free(code);
        _exit(0);
    }

    /* Single-file compilation mode (original behavior) */

    /* Create LLVM context */
    llvm_ctx *ctx = llvm_create_context();
    if (ctx == NULL) {
        fprintf(stderr, "Error: Failed to create LLVM context\n");
        hl_code_free(code);
        _exit(1);
    }

    ctx->opt_level = opt_level;
    ctx->emit_debug_info = emit_debug;
    ctx->inline_threshold = inline_threshold;
    ctx->fast_math = fast_math;
    ctx->target_cpu = target_cpu;
    ctx->target_features = target_features;
    ctx->bytecode_data = (unsigned char *)fdata;
    ctx->bytecode_size = size;

    /* Initialize the module */
    if (verbose) {
        printf("Initializing LLVM module...\n");
    }

    if (!llvm_init_module(ctx, code, input_file)) {
        fprintf(stderr, "Error: Failed to initialize module: %s\n",
                ctx->error_msg ? ctx->error_msg : "unknown error");
        llvm_destroy_context(ctx);
        hl_code_free(code);
        _exit(1);
    }

    /* Compile all functions */
    if (verbose) {
        printf("Compiling %d functions...\n", code->nfunctions);
    }

    for (int i = 0; i < code->nfunctions; i++) {
        hl_function *f = &code->functions[i];
        if (verbose && (i % 100 == 0 || i == code->nfunctions - 1)) {
            printf("  Compiling function %d/%d...\n", i + 1, code->nfunctions);
        }
        if (!llvm_compile_function(ctx, f)) {
            fprintf(stderr, "Error: Failed to compile function %d: %s\n",
                    i, ctx->error_msg ? ctx->error_msg : "unknown error");
            llvm_destroy_context(ctx);
            hl_code_free(code);
            _exit(1);
        }
    }

    /* Generate entry point */
    if (verbose) {
        printf("Generating entry point...\n");
    }

    if (!llvm_generate_entry_point(ctx, code->entrypoint)) {
        fprintf(stderr, "Error: Failed to generate entry point: %s\n",
                ctx->error_msg ? ctx->error_msg : "unknown error");
        llvm_destroy_context(ctx);
        hl_code_free(code);
        _exit(1);
    }

    /* Finalize the module */
    if (!llvm_finalize_module(ctx)) {
        fprintf(stderr, "Error: Failed to finalize module: %s\n",
                ctx->error_msg ? ctx->error_msg : "unknown error");
        llvm_destroy_context(ctx);
        hl_code_free(code);
        _exit(1);
    }

    /* Verify the module */
    if (verbose) {
        printf("Verifying module...\n");
    }

    if (!llvm_verify(ctx)) {
        fprintf(stderr, "Error: Module verification failed: %s\n",
                ctx->error_msg ? ctx->error_msg : "unknown error");
        llvm_destroy_context(ctx);
        hl_code_free(code);
        _exit(1);
    }

    /* Optimize if requested */
    if (opt_level > LLVM_OPT_NONE) {
        if (verbose) {
            printf("Optimizing (level %d)...\n", opt_level);
        }
        llvm_optimize(ctx);
    }

    /* Write output */
    if (verbose) {
        const char *format_name;
        switch (format) {
        case LLVM_OUTPUT_LLVM_IR: format_name = "LLVM IR"; break;
        case LLVM_OUTPUT_BITCODE: format_name = "bitcode"; break;
        case LLVM_OUTPUT_ASSEMBLY: format_name = "assembly"; break;
        case LLVM_OUTPUT_OBJECT: format_name = "object"; break;
        default: format_name = "unknown"; break;
        }
        printf("Writing %s to %s...\n", format_name, output_file);
    }

    if (!llvm_output(ctx, output_file, format)) {
        fprintf(stderr, "Error: Failed to write output: %s\n",
                ctx->error_msg ? ctx->error_msg : "unknown error");
        llvm_destroy_context(ctx);
        hl_code_free(code);
        _exit(1);
    }

    if (report_rss) {
        long rss = get_rss_kb();
        if (rss > 0) {
            printf("RSS: %ld MB\n", rss / 1024);
        }
    }

    if (verbose) {
        printf("Done!\n");
    }

    /* Cleanup — use _exit() to avoid LLVM/LLD static destructor
     * double-free when both shared libraries are loaded */
    llvm_destroy_context(ctx);
    hl_free(&module_ctx.alloc);
    free(module_ctx.functions_types);
    free(fdata);
    hl_code_free(code);

    _exit(0);
}
