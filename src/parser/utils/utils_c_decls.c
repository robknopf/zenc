// SPDX-License-Identifier: MIT
//
// The declarations of the C a program includes (c_decls.h), read with tree-sitter's C
// grammar from the C preprocessor's output.
//
// The preprocessor's output has two kinds of line: its own (# 12 "file.h" line markers, and
// the #defines kept by -dD) and C. The # lines are read here, for the macros and to know
// which header and line each piece of C comes from; tree-sitter gets the C with those lines
// blanked, so its rows are the output's lines. A #define can fall in the middle of a
// declaration (glibc writes one after each enum constant), which the grammar doesn't take.
//
// What the grammar can't parse (glibc's unnamed bit-fields, say) it recovers from locally:
// a struct with an error in it gets no field list (complete = 0) rather than a wrong one.

#include "../parser.h"
#include "c_decls.h"
#include <tree_sitter/api.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

const TSLanguage *tree_sitter_c(void);

// ---- The table ------------------------------------------------------------------------------

typedef struct
{
    CDecl **slots; // by key: the name, or "struct NAME" for a tag
    unsigned cap, count;
    CDecl **all; // in the order read
    unsigned nall, all_cap;
} CDeclTable;

static unsigned cd_hash(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++)
    {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

static const char *cd_key(const CDecl *d, char *buf, size_t n)
{
    const char *tag = d->kind == CD_STRUCT  ? "struct "
                      : d->kind == CD_UNION ? "union "
                      : d->kind == CD_ENUM  ? "enum "
                                            : NULL;
    if (!tag)
    {
        return d->name;
    }
    snprintf(buf, n, "%s%s", tag, d->name);
    return buf;
}

static unsigned cd_slot(CDecl **slots, unsigned cap, const char *key)
{
    unsigned i = cd_hash(key) & (cap - 1);
    char buf[512];
    while (slots[i] && strcmp(cd_key(slots[i], buf, sizeof buf), key) != 0)
    {
        i = (i + 1) & (cap - 1);
    }
    return i;
}

static CDeclTable *cd_table(ParserContext *ctx)
{
    if (!ctx->c_decls)
    {
        ctx->c_decls = xcalloc(1, sizeof(CDeclTable));
    }
    return (CDeclTable *)ctx->c_decls;
}

// Adds `d` (owned by the table from now on). The first declaration of a name is kept, but a
// struct's definition replaces a forward declaration, and a function's definition one
// without parameters.
static void cd_put(ParserContext *ctx, CDecl *d)
{
    CDeclTable *t = cd_table(ctx);
    if ((t->count + 1) * 2 > t->cap)
    {
        unsigned cap = t->cap ? t->cap * 2 : 4096;
        CDecl **slots = xcalloc(cap, sizeof(CDecl *));
        char buf[512];
        for (unsigned i = 0; i < t->cap; i++)
        {
            if (t->slots[i])
            {
                slots[cd_slot(slots, cap, cd_key(t->slots[i], buf, sizeof buf))] = t->slots[i];
            }
        }
        zfree(t->slots);
        t->slots = slots;
        t->cap = cap;
    }
    char buf[512];
    unsigned i = cd_slot(t->slots, t->cap, cd_key(d, buf, sizeof buf));
    CDecl *was = t->slots[i];
    if (was)
    {
        int better = (d->kind == CD_STRUCT || d->kind == CD_UNION) && d->complete && !was->complete;
        better |= d->kind == CD_FUNCTION && d->params_known && !was->params_known;
        if (!better)
        {
            return;
        }
        t->slots[i] = d;
        for (unsigned k = 0; k < t->nall; k++)
        {
            if (t->all[k] == was)
            {
                t->all[k] = d;
            }
        }
        return;
    }
    t->slots[i] = d;
    t->count++;
    if (t->nall == t->all_cap)
    {
        t->all_cap = t->all_cap ? t->all_cap * 2 : 1024;
        t->all = xrealloc(t->all, t->all_cap * sizeof(CDecl *));
    }
    t->all[t->nall++] = d;
}

const CDecl *c_decl_find(ParserContext *ctx, const char *name)
{
    CDeclTable *t = (CDeclTable *)ctx->c_decls;
    if (!t || !t->slots || !name)
    {
        return NULL;
    }
    return t->slots[cd_slot(t->slots, t->cap, name)];
}

const CDecl *const *c_decl_all(ParserContext *ctx, unsigned *n)
{
    CDeclTable *t = (CDeclTable *)ctx->c_decls;
    *n = t ? t->nall : 0;
    return t ? (const CDecl *const *)t->all : NULL;
}

static char *cd_dup(const char *s)
{
    return s ? xstrdup(s) : NULL;
}

void c_decl_add(ParserContext *ctx, const CDecl *src)
{
    CDecl *d = xcalloc(1, sizeof(CDecl));
    *d = *src;
    d->name = cd_dup(src->name);
    d->file = cd_dup(src->file);
    d->text = cd_dup(src->text);
    d->type = cd_dup(src->type);
    d->aggregate = cd_dup(src->aggregate);
    if (src->nparams)
    {
        d->params = xcalloc((size_t)src->nparams, sizeof(CDeclField));
        for (int i = 0; i < src->nparams; i++)
        {
            d->params[i].name = cd_dup(src->params[i].name);
            d->params[i].type = cd_dup(src->params[i].type);
        }
    }
    if (src->nfields)
    {
        d->fields = xcalloc((size_t)src->nfields, sizeof(CDeclField));
        for (int i = 0; i < src->nfields; i++)
        {
            d->fields[i].name = cd_dup(src->fields[i].name);
            d->fields[i].type = cd_dup(src->fields[i].type);
        }
    }
    cd_put(ctx, d);
}

// ---- Reading the tree -----------------------------------------------------------------------

typedef struct
{
    ParserContext *ctx;
    const char *src;
    const char **row_file; // the header each row (line) of the output comes from
    int *row_line;
    int nrows;
} CdRead;

static int cd_is(TSNode n, const char *type)
{
    return !ts_node_is_null(n) && strcmp(ts_node_type(n), type) == 0;
}

static TSNode cd_field(TSNode n, const char *field)
{
    return ts_node_child_by_field_name(n, field, (uint32_t)strlen(field));
}

// The node's text, with runs of white space made one space.
static char *cd_text(const CdRead *r, TSNode n)
{
    uint32_t b = ts_node_start_byte(n), e = ts_node_end_byte(n);
    char *out = xmalloc(e - b + 1);
    size_t k = 0;
    int space = 0;
    for (uint32_t i = b; i < e; i++)
    {
        char c = r->src[i];
        if (isspace((unsigned char)c))
        {
            space = k > 0;
            continue;
        }
        if (space)
        {
            out[k++] = ' ';
            space = 0;
        }
        out[k++] = c;
    }
    out[k] = 0;
    return out;
}

static char *cd_cat(const char *a, const char *b)
{
    size_t na = strlen(a), nb = strlen(b);
    char *s = xmalloc(na + nb + 1);
    memcpy(s, a, na);
    memcpy(s + na, b, nb + 1);
    return s;
}

// The specifiers of a declaration or field, as a type: its qualifiers and the type node,
// with a struct, union or enum given by its tag (not its body).
static char *cd_specifiers(const CdRead *r, TSNode decl)
{
    char *spec = xstrdup("");
    uint32_t n = ts_node_named_child_count(decl);
    TSNode type = cd_field(decl, "type");
    for (uint32_t i = 0; i < n; i++)
    {
        TSNode c = ts_node_named_child(decl, i);
        if (cd_is(c, "type_qualifier"))
        {
            char *q = cd_text(r, c);
            char *s = cd_cat(spec, q);
            spec = cd_cat(s, " ");
        }
    }
    if (ts_node_is_null(type))
    {
        return spec;
    }
    const char *tt = ts_node_type(type);
    if (!strcmp(tt, "struct_specifier") || !strcmp(tt, "union_specifier") ||
        !strcmp(tt, "enum_specifier"))
    {
        TSNode name = cd_field(type, "name");
        const char *kw = !strcmp(tt, "struct_specifier")  ? "struct "
                         : !strcmp(tt, "union_specifier") ? "union "
                                                          : "enum ";
        char *nm = ts_node_is_null(name) ? xstrdup("<anonymous>") : cd_text(r, name);
        return cd_cat(cd_cat(spec, kw), nm);
    }
    return cd_cat(spec, cd_text(r, type));
}

static int cd_is_declarator_leaf(TSNode n)
{
    const char *t = ts_node_type(n);
    return !strcmp(t, "identifier") || !strcmp(t, "field_identifier") ||
           !strcmp(t, "type_identifier") || !strcmp(t, "primitive_type");
}

static char *cd_params(const CdRead *r, TSNode list, CDeclField **out, int *n, int *variadic);

// The declarator `d` as an abstract one (without its name), inside out: `*a[3]` is "*[3]",
// `(*fp)(int)` is "(*)(int)". `skip` (a function declarator) contributes nothing, so the
// type left is its return type. Its name goes to *name.
static char *cd_abstract(const CdRead *r, TSNode d, TSNode skip, char **name)
{
    if (ts_node_is_null(d))
    {
        return xstrdup("");
    }
    if (cd_is_declarator_leaf(d))
    {
        if (name)
        {
            *name = cd_text(r, d);
        }
        return xstrdup("");
    }
    const char *t = ts_node_type(d);
    TSNode inner = cd_field(d, "declarator");
    if (!strcmp(t, "pointer_declarator") || !strcmp(t, "abstract_pointer_declarator"))
    {
        char *quals = xstrdup("");
        uint32_t n = ts_node_named_child_count(d);
        for (uint32_t i = 0; i < n; i++)
        {
            TSNode c = ts_node_named_child(d, i);
            if (cd_is(c, "type_qualifier"))
            {
                quals = cd_cat(cd_cat(quals, " "), cd_text(r, c));
            }
        }
        char *rest = cd_abstract(r, inner, skip, name);
        return cd_cat(cd_cat("*", quals), rest[0] && quals[0] ? cd_cat(" ", rest) : rest);
    }
    if (!strcmp(t, "array_declarator") || !strcmp(t, "abstract_array_declarator"))
    {
        TSNode size = cd_field(d, "size");
        char *rest = cd_abstract(r, inner, skip, name);
        char *dim =
            ts_node_is_null(size) ? xstrdup("[]") : cd_cat(cd_cat("[", cd_text(r, size)), "]");
        return cd_cat(rest, dim);
    }
    if (!strcmp(t, "function_declarator") || !strcmp(t, "abstract_function_declarator"))
    {
        char *rest = cd_abstract(r, inner, skip, name);
        if (ts_node_eq(d, skip))
        {
            return rest;
        }
        TSNode params = cd_field(d, "parameters");
        return cd_cat(rest, ts_node_is_null(params) ? "()" : cd_text(r, params));
    }
    if (!strcmp(t, "parenthesized_declarator") || !strcmp(t, "abstract_parenthesized_declarator"))
    {
        // (the grammar's parenthesized declarator holds its inner one as a plain child)
        TSNode c = ts_node_named_child_count(d) ? ts_node_named_child(d, 0) : inner;
        while (!ts_node_is_null(c) && cd_is(c, "attribute_specifier"))
        {
            c = ts_node_next_named_sibling(c);
        }
        char *rest = cd_abstract(r, c, skip, name);
        // A parenthesis around nothing left (the function skipped) goes too.
        return rest[0] ? cd_cat(cd_cat("(", rest), ")") : rest;
    }
    if (!strcmp(t, "init_declarator"))
    {
        return cd_abstract(r, inner, skip, name);
    }
    if (!strcmp(t, "attributed_declarator"))
    {
        TSNode c = ts_node_named_child(d, 0);
        return cd_abstract(r, c, skip, name);
    }
    return xstrdup("");
}

// The function declarator a declarator names a function with, or a null node: the one whose
// own declarator leads (through parentheses) straight to the name. `int (*fp)(int)` is a
// pointer, not a function.
static TSNode cd_function_of(TSNode d)
{
    TSNode none = {0};
    while (!ts_node_is_null(d))
    {
        const char *t = ts_node_type(d);
        if (!strcmp(t, "function_declarator"))
        {
            TSNode in = cd_field(d, "declarator");
            while (!ts_node_is_null(in) &&
                   (cd_is(in, "parenthesized_declarator") || cd_is(in, "attributed_declarator")))
            {
                in = ts_node_named_child(in, 0);
            }
            if (!ts_node_is_null(in) && cd_is_declarator_leaf(in))
            {
                return d;
            }
            d = cd_field(d, "declarator");
            continue;
        }
        if (!strcmp(t, "pointer_declarator") || !strcmp(t, "array_declarator") ||
            !strcmp(t, "init_declarator"))
        {
            d = cd_field(d, "declarator");
            continue;
        }
        if (!strcmp(t, "parenthesized_declarator") || !strcmp(t, "attributed_declarator"))
        {
            d = ts_node_named_child(d, 0);
            continue;
        }
        return none;
    }
    return none;
}

static char *cd_type_of(const char *spec, const char *abstract)
{
    if (!abstract[0])
    {
        return xstrdup(spec);
    }
    return cd_cat(cd_cat(spec, " "), abstract);
}

static char *cd_params(const CdRead *r, TSNode list, CDeclField **out, int *n, int *variadic)
{
    *out = NULL;
    *n = 0;
    *variadic = 0;
    uint32_t count = ts_node_named_child_count(list);
    CDeclField *p = count ? xcalloc(count, sizeof(CDeclField)) : NULL;
    for (uint32_t i = 0; i < count; i++)
    {
        TSNode c = ts_node_named_child(list, i);
        if (cd_is(c, "variadic_parameter"))
        {
            *variadic = 1;
            continue;
        }
        if (!cd_is(c, "parameter_declaration"))
        {
            continue;
        }
        char *spec = cd_specifiers(r, c);
        TSNode d = cd_field(c, "declarator");
        char *name = NULL;
        char *abs = cd_abstract(r, d, (TSNode){0}, &name);
        if (!strcmp(spec, "void") && !abs[0] && !name && count == 1)
        {
            break; // (void): none
        }
        p[*n].name = name;
        p[*n].type = cd_type_of(spec, abs);
        (*n)++;
    }
    *out = p;
    return NULL;
}

static void cd_locate(const CdRead *r, TSNode n, CDecl *d)
{
    uint32_t row = ts_node_start_point(n).row;
    if ((int)row < r->nrows)
    {
        d->file = r->row_file[row] ? xstrdup(r->row_file[row]) : NULL;
        d->line = r->row_line[row];
        d->end_line = d->line;
        uint32_t end = ts_node_end_point(n).row;
        if ((int)end < r->nrows && r->row_file[end] == r->row_file[row] &&
            r->row_line[end] >= d->line)
        {
            d->end_line = r->row_line[end];
        }
    }
}

static void cd_name(const CdRead *r, const char *name)
{
    c_name_note(r->ctx, name, strlen(name));
}

static void cd_aggregate(const CdRead *r, TSNode spec, CDecl *into);
static char *cd_shown(const CdRead *r, TSNode n);

// The fields of a struct or union body into `d` (anonymous members flattened in).
static void cd_fields(const CdRead *r, TSNode body, CDecl *d, int depth)
{
    uint32_t n = ts_node_named_child_count(body);
    for (uint32_t i = 0; i < n && depth < 16; i++)
    {
        TSNode f = ts_node_named_child(body, i);
        if (!cd_is(f, "field_declaration"))
        {
            continue;
        }
        TSNode type = cd_field(f, "type");
        cd_aggregate(r, type, NULL); // a tag or enum declared inside is at file scope too
        char *spec = cd_specifiers(r, f);
        uint32_t m = ts_node_named_child_count(f);
        int declarators = 0;
        for (uint32_t k = 0; k < m; k++)
        {
            TSNode c = ts_node_named_child(f, k);
            const char *ft = ts_node_type(c);
            if (!strcmp(ft, "field_identifier") || strstr(ft, "declarator"))
            {
                if (ts_node_eq(c, type))
                {
                    continue;
                }
                char *name = NULL;
                char *abs = cd_abstract(r, c, (TSNode){0}, &name);
                if (!name)
                {
                    continue;
                }
                declarators++;
                d->fields = xrealloc(d->fields, (size_t)(d->nfields + 1) * sizeof(CDeclField));
                d->fields[d->nfields].name = name;
                d->fields[d->nfields].type = cd_type_of(spec, abs);
                d->nfields++;
            }
        }
        // An anonymous struct or union member: its fields are the outer one's.
        if (!declarators && !ts_node_is_null(type) &&
            (cd_is(type, "struct_specifier") || cd_is(type, "union_specifier")) &&
            ts_node_is_null(cd_field(type, "name")))
        {
            TSNode inner = cd_field(type, "body");
            if (!ts_node_is_null(inner))
            {
                cd_fields(r, inner, d, depth + 1);
            }
        }
    }
}

// A struct, union or enum specifier: its tag (when it has one) into the table, its enum
// constants, and its fields into `into` too (a typedef of it), if given.
static void cd_aggregate(const CdRead *r, TSNode spec, CDecl *into)
{
    if (ts_node_is_null(spec))
    {
        return;
    }
    const char *t = ts_node_type(spec);
    int is_struct = !strcmp(t, "struct_specifier"), is_union = !strcmp(t, "union_specifier");
    int is_enum = !strcmp(t, "enum_specifier");
    if (!is_struct && !is_union && !is_enum)
    {
        return;
    }
    TSNode name = cd_field(spec, "name");
    TSNode body = cd_field(spec, "body");
    char *tag = ts_node_is_null(name) ? NULL : cd_text(r, name);
    if (tag)
    {
        cd_name(r, tag);
    }
    if (is_enum)
    {
        if (ts_node_is_null(body))
        {
            if (tag)
            {
                CDecl *d = xcalloc(1, sizeof(CDecl));
                d->kind = CD_ENUM;
                d->name = tag;
                cd_locate(r, spec, d);
                d->text = cd_cat("enum ", tag);
                cd_put(r->ctx, d);
            }
            return;
        }
        char *enum_type =
            tag ? cd_cat("enum ", tag) : (into ? xstrdup(into->name) : xstrdup("int"));
        uint32_t n = ts_node_named_child_count(body);
        for (uint32_t i = 0; i < n; i++)
        {
            TSNode e = ts_node_named_child(body, i);
            if (!cd_is(e, "enumerator"))
            {
                continue;
            }
            TSNode en = cd_field(e, "name");
            if (ts_node_is_null(en))
            {
                continue;
            }
            CDecl *d = xcalloc(1, sizeof(CDecl));
            d->kind = CD_ENUM_CONSTANT;
            d->name = cd_text(r, en);
            d->type = enum_type;
            d->text = cd_text(r, e);
            cd_locate(r, e, d);
            cd_name(r, d->name);
            cd_put(r->ctx, d);
        }
        if (tag)
        {
            CDecl *d = xcalloc(1, sizeof(CDecl));
            d->kind = CD_ENUM;
            d->name = tag;
            d->complete = !ts_node_has_error(spec);
            cd_locate(r, spec, d);
            d->text = cd_cat("enum ", tag);
            cd_put(r->ctx, d);
        }
        return;
    }
    CDecl agg = {0};
    if (!ts_node_is_null(body))
    {
        cd_fields(r, body, &agg, 0);
        agg.complete = !ts_node_has_error(spec);
    }
    if (into)
    {
        into->fields = agg.fields;
        into->nfields = agg.nfields;
        into->complete = agg.complete;
        into->aggregate = tag ? cd_cat(is_struct ? "struct " : "union ", tag) : xstrdup("");
    }
    if (tag)
    {
        CDecl *d = xcalloc(1, sizeof(CDecl));
        *d = agg;
        d->kind = is_struct ? CD_STRUCT : CD_UNION;
        d->name = tag;
        d->text = ts_node_is_null(body) ? cd_cat(is_struct ? "struct " : "union ", tag)
                                        : cd_shown(r, spec);
        cd_locate(r, spec, d);
        cd_put(r->ctx, d);
    }
}

// The text a declaration shows as: its own text, on one line, cut at 400 characters (and a
// function's body left out).
static char *cd_shown(const CdRead *r, TSNode n)
{
    char *s = cd_text(r, n);
    char *brace = strchr(s, '{');
    if (brace && cd_is(n, "function_definition"))
    {
        *brace = 0;
    }
    size_t len = strlen(s);
    while (len && s[len - 1] == ' ')
    {
        s[--len] = 0;
    }
    if (len > 400)
    {
        strcpy(s + 397, "...");
    }
    return s;
}

// A declaration, typedef or function definition at file scope.
static void cd_declaration(const CdRead *r, TSNode decl)
{
    const char *t = ts_node_type(decl);
    int is_typedef = !strcmp(t, "type_definition");
    int is_definition = !strcmp(t, "function_definition");
    TSNode type = cd_field(decl, "type");
    char *spec = cd_specifiers(r, decl);
    int any = 0;
    uint32_t n = ts_node_named_child_count(decl);
    for (uint32_t i = 0; i < n; i++)
    {
        TSNode c = ts_node_named_child(decl, i);
        // (the grammar takes size_t, int32_t and the like as primitive types, also where a
        // typedef declares them)
        if (ts_node_eq(c, type) ||
            (!strstr(ts_node_type(c), "declarator") && !cd_is(c, "identifier") &&
             !cd_is(c, "type_identifier") && !cd_is(c, "primitive_type")))
        {
            continue;
        }
        if ((cd_is(c, "type_identifier") || cd_is(c, "primitive_type")) && !is_typedef)
        {
            continue;
        }
        char *name = NULL;
        TSNode fn = is_typedef ? (TSNode){0} : cd_function_of(c);
        char *abs = cd_abstract(r, c, fn, &name);
        if (!name)
        {
            continue;
        }
        any = 1;
        CDecl *d = xcalloc(1, sizeof(CDecl));
        d->name = name;
        d->text = cd_shown(r, decl);
        cd_locate(r, decl, d);
        d->type = cd_type_of(spec, abs);
        if (is_typedef)
        {
            d->kind = CD_TYPEDEF;
            // A typedef of the struct itself has its fields; one of a pointer to it doesn't.
            cd_aggregate(r, type, abs[0] ? NULL : d);
        }
        else if (!ts_node_is_null(fn))
        {
            d->kind = CD_FUNCTION;
            TSNode params = cd_field(fn, "parameters");
            if (!ts_node_is_null(params))
            {
                cd_params(r, params, &d->params, &d->nparams, &d->variadic);
                d->params_known = ts_node_named_child_count(params) > 0 || is_definition;
            }
        }
        else
        {
            d->kind = CD_VARIABLE;
        }
        cd_name(r, name);
        cd_put(r->ctx, d);
    }
    if (!is_typedef)
    {
        cd_aggregate(r, type, NULL);
    }
    (void)any;
}

static void cd_walk(const CdRead *r, TSNode n, int depth)
{
    uint32_t count = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < count && depth < 8; i++)
    {
        TSNode c = ts_node_named_child(n, i);
        const char *t = ts_node_type(c);
        if (!strcmp(t, "declaration") || !strcmp(t, "type_definition") ||
            !strcmp(t, "function_definition"))
        {
            cd_declaration(r, c);
        }
        else if (!strcmp(t, "struct_specifier") || !strcmp(t, "union_specifier") ||
                 !strcmp(t, "enum_specifier"))
        {
            cd_aggregate(r, c, NULL);
        }
        else if (!strcmp(t, "linkage_specification") || !strcmp(t, "declaration_list") ||
                 !strcmp(t, "ERROR"))
        {
            cd_walk(r, c, depth + 1); // extern "C" { ... }, and what error recovery kept
        }
    }
}

