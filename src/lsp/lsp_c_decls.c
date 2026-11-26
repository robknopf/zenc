// SPDX-License-Identifier: MIT
#include "lsp_c_decls.h"
#include "lsp_project.h"
#include "../parser/utils/c_decls.h"
#include "../codegen/codegen.h"
#include "../utils/cJSON.h"
#include "../utils/utils.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LSP_C_COMPLETION_MAX 300

static unsigned long long hash_text(unsigned long long h, const char *s, size_t n)
{
    for (size_t i = 0; s && i < n; i++)
    {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

void lsp_c_decls_update(ProjectFile *pf)
{
    if (!g_project || !g_project->ctx || !pf || !pf->ast || !g_project->ctx->compiler)
    {
        return;
    }
    ParserContext *ctx = g_project->ctx;
    // What the C is read from: the includes and raw blocks, and the document's build
    // directives (include directories, defines, pkg-config). Read again only when they
    // change (also when they couldn't be read: a header not found isn't looked for at
    // every keystroke).
    char *unit = codegen_c_names_unit(ctx, pf->ast);
    unsigned long long key = hash_text(14695981039346656037ULL, unit, unit ? strlen(unit) : 0);
    for (const char *p = pf->source; p && *p;)
    {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        const char *q = p;
        while (q < p + n && (*q == ' ' || *q == '\t'))
        {
            q++;
        }
        if (strncmp(q, "//>", 3) == 0)
        {
            key = hash_text(key, p, n + 1);
        }
        p = nl ? nl + 1 : NULL;
    }
    key |= 1; // (0: never read)
    if (pf->c_names.unit_key == key)
    {
        lsp_c_decls_use(pf);
        return;
    }
    // The directives apply to this document's C as they do in a build of it, and to
    // nothing else: the compiler's state (flags, include directories, defines) is put back.
    ZenCompiler saved = *ctx->compiler;
    scan_build_directives_at(ctx, pf->source, pf->path);
    c_names_scan_into(ctx, pf->ast, &pf->c_names);
    *ctx->compiler = saved;
    pf->c_names.unit_key = key;
}

void lsp_c_decls_use(ProjectFile *pf)
{
    if (g_project && g_project->ctx)
    {
        c_names_load(g_project->ctx, pf ? &pf->c_names : NULL);
    }
}

static int is_ident(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

// The start of line `line` (0-based) of `source`, or NULL.
static const char *line_start(const char *source, int line)
{
    const char *p = source;
    for (int i = 0; p && i < line; i++)
    {
        p = strchr(p, '\n');
        p = p ? p + 1 : NULL;
    }
    return p;
}

// The identifier at or just before (line, col) of `source` into `out`; its start column in
// *start. 0 if there is none.
static int word_at(const char *source, int line, int col, char *out, size_t n, int *start,
                   int up_to_cursor)
{
    const char *ls = source ? line_start(source, line) : NULL;
    if (!ls)
    {
        return 0;
    }
    int len = 0;
    while (ls[len] && ls[len] != '\n')
    {
        len++;
    }
    if (col > len)
    {
        col = len;
    }
    int b = col, e = col;
    while (b > 0 && is_ident(ls[b - 1]))
    {
        b--;
    }
    if (!up_to_cursor)
    {
        while (e < len && is_ident(ls[e]))
        {
            e++;
        }
    }
    if (e <= b || (size_t)(e - b) >= n || isdigit((unsigned char)ls[b]))
    {
        return 0;
    }
    memcpy(out, ls + b, (size_t)(e - b));
    out[e - b] = 0;
    if (start)
    {
        *start = b;
    }
    // `struct NAME`: the tag.
    int k = b;
    while (k > 0 && (ls[k - 1] == ' ' || ls[k - 1] == '\t'))
    {
        k--;
    }
    static const char *const tags[] = {"struct", "union", "enum", NULL};
    for (int i = 0; tags[i]; i++)
    {
        int tl = (int)strlen(tags[i]);
        if (k >= tl && k < b && strncmp(ls + k - tl, tags[i], (size_t)tl) == 0 &&
            (k == tl || !is_ident(ls[k - tl - 1])) && strlen(out) + (size_t)tl + 1 < n)
        {
            char tmp[MAX_VAR_NAME_LEN];
            snprintf(tmp, sizeof(tmp), "%s %s", tags[i], out);
            snprintf(out, n, "%s", tmp);
        }
    }
    return 1;
}

// Line `line` (0-based) of the file at `path` into `out`; 0 if it can't be read.
static int read_line(const char *path, int line, char *out, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        return 0;
    }
    int cur = 0;
    int ok = 0;
    while (fgets(out, (int)n, f))
    {
        size_t len = strlen(out);
        int whole = len > 0 && out[len - 1] == '\n';
        if (cur == line)
        {
            ok = 1;
            break;
        }
        if (whole)
        {
            cur++;
        }
    }
    fclose(f);
    return ok;
}

const CHeaderSymbol *lsp_c_decls_symbol_at(ProjectFile *pf, int line, int col)
{
    static CHeaderSymbol symbol;
    static char path[PATH_MAX];
    if (!pf || !pf->c_names.ready)
    {
        return NULL;
    }
    lsp_c_decls_use(pf);
    char word[MAX_VAR_NAME_LEN];
    if (!word_at(pf->source, line, col, word, sizeof(word), NULL, 0))
    {
        return NULL;
    }
    const CDecl *d = c_decl_find(g_project->ctx, word);
    if (!d || !d->file || d->file[0] == '<' || d->line <= 0)
    {
        return NULL;
    }
    if (!realpath(d->file, path))
    {
        snprintf(path, sizeof(path), "%s", d->file);
    }
    memset(&symbol, 0, sizeof(symbol));
    symbol.path = path;
    symbol.line = d->line - 1;
    symbol.start_line = d->line - 1;
    symbol.end_line = (d->end_line >= d->line ? d->end_line : d->line) - 1;
    // The name's column: where it is on one of the declaration's lines, as a word.
    const char *name = d->name;
    size_t nl = strlen(name);
    char text[4096];
    for (int l = symbol.start_line; l <= symbol.end_line; l++)
    {
        if (!read_line(path, l, text, sizeof(text)))
        {
            break;
        }
        for (const char *p = strstr(text, name); p; p = strstr(p + 1, name))
        {
            if ((p == text || !is_ident(p[-1])) && !is_ident(p[nl]))
            {
                symbol.line = l;
                symbol.column = (int)(p - text);
                symbol.length = (int)nl;
                return &symbol;
            }
        }
    }
    symbol.length = (int)nl;
    return &symbol;
}

static int completion_kind(const CDecl *d)
{
    switch (d->kind)
    {
    case CD_FUNCTION:
        return 3; // Function
    case CD_VARIABLE:
        return 6; // Variable
    case CD_TYPEDEF:
        return 22; // Struct (as lsp_c_headers.c gives a typedef)
    case CD_ENUM_CONSTANT:
        return 20; // EnumMember
    case CD_MACRO:
        return 21; // Constant
    default:
        return 22;
    }
}

static int starts_with_nocase(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++)
    {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
        {
            return 0;
        }
    }
    return 1;
}

int lsp_c_decls_complete(ProjectFile *pf, int line, int col, cJSON *items)
{
    if (!pf || !pf->c_names.ready)
    {
        return 0;
    }
    lsp_c_decls_use(pf);
    char prefix[MAX_VAR_NAME_LEN];
    if (!word_at(pf->source, line, col, prefix, sizeof(prefix), NULL, 1) || strchr(prefix, ' '))
    {
        return 0; // nothing typed yet: thousands of names say nothing
    }
    unsigned n = 0;
    const CDecl *const *all = c_decl_all(g_project->ctx, &n);
    int added = 0, more = 0;
    for (unsigned i = 0; i < n; i++)
    {
        const CDecl *d = all[i];
        if (d->kind == CD_STRUCT || d->kind == CD_UNION || d->kind == CD_ENUM)
        {
            continue; // tags are written after struct/union/enum
        }
        if ((d->name[0] == '_' && prefix[0] != '_') || !starts_with_nocase(d->name, prefix))
        {
            continue;
        }
        if (added == LSP_C_COMPLETION_MAX)
        {
            more = 1;
            break;
        }
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "label", d->name);
        cJSON_AddNumberToObject(item, "kind", completion_kind(d));
        const char *detail = d->kind == CD_FUNCTION || d->kind == CD_MACRO ? d->text : d->type;
        if (detail)
        {
            cJSON_AddStringToObject(item, "detail", detail);
        }
        cJSON_AddStringToObject(item, "sortText", "080");
        if (d->kind == CD_FUNCTION)
        {
            char snippet[MAX_VAR_NAME_LEN + 8];
            snprintf(snippet, sizeof(snippet), d->nparams > 0 || d->variadic ? "%s($1)" : "%s()",
                     d->name);
            cJSON_AddStringToObject(item, "insertText", snippet);
            cJSON_AddNumberToObject(item, "insertTextFormat", 2); // Snippet
        }
        cJSON_AddItemToArray(items, item);
        added++;
    }
    return more;
}

