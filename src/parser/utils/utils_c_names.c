// SPDX-License-Identifier: MIT
//
// The names the program's C declares: its #includes and raw blocks, run through the C
// preprocessor with the flags the C compiler gets (-E -dD), then scanned for every name
// declared at file scope: functions, variables, typedefs, struct, union and enum tags, enum
// constants, and macros. Struct fields and parameters aren't names of their own.
//
// With them, a name the Zen C code uses that neither it nor its C declares is an error at
// the .zc line ("Undefined function 'pritnf'; did you mean 'printf'?"), and a C type isn't
// "assumed". Without them (no C compiler, a header that doesn't preprocess, MISRA mode,
// the LSP), every name C might declare is taken on trust, as before.
//
// The scan is tolerant: what it doesn't understand it skips, so a name can at worst be
// missed, and a missed name would be reported as undefined. It reads declarations, not
// expressions, and knows C's keywords and GCC's extensions that appear in system headers.

#include "../parser.h"
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

static void c_name_add(ParserContext *ctx, const char *s, size_t n)
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

// ---- Tokens -------------------------------------------------------------------------------

typedef enum
{
    CT_EOF,
    CT_IDENT,
    CT_PUNCT, // one character, in `c`
    CT_OTHER  // a number, string or character literal
} CTokKind;

typedef struct
{
    CTokKind kind;
    const char *s; // an identifier's text
    size_t n;
    char c;
} CTok;

typedef struct
{
    ParserContext *ctx;
    const char *p;
    int line_start;
} CScan;

// The next token; a preprocessor line (a #define, a line marker) is read as it is met.
static CTok c_next(CScan *sc)
{
    CTok t = {CT_EOF, NULL, 0, 0};
    const char *p = sc->p;
    for (;;)
    {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\f' || *p == '\v')
        {
            p++;
        }
        if (*p == '\n')
        {
            p++;
            sc->line_start = 1;
            continue;
        }
        if (*p == '#' && sc->line_start)
        {
            const char *d = p + 1;
            while (*d == ' ' || *d == '\t')
            {
                d++;
            }
            if (strncmp(d, "define", 6) == 0 && (d[6] == ' ' || d[6] == '\t'))
            {
                d += 6;
                while (*d == ' ' || *d == '\t')
                {
                    d++;
                }
                const char *b = d;
                while (isalnum((unsigned char)*d) || *d == '_' || *d == '$')
                {
                    d++;
                }
                c_name_add(sc->ctx, b, (size_t)(d - b));
            }
            // To the end of the line, with its continuations.
            while (*p && *p != '\n')
            {
                if (*p == '\\' && p[1] == '\n')
                {
                    p++;
                }
                p++;
            }
            continue;
        }
        if (p[0] == '/' && p[1] == '/') // -dD keeps macro bodies as written; none outside
        {
            while (*p && *p != '\n')
            {
                p++;
            }
            continue;
        }
        if (p[0] == '/' && p[1] == '*')
        {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/'))
            {
                p++;
            }
            if (*p)
            {
                p += 2;
            }
            continue;
        }
        break;
    }
    sc->line_start = 0;
    if (!*p)
    {
        sc->p = p;
        return t;
    }
    if (isalpha((unsigned char)*p) || *p == '_' || *p == '$')
    {
        const char *b = p;
        while (isalnum((unsigned char)*p) || *p == '_' || *p == '$')
        {
            p++;
        }
        // A prefixed literal (L"...", u8'x') is a literal.
        if ((*p == '"' || *p == '\'') && p - b <= 2)
        {
            char q = *p++;
            while (*p && *p != q && *p != '\n')
            {
                p += (*p == '\\' && p[1]) ? 2 : 1;
            }
            if (*p == q)
            {
                p++;
            }
            t.kind = CT_OTHER;
        }
        else
        {
            t.kind = CT_IDENT;
            t.s = b;
            t.n = (size_t)(p - b);
        }
    }
    else if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1])))
    {
        while (isalnum((unsigned char)*p) || *p == '.' || *p == '_' ||
               ((*p == '+' || *p == '-') &&
                (p[-1] == 'e' || p[-1] == 'E' || p[-1] == 'p' || p[-1] == 'P')))
        {
            p++;
        }
        t.kind = CT_OTHER;
    }
    else if (*p == '"' || *p == '\'')
    {
        char q = *p++;
        while (*p && *p != q && *p != '\n')
        {
            p += (*p == '\\' && p[1]) ? 2 : 1;
        }
        if (*p == q)
        {
            p++;
        }
        t.kind = CT_OTHER;
    }
    else
    {
        t.kind = CT_PUNCT;
        t.c = *p++;
    }
    sc->p = p;
    return t;
}

