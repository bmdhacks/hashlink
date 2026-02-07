/*
 * llvm_link.cpp - Thin C++ wrapper around LLD's ELF linker.
 *
 * Provides a C-callable function so hl2llvm (which is C) can invoke
 * lld::elf::link() directly without needing a separate linker binary.
 */

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <vector>
#include <string>
#include <fstream>

#include "lld/Common/Driver.h"
#include "llvm/Support/raw_ostream.h"

LLD_HAS_DRIVER(elf)

/* Parse colon-separated directory list into a vector of strings */
static std::vector<std::string> split_dirs(const char *dirs) {
    std::vector<std::string> result;
    if (!dirs || !dirs[0])
        return result;
    std::string s(dirs);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t colon = s.find(':', pos);
        if (colon == std::string::npos)
            colon = s.size();
        std::string dir = s.substr(pos, colon - pos);
        if (!dir.empty())
            result.push_back(dir);
        pos = colon + 1;
    }
    return result;
}

/* Find all .hdll files in a directory */
static std::vector<std::string> find_hdlls(const std::string &dir) {
    std::vector<std::string> result;
    DIR *d = opendir(dir.c_str());
    if (!d)
        return result;
    struct dirent *entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name(entry->d_name);
        if (name.size() > 5 && name.substr(name.size() - 5) == ".hdll") {
            result.push_back(dir + "/" + name);
        }
    }
    closedir(d);
    return result;
}

/* Find libaot_runtime.a in the given directories */
static std::string find_aot_runtime(const std::vector<std::string> &dirs) {
    for (const auto &dir : dirs) {
        std::string path = dir + "/libaot_runtime.a";
        std::ifstream test(path);
        if (test.good())
            return path;
    }
    return "";
}

/* Find a shared library by name, searching user dirs first (supports versioned .so.N).
 * Looks for: lib<name>.so, lib<name>.so.* in user dirs, then system dirs.
 * Returns full path or empty string. */
static std::string find_lib(const char *name, const std::vector<std::string> &user_dirs) {
    std::string prefix = std::string("lib") + name + ".so";
    /* Search user dirs first, then system dirs */
    const char *sys_dirs[] = {
        "/lib64", "/usr/lib64",
        "/lib/aarch64-linux-gnu", "/usr/lib/aarch64-linux-gnu",
        "/lib", "/usr/lib",
        nullptr
    };
    std::vector<std::string> all_dirs(user_dirs);
    for (const char **p = sys_dirs; *p; p++)
        all_dirs.push_back(*p);

    for (const auto &dir : all_dirs) {
        /* Try exact match first (unversioned symlink) */
        std::string exact = dir + "/" + prefix;
        std::ifstream test_exact(exact);
        if (test_exact.good())
            return exact;

        /* Scan directory for versioned variants (e.g., libhl.so.1) */
        DIR *d = opendir(dir.c_str());
        if (!d) continue;
        struct dirent *entry;
        while ((entry = readdir(d)) != nullptr) {
            std::string fname(entry->d_name);
            /* Match lib<name>.so.* */
            if (fname.size() > prefix.size() && fname.substr(0, prefix.size() + 1) == prefix + ".") {
                closedir(d);
                return dir + "/" + fname;
            }
        }
        closedir(d);
    }
    return "";
}

/* Find CRT objects (crt1.o, crti.o, crtn.o) needed for a proper executable */
static std::string find_crt(const char *name) {
    const char *search_paths[] = {
        "/usr/lib64",
        "/usr/lib/aarch64-linux-gnu",
        "/usr/lib",
        "/lib64",
        "/lib/aarch64-linux-gnu",
        "/lib",
        nullptr
    };
    for (const char **p = search_paths; *p; p++) {
        std::string path = std::string(*p) + "/" + name;
        std::ifstream test(path);
        if (test.good())
            return path;
    }
    return "";
}