// ---- The preprocessor's own lines -----------------------------------------------------------

// The type of a numeric literal that is a macro's whole body (in parentheses, signed or not),
// as C gives it: 800 int, 800u unsigned int, 800L long, 0.5 double, 0.5f float. NULL if the
// body is anything else, or a number too big for int with no suffix.
static char *cd_number_type(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '(')
    {
        p++;
    }
    if (*p == '-' || *p == '+')
    {
        p++;
    }
    if (!isdigit((unsigned char)*p) && !(*p == '.' && isdigit((unsigned char)p[1])))
    {
        return NULL;
    }
    const char *b = p;
    int hex = p[0] == '0' && (p[1] == 'x' || p[1] == 'X');
    int real = 0;
    if (hex)
    {
        p += 2;
    }
    while ((hex ? isxdigit((unsigned char)*p) : isdigit((unsigned char)*p)) || *p == '.' ||
           *p == '\'' ||
           (!hex && (*p == 'e' || *p == 'E') &&
            (p[1] == '-' || p[1] == '+' || isdigit((unsigned char)p[1]))))
    {
        if (*p == '.' || (!hex && (*p == 'e' || *p == 'E')))
        {
            real = 1;
            if ((*p == 'e' || *p == 'E') && (p[1] == '-' || p[1] == '+'))
            {
                p++;
            }
        }
        p++;
    }
    const char *suffix = p;
    while (isalpha((unsigned char)*p))
    {
        p++;
    }
    size_t ns = (size_t)(p - suffix);
    while (*p == ' ' || *p == '\t' || *p == ')')
    {
        p++;
    }
    if (*p)
    {
        return NULL;
    }
    char sfx[8] = {0};
    if (ns >= sizeof sfx)
    {
        return NULL;
    }
    for (size_t i = 0; i < ns; i++)
    {
        sfx[i] = (char)tolower((unsigned char)suffix[i]);
    }
    if (real)
    {
        return xstrdup(!ns                 ? "double"
                       : !strcmp(sfx, "f") ? "float"
                       : !strcmp(sfx, "l") ? "long double"
                                           : "");
    }
    if (!ns)
    {
        char *end = NULL;
        unsigned long long v = strtoull(b, &end, 0);
        return v <= 0x7fffffffULL ? xstrdup("int") : NULL;
    }
    int u = strchr(sfx, 'u') != NULL;
    int l = (int)(strchr(sfx, 'l') ? (strstr(sfx, "ll") ? 2 : 1) : 0);
    if (strspn(sfx, "ul") != ns)
    {
        return NULL;
    }
    static const char *const names[2][3] = {
        {"int", "long", "long long"}, {"unsigned int", "unsigned long", "unsigned long long"}};
    return xstrdup(names[u][l]);
}