static int c_is(const CTok *t, const char *word)
{
    return t->kind == CT_IDENT && t->n == strlen(word) && memcmp(t->s, word, t->n) == 0;
}

static int c_is_any(const CTok *t, const char *const *words)
{
    for (; *words; words++)
    {
        if (c_is(t, *words))
        {
            return 1;
        }
    }
    return 0;
}

// Words that are never a declared name.
static const char *const c_specifiers[] = {"void",
                                           "char",
                                           "short",
                                           "int",
                                           "long",
                                           "float",
                                           "double",
                                           "signed",
                                           "unsigned",
                                           "__signed__",
                                           "__signed",
                                           "_Bool",
                                           "bool",
                                           "_Complex",
                                           "__complex__",
                                           "_Imaginary",
                                           "__int128",
                                           "__int128_t",
                                           "__uint128_t",
                                           "_Float16",
                                           "_Float32",
                                           "_Float64",
                                           "_Float128",
                                           "_Float32x",
                                           "_Float64x",
                                           "__float128",
                                           "__float80",
                                           "__fp16",
                                           "_Decimal32",
                                           "_Decimal64",
                                           "_Decimal128",
                                           "auto",
                                           "register",
                                           "static",
                                           "extern",
                                           "inline",
                                           "__inline",
                                           "__inline__",
                                           "_Noreturn",
                                           "noreturn",
                                           "typedef",
                                           "const",
                                           "__const",
                                           "__const__",
                                           "volatile",
                                           "__volatile__",
                                           "restrict",
                                           "__restrict",
                                           "__restrict__",
                                           "_Atomic",
                                           "_Thread_local",
                                           "__thread",
                                           "thread_local",
                                           "__extension__",
                                           "_Nullable",
                                           "_Nonnull",
                                           "_Null_unspecified",
                                           "__unaligned",
                                           "__w64",
                                           "__ptr32",
                                           "__ptr64",
                                           "__cdecl",
                                           "__stdcall",
                                           "__fastcall",
                                           NULL};

// Words followed by a parenthesised part to skip.
static const char *const c_wrappers[] = {
    "__attribute__", "__attribute", "__declspec", "_Alignas", "alignas", "__asm__",
    "__asm",         "asm",         "__typeof__", "__typeof", "typeof",  "typeof_unqual",
    "_Generic",      "__alignof__", "_Alignof",   NULL};

// ---- Declarations ---------------------------------------------------------------------------

#define C_MAX_TOKS 4096

typedef struct
{
    CTok t[C_MAX_TOKS];
    int n;
    int truncated;
} CDecl;

// Index of the token after the parenthesised part starting at t[i] (a '(' ), or n.
static int c_skip_parens(const CDecl *d, int i)
{
    int depth = 0;
    for (; i < d->n; i++)
    {
        if (d->t[i].kind != CT_PUNCT)
        {
            continue;
        }
        if (d->t[i].c == '(')
        {
            depth++;
        }
        else if (d->t[i].c == ')' && --depth == 0)
        {
            return i + 1;
        }
    }
    return d->n;
}

// Index of the token after the braced part starting at t[i] (a '{'), or n.
static int c_skip_braces(const CDecl *d, int i)
{
    int depth = 0;
    for (; i < d->n; i++)
    {
        if (d->t[i].kind != CT_PUNCT)
        {
            continue;
        }
        if (d->t[i].c == '{')
        {
            depth++;
        }
        else if (d->t[i].c == '}' && --depth == 0)
        {
            return i + 1;
        }
    }
    return d->n;
}