extern "C" {

/*
 * llvm_lld_link_elf - Link object files into an ELF executable.
 *
 * manifest_path: path to manifest.txt (one .o file per line, # comments)
 * output_path:   path for the output executable
 * lib_dirs:      colon-separated library search paths (e.g. "/path/to/libs")
 * verbose:       print linker invocation if true
 *
 * Returns 0 on success, non-zero on failure.
 */
int llvm_lld_link_elf(const char *manifest_path, const char *output_path,
                      const char *lib_dirs, int verbose) {
    /* Read manifest to get list of .o files */
    std::vector<std::string> obj_files;
    {
        std::ifstream manifest(manifest_path);
        if (!manifest.is_open()) {
            fprintf(stderr, "Error: Cannot open manifest %s\n", manifest_path);
            return 1;
        }
        std::string line;
        while (std::getline(manifest, line)) {
            if (line.empty() || line[0] == '#')
                continue;
            obj_files.push_back(line);
        }
    }

    if (obj_files.empty()) {
        fprintf(stderr, "Error: No object files in manifest\n");
        return 1;
    }

    auto user_dirs = split_dirs(lib_dirs);

    /* Collect all string arguments in a stable vector first,
     * then build the const char* args vector from it. */
    std::vector<std::string> str_args;

    str_args.push_back("ld.lld");
    str_args.push_back("-o");
    str_args.push_back(output_path);

    /* CRT startup objects */
    std::string crt1 = find_crt("crt1.o");
    std::string crti = find_crt("crti.o");
    std::string crtn = find_crt("crtn.o");
    if (!crt1.empty()) str_args.push_back(crt1);
    if (!crti.empty()) str_args.push_back(crti);

    /* All object files from manifest */
    for (const auto &obj : obj_files)
        str_args.push_back(obj);

    /* libaot_runtime.a — provides aot_types, aot_get_global, etc. */
    std::string aot_runtime = find_aot_runtime(user_dirs);
    if (!aot_runtime.empty()) {
        str_args.push_back(aot_runtime);
    } else {
        fprintf(stderr, "Warning: libaot_runtime.a not found in library paths\n");
    }

    /* User library search paths + rpath */
    for (const auto &dir : user_dirs) {
        str_args.push_back("-L" + dir);
        str_args.push_back("-rpath=" + dir);
    }

    /* System library search paths */
    str_args.push_back("-L/lib64");
    str_args.push_back("-L/usr/lib64");
    str_args.push_back("-L/lib/aarch64-linux-gnu");
    str_args.push_back("-L/usr/lib/aarch64-linux-gnu");
    str_args.push_back("-L/lib");
    str_args.push_back("-L/usr/lib");

    /* Required shared libraries.
     * Try to resolve full paths from user dirs first (supports versioned
     * .so.N files on filesystems without symlinks like vfat).
     * Falls back to -l flag for system library search. */
    const char *needed_libs[] = {"hl", "m", "dl", "pthread", "uv", "c", nullptr};
    for (const char **lib = needed_libs; *lib; lib++) {
        std::string path = find_lib(*lib, user_dirs);
        if (!path.empty()) {
            str_args.push_back(path);
        } else {
            str_args.push_back(std::string("-l") + *lib);
        }
    }

    /* hdll files — ELF shared objects with non-standard extension */
    for (const auto &dir : user_dirs) {
        auto hdlls = find_hdlls(dir);
        for (const auto &hdll : hdlls)
            str_args.push_back(hdll);
    }

    /* CRT finalization */
    if (!crtn.empty()) str_args.push_back(crtn);

    /* Dynamic linker */
    std::string dynlinker;
    /* Check common paths */
    const char *dynlinker_paths[] = {
        "/lib/ld-linux-aarch64.so.1",
        "/lib64/ld-linux-aarch64.so.1",
        "/usr/lib/ld-linux-aarch64.so.1",
        nullptr
    };
    for (const char **p = dynlinker_paths; *p; p++) {
        std::ifstream test(*p);
        if (test.good()) {
            dynlinker = std::string("--dynamic-linker=") + *p;
            break;
        }
    }
    if (!dynlinker.empty())
        str_args.push_back(dynlinker);

    /* Build const char* vector from stable strings */
    std::vector<const char *> args;
    for (const auto &s : str_args)
        args.push_back(s.c_str());

    if (verbose) {
        printf("Linker invocation:\n ");
        for (const auto &arg : args)
            printf(" %s", arg);
        printf("\n");
    }

    /* Invoke LLD */
    std::string lld_stdout_str, lld_stderr_str;
    llvm::raw_string_ostream lld_stdout(lld_stdout_str);
    llvm::raw_string_ostream lld_stderr(lld_stderr_str);

    lld::Result result = lld::lldMain(
        args,
        lld_stdout,
        lld_stderr,
        {{lld::Gnu, &lld::elf::link}}
    );

    if (!lld_stdout_str.empty())
        fprintf(stdout, "%s", lld_stdout_str.c_str());
    if (!lld_stderr_str.empty())
        fprintf(stderr, "%s", lld_stderr_str.c_str());

    if (result.retCode != 0) {
        fprintf(stderr, "Error: Linking failed (exit code %d)\n", result.retCode);
        return result.retCode;
    }

    return 0;
}

} /* extern "C" */