// A #define line: the macro, into the table and the names.
static void cd_define(ParserContext *ctx, const char *line, const char *file, int lineno)
{
    const char *p = line + 1;
    while (*p == ' ' || *p == '\t')
    {
        p++;
    }
    if (strncmp(p, "define", 6) != 0 || (p[6] != ' ' && p[6] != '\t'))
    {
        return;
    }
    p += 6;
    while (*p == ' ' || *p == '\t')
    {
        p++;
    }
    const char *b = p;
    while (isalnum((unsigned char)*p) || *p == '_' || *p == '$')
    {
        p++;
    }
    if (p == b)
    {
        return;
    }
    c_name_note(ctx, b, (size_t)(p - b));
    CDecl *d = xcalloc(1, sizeof(CDecl));
    d->kind = CD_MACRO;
    d->name = xmalloc((size_t)(p - b) + 1);
    memcpy(d->name, b, (size_t)(p - b));
    d->name[p - b] = 0;
    size_t len = strlen(line);
    d->text = xmalloc(len + 1);
    memcpy(d->text, line, len + 1);
    if (len > 400)
    {
        strcpy(d->text + 397, "...");
    }
    d->file = file ? xstrdup(file) : NULL;
    d->line = lineno;
    d->end_line = lineno;
    if (*p == ' ' || *p == '\t') // an object-like macro: its value's type, if a number
    {
        d->type = cd_number_type(p);
    }
    cd_put(ctx, d);
}