// Tags and enum constants anywhere in the declaration (a nested struct's too: C puts
// them all at file scope).
static void c_scan_tags(ParserContext *ctx, const CDecl *d)
{
    for (int i = 0; i < d->n; i++)
    {
        int is_enum = c_is(&d->t[i], "enum");
        if (!is_enum && !c_is(&d->t[i], "struct") && !c_is(&d->t[i], "union"))
        {
            continue;
        }
        int j = i + 1;
        while (j < d->n && c_is_any(&d->t[j], c_wrappers))
        {
            j = j + 1 < d->n && d->t[j + 1].kind == CT_PUNCT && d->t[j + 1].c == '('
                    ? c_skip_parens(d, j + 1)
                    : j + 1;
        }
        if (j < d->n && d->t[j].kind == CT_IDENT && !c_is_any(&d->t[j], c_specifiers))
        {
            c_name_add(ctx, d->t[j].s, d->t[j].n);
            j++;
        }
        if (is_enum && j < d->n && d->t[j].kind == CT_PUNCT && d->t[j].c == ':')
        {
            while (j < d->n && !(d->t[j].kind == CT_PUNCT && d->t[j].c == '{'))
            {
                j++; // enum E : int { ... } (C23)
            }
        }
        if (!is_enum || j >= d->n || d->t[j].kind != CT_PUNCT || d->t[j].c != '{')
        {
            continue;
        }
        // Each enumerator: the identifier after '{' or ',' at the body's own depth.
        int end = c_skip_braces(d, j), depth = 0, expect = 1;
        for (int k = j + 1; k < end - 1; k++)
        {
            const CTok *t = &d->t[k];
            if (t->kind == CT_PUNCT)
            {
                if (t->c == '(' || t->c == '[' || t->c == '{')
                {
                    depth++;
                }
                else if (t->c == ')' || t->c == ']' || t->c == '}')
                {
                    depth--;
                }
                else if (t->c == ',' && depth == 0)
                {
                    expect = 1;
                }
                continue;
            }
            if (expect && depth == 0 && t->kind == CT_IDENT && !c_is_any(t, c_wrappers))
            {
                c_name_add(ctx, t->s, t->n);
            }
            if (!c_is_any(t, c_wrappers))
            {
                expect = 0;
            }
        }
    }
}

// The name a declarator declares: its first identifier that isn't a qualifier or an
// attribute (`*const p`, `(*fp)(int)`, `f(int x)`, `a[N]`), before any initializer.
static void c_scan_declarator(ParserContext *ctx, const CDecl *d, int i, int end)
{
    while (i < end)
    {
        const CTok *t = &d->t[i];
        if (t->kind == CT_PUNCT && (t->c == '=' || t->c == ':' || t->c == '{'))
        {
            return;
        }
        if (t->kind == CT_IDENT)
        {
            if (c_is_any(t, c_wrappers))
            {
                i = i + 1 < end && d->t[i + 1].kind == CT_PUNCT && d->t[i + 1].c == '('
                        ? c_skip_parens(d, i + 1)
                        : i + 1;
                continue;
            }
            if (!c_is_any(t, c_specifiers))
            {
                c_name_add(ctx, t->s, t->n);
                return;
            }
        }
        i++;
    }
}

static void c_scan_decl(ParserContext *ctx, const CDecl *d)
{
    if (d->n == 0 || d->truncated)
    {
        return;
    }
    if (c_is(&d->t[0], "_Static_assert") || c_is(&d->t[0], "static_assert"))
    {
        return;
    }
    c_scan_tags(ctx, d);
    // The specifiers: keywords, a struct/union/enum with its tag and body, wrapped parts,
    // and one typedef name if no other type has been named.
    int i = 0, have_type = 0;
    while (i < d->n)
    {
        const CTok *t = &d->t[i];
        if (t->kind == CT_IDENT && c_is_any(t, c_wrappers))
        {
            i = i + 1 < d->n && d->t[i + 1].kind == CT_PUNCT && d->t[i + 1].c == '('
                    ? c_skip_parens(d, i + 1)
                    : i + 1;
            continue;
        }
        if (t->kind == CT_IDENT && c_is_any(t, c_specifiers))
        {
            if (!c_is(t, "const") && !c_is(t, "volatile") && !c_is(t, "static") &&
                !c_is(t, "extern") && !c_is(t, "typedef") && !c_is(t, "inline") &&
                !c_is(t, "__extension__") && !c_is(t, "register"))
            {
                have_type = 1;
            }
            i++;
            continue;
        }
        if (c_is(t, "struct") || c_is(t, "union") || c_is(t, "enum"))
        {
            have_type = 1;
            i++;
            while (i < d->n && c_is_any(&d->t[i], c_wrappers))
            {
                i = i + 1 < d->n && d->t[i + 1].kind == CT_PUNCT && d->t[i + 1].c == '('
                        ? c_skip_parens(d, i + 1)
                        : i + 1;
            }
            if (i < d->n && d->t[i].kind == CT_IDENT)
            {
                i++;
            }
            if (i < d->n && d->t[i].kind == CT_PUNCT && d->t[i].c == ':')
            {
                while (i < d->n && !(d->t[i].kind == CT_PUNCT && d->t[i].c == '{'))
                {
                    i++;
                }
            }
            if (i < d->n && d->t[i].kind == CT_PUNCT && d->t[i].c == '{')
            {
                i = c_skip_braces(d, i);
            }
            continue;
        }
        if (t->kind == CT_IDENT && !have_type && i + 1 < d->n &&
            !(d->t[i + 1].kind == CT_PUNCT && (d->t[i + 1].c == '(' || d->t[i + 1].c == ';' ||
                                               d->t[i + 1].c == ',' || d->t[i + 1].c == '=')))
        {
            have_type = 1; // a typedef name: size_t n
            i++;
            continue;
        }
        break;
    }
    // The declarators, split at the top-level commas.
    int start = i, depth = 0;
    for (int k = i; k <= d->n; k++)
    {
        int at_end = k == d->n;
        const CTok *t = at_end ? NULL : &d->t[k];
        if (t && t->kind == CT_PUNCT)
        {
            if (t->c == '(' || t->c == '[' || t->c == '{')
            {
                depth++;
            }
            else if (t->c == ')' || t->c == ']' || t->c == '}')
            {
                depth--;
            }
        }
        if (at_end || (t->kind == CT_PUNCT && t->c == ',' && depth == 0))
        {
            c_scan_declarator(ctx, d, start, k);
            start = k + 1;
        }
    }
}