int lsp_c_decls_fields(ProjectFile *pf, const char *type, cJSON *items)
{
    if (!pf || !pf->c_names.ready || !type)
    {
        return 0;
    }
    lsp_c_decls_use(pf);
    char base[MAX_TYPE_NAME_LEN];
    snprintf(base, sizeof(base), "%s", type);
    char *star = strchr(base, '*');
    if (star)
    {
        *star = 0;
    }
    const CDecl *agg = c_decl_aggregate(g_project->ctx, base);
    if (!agg)
    {
        return 0;
    }
    for (int i = 0; i < agg->nfields; i++)
    {
        if (!agg->fields[i].name)
        {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "label", agg->fields[i].name);
        cJSON_AddNumberToObject(item, "kind", 5); // Field
        cJSON_AddStringToObject(item, "detail", agg->fields[i].type);
        cJSON_AddItemToArray(items, item);
    }
    return 1;
}

// ---- Diagnostics ----------------------------------------------------------------------------

static void add_diagnostic(cJSON *diagnostics, Token t, const char *message)
{
    if (t.line <= 0)
    {
        return;
    }
    cJSON *d = cJSON_CreateObject();
    cJSON *range = cJSON_AddObjectToObject(d, "range");
    cJSON *start = cJSON_AddObjectToObject(range, "start");
    cJSON_AddNumberToObject(start, "line", t.line - 1);
    cJSON_AddNumberToObject(start, "character", t.col > 0 ? t.col - 1 : 0);
    cJSON *end = cJSON_AddObjectToObject(range, "end");
    cJSON_AddNumberToObject(end, "line", t.line - 1);
    int first = t.col > 0 ? t.col - 1 : 0;
    int width = t.len > 0 ? (int)t.len : 1;
    cJSON_AddNumberToObject(end, "character", first + width);
    cJSON_AddNumberToObject(d, "severity", 1);
    cJSON_AddStringToObject(d, "source", "zc");
    cJSON_AddStringToObject(d, "message", message);
    cJSON_AddItemToArray(diagnostics, d);
}