// GCC's extensions that say nothing about what a declaration declares, blanked (spaces, so
// offsets and rows stay): __attribute__((...)), __asm__("...") labels, __extension__. The
// grammar doesn't take an attribute everywhere GCC does (zc's own preamble has one between
// an array and its initializer), and its error recovery can then swallow what follows.
static void cd_blank_extensions(char *s)
{
    static const char *const with_parens[] = {"__attribute__", "__attribute", "__asm__",
                                              "__asm",         "__declspec",  NULL};
    for (char *p = s; *p; p++)
    {
        if (*p == '"' || *p == '\'')
        {
            char q = *p++;
            while (*p && *p != q && *p != '\n')
            {
                p += (*p == '\\' && p[1]) ? 2 : 1;
            }
            if (!*p)
            {
                break;
            }
            continue;
        }
        if (!(isalpha((unsigned char)*p) || *p == '_') ||
            (p > s && (isalnum((unsigned char)p[-1]) || p[-1] == '_')))
        {
            continue;
        }
        char *b = p;
        while (isalnum((unsigned char)*p) || *p == '_')
        {
            p++;
        }
        size_t n = (size_t)(p - b);
        if (n == 13 && !strncmp(b, "__extension__", 13))
        {
            memset(b, ' ', n);
            p--;
            continue;
        }
        int wrapped = 0;
        for (int i = 0; with_parens[i]; i++)
        {
            wrapped |= strlen(with_parens[i]) == n && !strncmp(b, with_parens[i], n);
        }
        if (!wrapped)
        {
            p--;
            continue;
        }
        char *q = p;
        while (*q == ' ' || *q == '\t' || *q == '\n')
        {
            q++;
        }
        if (*q != '(')
        {
            p--;
            continue;
        }
        int depth = 0;
        char *e = q;
        for (; *e; e++)
        {
            if (*e == '"')
            {
                for (e++; *e && *e != '"'; e++)
                {
                    if (*e == '\\' && e[1])
                    {
                        e++;
                    }
                }
                if (!*e)
                {
                    break;
                }
                continue;
            }
            if (*e == '(')
            {
                depth++;
            }
            else if (*e == ')' && --depth == 0)
            {
                break;
            }
        }
        if (!*e)
        {
            p--;
            continue;
        }
        for (char *k = b; k <= e; k++)
        {
            if (*k != '\n')
            {
                *k = ' ';
            }
        }
        p = e;
    }
}