static void c_scan_text(ParserContext *ctx, const char *text)
{
    CScan sc = {ctx, text, 1};
    CDecl *d = xcalloc(1, sizeof(CDecl));
    int paren = 0, brace = 0;
    CTok prev = {CT_EOF, NULL, 0, 0};
    for (;;)
    {
        CTok t = c_next(&sc);
        if (t.kind == CT_EOF)
        {
            break;
        }
        if (t.kind == CT_PUNCT)
        {
            if (t.c == '(')
            {
                paren++;
            }
            else if (t.c == ')')
            {
                paren = paren > 0 ? paren - 1 : 0;
            }
            else if (t.c == ';' && paren == 0 && brace == 0)
            {
                c_scan_decl(ctx, d);
                d->n = 0;
                d->truncated = 0;
                prev = t;
                continue;
            }
            else if (t.c == '{' && paren == 0 && brace == 0 && prev.kind == CT_PUNCT &&
                     prev.c == ')')
            {
                // A function's body (an inline function in a header): its name, then the
                // body skipped. (A struct's body follows its tag or the keyword instead.)
                int has_eq = 0;
                for (int k = 0; k < d->n; k++)
                {
                    has_eq |= d->t[k].kind == CT_PUNCT && d->t[k].c == '=';
                }
                if (!has_eq)
                {
                    c_scan_decl(ctx, d);
                    d->n = 0;
                    d->truncated = 0;
                    int depth = 1;
                    while (depth > 0)
                    {
                        CTok b = c_next(&sc);
                        if (b.kind == CT_EOF)
                        {
                            break;
                        }
                        if (b.kind == CT_PUNCT && b.c == '{')
                        {
                            depth++;
                        }
                        else if (b.kind == CT_PUNCT && b.c == '}')
                        {
                            depth--;
                        }
                    }
                    prev.kind = CT_PUNCT;
                    prev.c = '}';
                    continue;
                }
            }
            if (t.c == '{')
            {
                brace++;
            }
            else if (t.c == '}')
            {
                brace = brace > 0 ? brace - 1 : 0;
            }
        }
        if (d->n < C_MAX_TOKS)
        {
            d->t[d->n++] = t;
        }
        else
        {
            d->truncated = 1;
        }
        prev = t;
    }
    if (d->n)
    {
        c_scan_decl(ctx, d);
    }
    zfree(d);
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
    fseek(f, 0, SEEK_SET);
    char *buf = n >= 0 ? xmalloc((size_t)n + 1) : NULL;
    size_t got = buf ? fread(buf, 1, (size_t)n, f) : 0;
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

// Loads the names of `path` if every file it lists is unchanged.
static int c_cache_load(ParserContext *ctx, const char *path)
{
    char *text = c_read_file(path);
    if (!text)
    {
        return 0;
    }
    if (strncmp(text, "zc-c-names 2\n", 13) != 0)
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
    for (p += 4; *p;)
    {
        char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        c_name_add(ctx, p, n);
        p += n + (nl ? 1 : 0);
    }
    zfree(text);
    return 1;
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
    fputs("zc-c-names 2\n", f);
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
    for (unsigned i = 0; i < ctx->c_names_cap; i++)
    {
        if (ctx->c_names[i])
        {
            fprintf(f, "%s\n", ctx->c_names[i]);
        }
    }
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
    if (!ctx->compiler || cfg->misra_mode || cfg->mode_lsp || cfg->mode_doc || !cfg->cc[0] ||
        (off && *off && strcmp(off, "0") != 0))
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
    c_scan_text(ctx, text);
    ctx->c_names_ready = ctx->c_names_count > 0;
    if (ctx->c_names_ready && have_dir)
    {
        c_cache_save(ctx, cached, text);
    }
    zfree(text);
}