// A call to `name` is to the C function: the Zen C declares nothing of that name.
static const CDecl *c_function(ParserContext *ctx, const char *name)
{
    const CDecl *d = c_decl_find(ctx, name);
    if (!d || d->kind != CD_FUNCTION || find_func(ctx, name) || find_struct_def(ctx, name) ||
        find_symbol_in_all(ctx, name) || find_enum_variant(ctx, name))
    {
        return NULL;
    }
    return d;
}

static void check_call(ParserContext *ctx, ASTNode *call, cJSON *diagnostics)
{
    ASTNode *callee = call->call.callee;
    if (!callee || callee->kind != NODE_EXPR_VAR)
    {
        return;
    }
    const CDecl *d = c_function(ctx, callee->var_ref.name);
    if (!d)
    {
        return;
    }
    int given = 0;
    for (ASTNode *a = call->call.args; a; a = a->next)
    {
        given++;
    }
    char msg[1024];
    if (d->params_known && (d->variadic ? given < d->nparams : given != d->nparams))
    {
        snprintf(msg, sizeof(msg), "'%s' takes %s%d argument%s, but %d %s given\n%s", d->name,
                 d->variadic ? "at least " : "", d->nparams, d->nparams == 1 ? "" : "s", given,
                 given == 1 ? "was" : "were", d->text ? d->text : "");
        add_diagnostic(diagnostics, callee->token, msg);
        return;
    }
    int i = 0;
    for (ASTNode *a = call->call.args; a && i < d->nparams; a = a->next, i++)
    {
        const char *want = c_decl_enum_of(ctx, d->params[i].type);
        if (!want || a->kind != NODE_EXPR_VAR)
        {
            continue; // (only a constant: the LSP has no types for locals)
        }
        const CDecl *c = c_decl_find(ctx, a->var_ref.name);
        if (!c || c->kind != CD_ENUM_CONSTANT || find_symbol_in_all(ctx, a->var_ref.name))
        {
            continue;
        }
        const char *have = c_decl_enum_of(ctx, c->type);
        if (have && strcmp(have, want) != 0)
        {
            snprintf(msg, sizeof(msg),
                     "Argument %d of '%s' is a '%s', but the parameter is a '%s'\n%s", i + 1,
                     d->name, have, want, d->text ? d->text : "");
            add_diagnostic(diagnostics, a->token, msg);
        }
    }
}

