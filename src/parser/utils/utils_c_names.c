// SPDX-License-Identifier: MIT
//
// The names the program's C declares: its #includes and raw blocks, run through the C
// preprocessor with the flags the C compiler gets (-E -dD), then read with tree-sitter's C
// grammar (utils_c_decls.c), which gives every declaration at file scope: functions,
// variables, typedefs, struct, union and enum tags, enum constants and macros, with their
// types, parameters and fields.
//
// With them, a name the Zen C code uses that neither it nor its C declares is an error at
// the .zc line ("Undefined function 'pritnf'; did you mean 'printf'?"), and a C type isn't
// "assumed". Without them (no C compiler, a header that doesn't preprocess, MISRA mode,
// the LSP, C++ or Objective-C output), every name C might declare is taken on trust, as
// before.

#include "../parser.h"
#include "c_decls.h"
#include "../../codegen/codegen.h"
#include "../../platform/os.h"
#include "../../utils/cmd.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

// ---- The set of names ---------------------------------------------------------------------

static unsigned c_name_hash(const char *s, size_t n)
{
    unsigned h = 2166136261u;
    for (size_t i = 0; i < n; i++)
    {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

static unsigned c_name_slot(char **names, unsigned cap, const char *s, size_t n)
{
    unsigned i = c_name_hash(s, n) & (cap - 1);
    while (names[i] && !(strlen(names[i]) == n && memcmp(names[i], s, n) == 0))
    {
        i = (i + 1) & (cap - 1);
    }
    return i;
}

void c_name_note(ParserContext *ctx, const char *s, size_t n)
{
    if (n == 0)
    {
        return;
    }
    if ((ctx->c_names_count + 1) * 2 > ctx->c_names_cap) // kept at most half full
    {
        unsigned cap = ctx->c_names_cap ? ctx->c_names_cap * 2 : 4096;
        char **names = xcalloc(cap, sizeof(char *));
        for (unsigned i = 0; i < ctx->c_names_cap; i++)
        {
            if (ctx->c_names[i])
            {
                names[c_name_slot(names, cap, ctx->c_names[i], strlen(ctx->c_names[i]))] =
                    ctx->c_names[i];
            }
        }
        zfree(ctx->c_names);
        ctx->c_names = names;
        ctx->c_names_cap = cap;
    }
    unsigned i = c_name_slot(ctx->c_names, ctx->c_names_cap, s, n);
    if (!ctx->c_names[i])
    {
        char *copy = xmalloc(n + 1);
        memcpy(copy, s, n);
        copy[n] = 0;
        ctx->c_names[i] = copy;
        ctx->c_names_count++;
    }
}

int c_name_known(ParserContext *ctx, const char *name)
{
    if (!ctx->c_names || !name)
    {
        return 0;
    }
    if (strncmp(name, "__builtin_", 10) == 0 || strcmp(name, "__func__") == 0 ||
        strcmp(name, "__FUNCTION__") == 0 || strcmp(name, "__PRETTY_FUNCTION__") == 0)
    {
        return 1; // the C compiler's own
    }
    return ctx->c_names[c_name_slot(ctx->c_names, ctx->c_names_cap, name, strlen(name))] != NULL;
}

const char *c_name_closest(ParserContext *ctx, const char *name)
{
    const char *best = NULL;
    int best_d = 3; // at most two edits away
    size_t n = strlen(name);
    for (unsigned i = 0; ctx->c_names && i < ctx->c_names_cap; i++)
    {
        const char *c = ctx->c_names[i];
        if (!c || c[0] == '_')
        {
            continue; // the implementation's own names aren't what was meant
        }
        size_t m = strlen(c);
        if (m + 2 < n || n + 2 < m)
        {
            continue;
        }
        int d = levenshtein(name, c);
        if (d < best_d)
        {
            best_d = d;
            best = c;
        }
    }
    return best;
}

// ---- Running the preprocessor ---------------------------------------------------------------

static char *c_read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0)
    {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    char *buf = xmalloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (buf)
    {
        buf[got] = 0;
    }
    return buf;
}

// ---- The cache ----------------------------------------------------------------------------
//
// Preprocessing the headers costs tens of milliseconds a transpile (about 24 ms for a
// program that includes libwgf), which a reload would pay on every save. So the names are
// kept in $XDG_CACHE_HOME/zenc/c-names (or ~/.cache/zenc/c-names), one file per
// translation unit and preprocessor command, and used while every file the preprocessor
// read (its line markers say which) has the same modification time and size.

static unsigned long long c_hash64(unsigned long long h, const char *s)
{
    for (; *s; s++)
    {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

static void c_mkdir(const char *path)
{
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static int c_cache_dir(char *out, size_t n)
{
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)
    {
        snprintf(out, n, "%s/zenc", xdg);
    }
    else if (home && *home)
    {
        snprintf(out, n, "%s/.cache", home);
        c_mkdir(out);
        snprintf(out, n, "%s/.cache/zenc", home);
    }
    else
    {
        return 0;
    }
    c_mkdir(out);
    size_t len = strlen(out);
    snprintf(out + len, n - len, "/c-names");
    c_mkdir(out);
    return 1;
}

static int c_stat(const char *path, long long *mtime, long long *size)
{
    struct stat st;
    if (stat(path, &st) != 0)
    {
        return 0;
    }
#if defined(__APPLE__)
    *mtime = (long long)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#elif defined(_WIN32)
    *mtime = (long long)st.st_mtime;
#else
    *mtime = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
    *size = (long long)st.st_size;
    return 1;
}

// The declarations, after the files: a line a declaration, its strings separated by tabs
// (C's text holds none: it is read with white space made single spaces), each "=" and the
// string, or empty for none; then a line for each parameter (P) and field (F).
//   D kind line end_line variadic params_known complete \t name \t file \t type \t text \t
//   aggregate

static void c_cache_put(FILE *f, const char *s)
{
    fputc('\t', f);
    if (s)
    {
        fputc('=', f);
        for (; *s; s++)
        {
            fputc(*s == '\t' || *s == '\n' ? ' ' : *s, f);
        }
    }
}

static void c_cache_write_decls(ParserContext *ctx, FILE *f)
{
    unsigned n = 0;
    const CDecl *const *all = c_decl_all(ctx, &n);
    for (unsigned i = 0; i < n; i++)
    {
        const CDecl *d = all[i];
        fprintf(f, "D %d %d %d %d %d %d", (int)d->kind, d->line, d->end_line, d->variadic,
                d->params_known, d->complete);
        c_cache_put(f, d->name);
        c_cache_put(f, d->file);
        c_cache_put(f, d->type);
        c_cache_put(f, d->text);
        c_cache_put(f, d->aggregate);
        fputc('\n', f);
        for (int k = 0; k < d->nparams; k++)
        {
            fputc('P', f);
            c_cache_put(f, d->params[k].name);
            c_cache_put(f, d->params[k].type);
            fputc('\n', f);
        }
        for (int k = 0; k < d->nfields; k++)
        {
            fputc('F', f);
            c_cache_put(f, d->fields[k].name);
            c_cache_put(f, d->fields[k].type);
            fputc('\n', f);
        }
    }
}

// The next tab-separated string of the line at *p (NULL if it was empty), ending it there.
static char *c_cache_get(char **p)
{
    if (**p != '\t')
    {
        return NULL;
    }
    char *s = ++*p;
    char *e = s;
    while (*e && *e != '\t')
    {
        e++;
    }
    *p = e;
    if (*s != '=')
    {
        return NULL;
    }
    char *out = xmalloc((size_t)(e - s));
    memcpy(out, s + 1, (size_t)(e - s - 1));
    out[e - s - 1] = 0;
    return out;
}

static int c_cache_read_decls(ParserContext *ctx, char *p)
{
    CDecl d;
    memset(&d, 0,
           sizeof(d)); // (no kind yet: 0 is none of CDeclKind's, which clang's -Wassign-enum minds)
    int have = 0;
    while (*p)
    {
        char *nl = strchr(p, '\n');
        if (!nl)
        {
            return 0;
        }
        *nl = 0;
        if (p[0] == 'D')
        {
            if (have)
            {
                c_decl_add(ctx, &d);
                c_name_note(ctx, d.name, strlen(d.name));
            }
            memset(&d, 0, sizeof d);
            int kind = 0, used = 0;
            if (sscanf(p, "D %d %d %d %d %d %d%n", &kind, &d.line, &d.end_line, &d.variadic,
                       &d.params_known, &d.complete, &used) < 6)
            {
                return 0;
            }
            d.kind = (CDeclKind)kind;
            char *q = p + used;
            d.name = c_cache_get(&q);
            d.file = c_cache_get(&q);
            d.type = c_cache_get(&q);
            d.text = c_cache_get(&q);
            d.aggregate = c_cache_get(&q);
            if (!d.name)
            {
                return 0;
            }
            have = 1;
        }
        else if ((p[0] == 'P' || p[0] == 'F') && have)
        {
            char *q = p + 1;
            CDeclField fl;
            fl.name = c_cache_get(&q);
            fl.type = c_cache_get(&q);
            CDeclField **list = p[0] == 'P' ? &d.params : &d.fields;
            int *count = p[0] == 'P' ? &d.nparams : &d.nfields;
            *list = xrealloc(*list, (size_t)(*count + 1) * sizeof(CDeclField));
            (*list)[(*count)++] = fl;
        }
        else
        {
            return 0;
        }
        p = nl + 1;
    }
    if (have)
    {
        c_decl_add(ctx, &d);
        c_name_note(ctx, d.name, strlen(d.name));
    }
    return 1;
}

// Loads the declarations of `path` if every file it lists is unchanged.
static int c_cache_load(ParserContext *ctx, const char *path)
{
    char *text = c_read_file(path);
    if (!text)
    {
        return 0;
    }
    if (strncmp(text, "zc-c-names 4\n", 13) != 0)
    {
        zfree(text);
        return 0;
    }
    char *p = text + 13;
    while (strncmp(p, "dep ", 4) == 0)
    {
        long long mt = 0, sz = 0, cmt = 0, csz = 0;
        int used = 0;
        if (sscanf(p + 4, "%lld %lld %n", &mt, &sz, &used) < 2)
        {
            zfree(text);
            return 0;
        }
        char *file = p + 4 + used, *nl = strchr(file, '\n');
        if (!nl)
        {
            zfree(text);
            return 0;
        }
        *nl = 0;
        if (!c_stat(file, &cmt, &csz) || cmt != mt || csz != sz)
        {
            zfree(text);
            return 0;
        }
        p = nl + 1;
    }
    if (strncmp(p, "---\n", 4) != 0)
    {
        zfree(text);
        return 0;
    }
    int ok = c_cache_read_decls(ctx, p + 4);
    zfree(text);
    return ok;
}

// Writes the names, and the files the preprocessed `text` came from, to `path`.
static void c_cache_save(ParserContext *ctx, const char *path, const char *text)
{
    char tmp[MAX_PATH_SIZE + 96];
    snprintf(tmp, sizeof tmp, "%s.%d.tmp", path, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f)
    {
        return;
    }
    fputs("zc-c-names 4\n", f);
    // The line markers: `# 1 "/usr/include/stdio.h" 1 3 4`.
    char **seen = NULL;
    size_t nseen = 0;
    for (const char *p = text; p && *p;)
    {
        const char *nl = strchr(p, '\n');
        if (p[0] == '#' && p[1] == ' ' && isdigit((unsigned char)p[2]))
        {
            const char *q = strchr(p, '"');
            const char *e = q ? strchr(q + 1, '"') : NULL;
            if (q && e && (!nl || e < nl) && q[1] != '<')
            {
                size_t n = (size_t)(e - q - 1);
                char file[MAX_PATH_SIZE];
                if (n < sizeof file)
                {
                    memcpy(file, q + 1, n);
                    file[n] = 0;
                    int dup = 0;
                    for (size_t i = 0; i < nseen && !dup; i++)
                    {
                        dup = strcmp(seen[i], file) == 0;
                    }
                    long long mt, sz;
                    if (!dup && c_stat(file, &mt, &sz))
                    {
                        fprintf(f, "dep %lld %lld %s\n", mt, sz, file);
                        seen = xrealloc(seen, (nseen + 1) * sizeof(char *));
                        seen[nseen++] = xstrdup(file);
                    }
                }
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    fputs("---\n", f);
    c_cache_write_decls(ctx, f);
    int ok = fclose(f) == 0;
    if (ok)
    {
        rename(tmp, path);
    }
    else
    {
        remove(tmp);
    }
}

void c_names_scan(ParserContext *ctx, ASTNode *root)
{
    ctx->c_names_ready = 0;
    CompilerConfig *cfg = ctx->config;
    const char *off = getenv("ZC_NO_C_NAMES"); // to compare, or to work round the scan
    // C only: C++, Objective-C and CUDA headers (namespaces, classes, overloads) aren't
    // read; those programs are as before.
    if (!ctx->compiler || cfg->misra_mode || cfg->mode_lsp || cfg->mode_doc || !cfg->cc[0] ||
        cfg->use_cpp || cfg->use_objc || cfg->use_cuda || (off && *off && strcmp(off, "0") != 0))
    {
        return;
    }
    char *unit = codegen_c_names_unit(ctx, root);
    if (!unit)
    {
        return;
    }
    static unsigned serial;
    char src[MAX_PATH_SIZE], out[MAX_PATH_SIZE];
    snprintf(src, sizeof src, "%s/zc_c_names_%d_%u.c", z_get_temp_dir(), (int)getpid(), serial);
    snprintf(out, sizeof out, "%s/zc_c_names_%d_%u.i", z_get_temp_dir(), (int)getpid(), serial++);
    ArgList args;
    arg_list_init(&args);
    build_preprocess_arg_list(&args, out, src, ctx->compiler);

    // The cache entry for this unit and command (the last three arguments, -o out src,
    // are this run's temporary files).
    unsigned long long key = c_hash64(14695981039346656037ULL, unit);
    for (size_t i = 0; i + 3 < args.count; i++)
    {
        key = c_hash64(c_hash64(key, args.args[i]), "\x1f");
    }
    char dir[MAX_PATH_SIZE], cached[MAX_PATH_SIZE + 32];
    int have_dir = c_cache_dir(dir, sizeof dir);
    if (have_dir)
    {
        snprintf(cached, sizeof cached, "%s/%016llx", dir, key);
        if (c_cache_load(ctx, cached))
        {
            arg_list_free(&args);
            zfree(unit);
            ctx->c_names_ready = ctx->c_names_count > 0;
            return;
        }
    }

    FILE *f = fopen(src, "wb");
    if (!f)
    {
        arg_list_free(&args);
        zfree(unit);
        return;
    }
    fputs(unit, f);
    fclose(f);
    zfree(unit);
    char errors[512]; // the C compiler reports a missing header itself, later
    int rc = z_run_command_capture_ex(args.args, errors, sizeof errors, 1);
    arg_list_free(&args);
    remove(src);
    char *text = rc == 0 ? c_read_file(out) : NULL;
    remove(out);
    if (!text)
    {
        return;
    }
    c_decls_extract(ctx, text);
    ctx->c_names_ready = ctx->c_names_count > 0;
    if (ctx->c_names_ready && have_dir)
    {
        c_cache_save(ctx, cached, text);
    }
    zfree(text);
}

void c_names_save(ParserContext *ctx, CNamesState *s)
{
    s->names = ctx->c_names;
    s->cap = ctx->c_names_cap;
    s->count = ctx->c_names_count;
    s->ready = ctx->c_names_ready;
    s->decls = ctx->c_decls;
}

void c_names_load(ParserContext *ctx, const CNamesState *s)
{
    ctx->c_names = s ? s->names : NULL;
    ctx->c_names_cap = s ? s->cap : 0;
    ctx->c_names_count = s ? s->count : 0;
    ctx->c_names_ready = s ? s->ready : 0;
    ctx->c_decls = s ? s->decls : NULL;
}

void c_names_scan_into(ParserContext *ctx, ASTNode *root, CNamesState *s)
{
    c_names_load(ctx, NULL);
    int lsp = ctx->config->mode_lsp;
    ctx->config->mode_lsp = 0;
    c_names_scan(ctx, root);
    ctx->config->mode_lsp = lsp;
    unsigned long long key = s->unit_key;
    c_names_save(ctx, s);
    s->unit_key = key;
}