void c_decls_extract(ParserContext *ctx, const char *text)
{
    size_t len = strlen(text);
    char *c_only = xmalloc(len + 1);
    memcpy(c_only, text, len + 1);
    int nrows = 1;
    for (const char *p = text; *p; p++)
    {
        nrows += *p == '\n';
    }
    CdRead r = {ctx, c_only, xcalloc((size_t)nrows, sizeof(char *)),
                xcalloc((size_t)nrows, sizeof(int)), nrows};
    const char *file = NULL;
    int lineno = 1;
    int row = 0;
    for (char *p = c_only; *p; row++)
    {
        char *nl = strchr(p, '\n');
        char *end = nl ? nl : p + strlen(p);
        char *q = p;
        while (q < end && (*q == ' ' || *q == '\t'))
        {
            q++;
        }
        r.row_file[row] = file;
        r.row_line[row] = lineno;
        if (*q == '#')
        {
            char saved = *end;
            *end = 0;
            int n = 0;
            if (sscanf(q, "# %d \"%n", &lineno, &n) == 1 && n > 0)
            {
                const char *fq = q + n;
                const char *fe = strchr(fq, '"');
                if (fe)
                {
                    char *f = xmalloc((size_t)(fe - fq) + 1);
                    memcpy(f, fq, (size_t)(fe - fq));
                    f[fe - fq] = 0;
                    file = f;
                }
                lineno--; // the line after the marker is the one it names
            }
            else
            {
                cd_define(ctx, q, file, lineno);
            }
            *end = saved;
            for (char *b = p; b < end; b++)
            {
                *b = ' '; // tree-sitter's rows stay the output's lines
            }
        }
        lineno++;
        p = nl ? nl + 1 : end;
    }
    cd_blank_extensions(c_only);
    TSParser *parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_c());
    TSTree *tree = ts_parser_parse_string(parser, NULL, c_only, (uint32_t)len);
    if (tree)
    {
        cd_walk(&r, ts_tree_root_node(tree), 0);
        ts_tree_delete(tree);
    }
    ts_parser_delete(parser);
    zfree(c_only);
}