static void walk(ParserContext *ctx, ASTNode *node, cJSON *diagnostics, int depth);

// Whether undefined names are reported: not in a document with a comptime block at its top,
// which can declare any name (`yield("def X = 1;")`) and which the server doesn't run.
static int g_report_undefined;

static void walk_list(ParserContext *ctx, ASTNode *node, cJSON *diagnostics, int depth)
{
    for (; node; node = node->next)
    {
        walk(ctx, node, diagnostics, depth);
    }
}

// "Undefined function 'x'" / "Undefined name 'X'", as a build says it.
static void report_undefined(ParserContext *ctx, ASTNode *ref, const char *what, cJSON *diagnostics)
{
    const char *close = c_name_closest(ctx, ref->var_ref.name);
    char msg[1024];
    if (close)
    {
        snprintf(msg, sizeof(msg), "Undefined %s '%s'\nDid you mean '%s'?", what, ref->var_ref.name,
                 close);
    }
    else
    {
        snprintf(msg, sizeof(msg),
                 "Undefined %s '%s'\nNot declared here, nor by an included C header", what,
                 ref->var_ref.name);
    }
    add_diagnostic(diagnostics, ref->token, msg);
}

static void walk(ParserContext *ctx, ASTNode *node, cJSON *diagnostics, int depth)
{
    // (an imported module's code is its own document's: its diagnostics are given there)
    if (!node || depth > 512 || node->kind == NODE_COMPTIME || node->kind == NODE_IMPORT)
    {
        return;
    }
    if (node->kind == NODE_EXPR_CALL && node->call.callee &&
        node->call.callee->kind == NODE_EXPR_VAR)
    {
        // The check a build makes, with what was in scope where each name was parsed.
        if (g_report_undefined && !c_names_call_known_parsed(ctx, node->call.callee))
        {
            report_undefined(ctx, node->call.callee, "function", diagnostics);
        }
        else
        {
            check_call(ctx, node, diagnostics);
        }
        walk_list(ctx, node->call.args, diagnostics, depth + 1);
        return;
    }
    if (node->kind == NODE_EXPR_VAR)
    {
        if (g_report_undefined && c_names_name_unknown_parsed(ctx, node))
        {
            report_undefined(ctx, node, "name", diagnostics);
        }
        return;
    }
    if (node->kind == NODE_RAW_STMT)
    {
        // (an interpolated string's expressions: the C gets them as text)
        for (int i = 0; i < node->raw_stmt.interp_expr_count; i++)
        {
            walk(ctx, node->raw_stmt.interp_exprs[i], diagnostics, depth + 1);
        }
    }
    ASTChildren ch = ast_children(node);
    for (int i = 0; i < ch.count; i++)
    {
        walk_list(ctx, *ch.slots[i], diagnostics, depth + 1);
    }
}

void lsp_c_decls_add_diagnostics(ProjectFile *pf, cJSON *diagnostics)
{
    if (!pf || !pf->ast || !pf->c_names.ready || !diagnostics)
    {
        return;
    }
    lsp_c_decls_use(pf);
    g_report_undefined = 1;
    ASTNode *top = pf->ast->kind == NODE_ROOT ? pf->ast->root.children : pf->ast;
    for (ASTNode *n = top; n; n = n->next)
    {
        if (n->kind == NODE_COMPTIME)
        {
            g_report_undefined = 0;
        }
    }
    walk_list(g_project->ctx, pf->ast, diagnostics, 0);
}