// ---- Types, for zc's inference and checks ---------------------------------------------------

const char *c_decl_writable(const char *ctype)
{
    if (!ctype || !ctype[0] || strchr(ctype, '(') || strchr(ctype, '[') ||
        strstr(ctype, "<anonymous>"))
    {
        return NULL;
    }
    // zc writes "char*", not "char *".
    char *out = xmalloc(strlen(ctype) + 1);
    size_t k = 0;
    for (const char *p = ctype; *p; p++)
    {
        if (*p == ' ' && p[1] == '*')
        {
            continue;
        }
        out[k++] = *p;
    }
    out[k] = 0;
    return out;
}

// A type with its qualifiers and a struct/union/enum keyword taken off: the name to look
// up, and the keyword ("struct ", ...) that was on it, if one.
static const char *cd_bare(const char *type, char *buf, size_t n, const char **keyword)
{
    *keyword = NULL;
    for (;;)
    {
        while (*type == ' ')
        {
            type++;
        }
        if (!strncmp(type, "const ", 6))
        {
            type += 6;
        }
        else if (!strncmp(type, "volatile ", 9))
        {
            type += 9;
        }
        else
        {
            break;
        }
    }
    static const char *const kws[] = {"struct ", "union ", "enum ", NULL};
    for (int i = 0; kws[i]; i++)
    {
        size_t kn = strlen(kws[i]);
        if (!strncmp(type, kws[i], kn))
        {
            *keyword = kws[i];
            type += kn;
            break;
        }
    }
    snprintf(buf, n, "%s", type);
    size_t len = strlen(buf);
    while (len && buf[len - 1] == ' ')
    {
        buf[--len] = 0;
    }
    return buf;
}

const char *c_decl_value_type(ParserContext *ctx, const char *name)
{
    const CDecl *d = c_decl_find(ctx, name);
    if (!d || (d->kind != CD_VARIABLE && d->kind != CD_ENUM_CONSTANT && d->kind != CD_MACRO))
    {
        return NULL;
    }
    if (d->kind == CD_ENUM_CONSTANT && d->type && !strncmp(d->type, "enum ", 5))
    {
        return d->type; // written as is: "enum color"
    }
    return c_decl_writable(d->type);
}

const char *c_decl_return_type(ParserContext *ctx, const char *name)
{
    const CDecl *d = c_decl_find(ctx, name);
    return d && d->kind == CD_FUNCTION ? c_decl_writable(d->type) : NULL;
}

const CDecl *c_decl_aggregate(ParserContext *ctx, const char *type)
{
    char buf[512], key[530];
    for (int hops = 0; type && hops < 16; hops++)
    {
        const char *kw = NULL;
        const char *name = cd_bare(type, buf, sizeof buf, &kw);
        if (strchr(name, '*') || strchr(name, '['))
        {
            return NULL;
        }
        if (kw)
        {
            if (!strcmp(kw, "enum "))
            {
                return NULL;
            }
            snprintf(key, sizeof key, "%s%s", kw, name);
            const CDecl *d = c_decl_find(ctx, key);
            return d && d->complete ? d : NULL;
        }
        const CDecl *d = c_decl_find(ctx, name);
        if (!d || d->kind != CD_TYPEDEF)
        {
            return NULL;
        }
        if (d->complete && d->aggregate)
        {
            return d; // the typedef of a struct body: its fields are its own
        }
        type = d->aggregate && d->aggregate[0] ? d->aggregate : d->type;
    }
    return NULL;
}

const CDeclField *c_decl_field(const CDecl *agg, const char *field)
{
    for (int i = 0; agg && i < agg->nfields; i++)
    {
        if (agg->fields[i].name && !strcmp(agg->fields[i].name, field))
        {
            return &agg->fields[i];
        }
    }
    return NULL;
}

const char *c_decl_enum_of(ParserContext *ctx, const char *type)
{
    char buf[512];
    for (int hops = 0; type && hops < 16; hops++)
    {
        const char *kw = NULL;
        const char *name = cd_bare(type, buf, sizeof buf, &kw);
        if (kw)
        {
            if (strcmp(kw, "enum ") != 0 || !strcmp(name, "<anonymous>"))
            {
                return NULL;
            }
            char *e = xmalloc(strlen(name) + 6);
            sprintf(e, "enum %s", name);
            return e;
        }
        const CDecl *d = c_decl_find(ctx, name);
        if (!d || d->kind != CD_TYPEDEF || !d->type)
        {
            return NULL;
        }
        if (!strcmp(d->type, "enum <anonymous>"))
        {
            return d->name; // the typedef names the enum
        }
        type = d->type;
    }
    return NULL;
}
