// SPDX-License-Identifier: MIT

#include "../ast/ast.h"
#include "../constants.h"
#include "../parser/parser.h"
#include "../zprep.h"
#include "codegen.h"
#include "compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../platform/misra.h"
#include "codegen_internal.h"

// Whether `t` is a function type or holds one (a pointer to, or an array of, function
// pointers).
static int has_function_type(Type *t)
{
    for (; t; t = t->inner)
    {
        if (t->kind == TYPE_FUNCTION)
        {
            return 1;
        }
    }
    return 0;
}

// --reload: the C type of a global whose storage a reload host keeps, or NULL for one the
// module keeps itself (a constant, a thread-local, or one whose type only the C compiler
// infers). Such a global is defined when the host compiles the module with
// ZC_RELOAD_OWN_GLOBALS, and declared extern otherwise: the host then points it at the
// storage of the version that defined it, so its value outlives the code.
char *reload_global_type(ParserContext *ctx, ASTNode *node)
{
    if (!ctx->config->reload_mode || ctx->config->use_cpp || node->kind != NODE_VAR_DECL ||
        node->var_decl.is_thread_local || !node->var_decl.name)
    {
        return NULL;
    }
    if (node->var_decl.type_str && has_function_type(node->type_info))
    {
        return type_to_c_string(node->type_info, ctx->config->misra_mode); // as emit_globals
    }
    if (node->var_decl.type_str)
    {
        return xstrdup(node->var_decl.type_str);
    }
    char *inferred = node->var_decl.init_expr ? infer_type(ctx, node->var_decl.init_expr) : NULL;
    if (inferred && strcmp(inferred, "__auto_type") == 0)
    {
        zfree(inferred);
        return NULL;
    }
    return inferred;
}

// --reload: a function's `static` locals keep their values across reloads too. Each one's
// storage is a host-kept global (like reload_global_type's), `_zs_<function>_<name>`, and
// inside the function its name means that global from its declaration to the end of its
// block. One whose C type isn't written out stays a C static, which a reload starts again.

static char *reload_static_type(ParserContext *ctx, ASTNode *decl)
{
    if (decl->type_info)
    {
        return type_to_c_string(decl->type_info, 0);
    }
    if (decl->var_decl.type_str && strcmp(decl->var_decl.type_str, "__auto_type") != 0)
    {
        return xstrdup(decl->var_decl.type_str);
    }
    char *inferred = decl->var_decl.init_expr ? infer_type(ctx, decl->var_decl.init_expr) : NULL;
    if (inferred && strcmp(inferred, "__auto_type") == 0)
    {
        zfree(inferred);
        return NULL;
    }
    return inferred;
}

static void reload_collect_in(ParserContext *ctx, ASTNode *node, const char *fname, const char *cfg,
                              int depth)
{
    if (!node || depth > 4096)
    {
        return;
    }
    if (node->kind == NODE_VAR_DECL && node->var_decl.is_static &&
        !node->var_decl.is_thread_local && node->var_decl.name && !reload_static_name(ctx, node))
    {
        char *type = reload_static_type(ctx, node);
        if (type)
        {
            // A second static of the same name in the function gets a number.
            size_t len = strlen(fname) + strlen(node->var_decl.name) + 24;
            char *c_name = xmalloc(len);
            snprintf(c_name, len, "_zs_%s_%s", fname, node->var_decl.name);
            for (int n = 2;; n++)
            {
                int taken = 0;
                for (int i = 0; i < ctx->cg.reload_static_count && !taken; i++)
                {
                    taken = strcmp(ctx->cg.reload_statics[i].c_name, c_name) == 0;
                }
                if (!taken)
                {
                    break;
                }
                snprintf(c_name, len, "_zs_%s_%s_%d", fname, node->var_decl.name, n);
            }
            ctx->cg.reload_statics =
                xrealloc(ctx->cg.reload_statics, sizeof(*ctx->cg.reload_statics) *
                                                     (size_t)(ctx->cg.reload_static_count + 1));
            struct ReloadStatic *s = &ctx->cg.reload_statics[ctx->cg.reload_static_count++];
            s->decl = node;
            s->c_name = c_name;
            s->type = type;
            s->cfg = cfg;
        }
    }
    ASTChildren children = ast_children(node);
    for (int i = 0; i < children.count; i++)
    {
        for (ASTNode *c = *children.slots[i]; c; c = c->next)
        {
            reload_collect_in(ctx, c, fname, cfg, depth + 1);
        }
    }
}

void reload_collect_statics(ParserContext *ctx, ASTNode *funcs)
{
    if (!ctx->config->reload_mode || ctx->config->use_cpp)
    {
        return;
    }
    for (ASTNode *f = funcs; f; f = f->next)
    {
        if (f->kind == NODE_FUNCTION && f->func.body && !f->func.generic_params)
        {
            reload_collect_in(ctx, f->func.body, f->link_name ? f->link_name : f->func.name,
                              f->cfg_condition, 0);
        }
    }
}

void emit_reload_statics(ParserContext *ctx)
{
    for (int i = 0; i < ctx->cg.reload_static_count; i++)
    {
        struct ReloadStatic *s = &ctx->cg.reload_statics[i];
        if (s->cfg)
        {
            EMIT(ctx, "#if %s\n", s->cfg);
        }
        EMIT(ctx, "#ifdef ZC_RELOAD_OWN_GLOBALS\nZC_GLOBAL ");
        emit_var_decl_type(ctx, s->type, s->c_name);
        if (s->decl->var_decl.init_expr)
        {
            EMIT(ctx, " = ");
            codegen_expression(ctx, s->decl->var_decl.init_expr);
        }
        EMIT(ctx, ";\n#else\nextern ");
        emit_var_decl_type(ctx, s->type, s->c_name);
        EMIT(ctx, ";\n#endif\n");
        if (s->cfg)
        {
            EMIT(ctx, "#endif\n");
        }
    }
}

const char *reload_static_name(ParserContext *ctx, ASTNode *decl)
{
    for (int i = 0; i < ctx->cg.reload_static_count; i++)
    {
        if (ctx->cg.reload_statics[i].decl == decl)
        {
            return ctx->cg.reload_statics[i].c_name;
        }
    }
    return NULL;
}

void reload_block_enter(ParserContext *ctx)
{
    ctx->cg.block_depth++;
}

void reload_block_leave(ParserContext *ctx)
{
    while (ctx->cg.reload_rename_count > 0 &&
           ctx->cg.reload_renames[ctx->cg.reload_rename_count - 1].depth >= ctx->cg.block_depth)
    {
        ctx->cg.reload_rename_count--;
    }
    ctx->cg.block_depth--;
}

// A local named `name` from here to the end of the block: a static's storage (`c_name`), or
// another local, which hides one of the same name.
void reload_local_declared(ParserContext *ctx, const char *name, const char *c_name)
{
    if (!c_name && reload_rename(ctx, name) == NULL)
    {
        return; // nothing to hide
    }
    if (ctx->cg.reload_rename_count >= 64)
    {
        return;
    }
    ctx->cg.reload_renames[ctx->cg.reload_rename_count].name = name;
    ctx->cg.reload_renames[ctx->cg.reload_rename_count].c_name = c_name;
    ctx->cg.reload_renames[ctx->cg.reload_rename_count].depth = ctx->cg.block_depth;
    ctx->cg.reload_rename_count++;
}

const char *reload_rename(ParserContext *ctx, const char *name)
{
    for (int i = ctx->cg.reload_rename_count - 1; i >= 0; i--)
    {
        if (strcmp(ctx->cg.reload_renames[i].name, name) == 0)
        {
            return ctx->cg.reload_renames[i].c_name;
        }
    }
    return NULL;
}

static void emit_globals_internal(ParserContext *ctx, ASTNode *node, VisitedModules **visited,
                                  int depth)
{
    if (depth > 1024)
    {
        zfatal("Infinite recursion detected in emit_globals (ctx, circular imports?)");
    }
    while (node)
    {
        if (node->kind == NODE_IMPORT)
        {
            if (!is_module_visited(*visited, node->import_stmt.path))
            {
                mark_module_visited(visited, node->import_stmt.path);
                emit_globals_internal(ctx, node->import_stmt.module_root, visited, depth + 1);
            }
            node = node->next;
            continue;
        }
        if (node->kind == NODE_VAR_DECL || node->kind == NODE_CONST)
        {
            if (node->cfg_condition)
            {
                EMIT(ctx, "#if %s\n", node->cfg_condition); // on a line of its own
            }
            char *host_type = reload_global_type(ctx, node);
            if (host_type)
            {
                EMIT(ctx, "#ifdef ZC_RELOAD_OWN_GLOBALS\n");
            }
            EMIT(ctx, "ZC_GLOBAL ");
            if (node->kind == NODE_CONST)
            {
                EMIT(ctx, "__attribute__((unused)) const ");
            }
            if (node->var_decl.type_str && has_function_type(node->type_info))
            {
                // type_str is Zen C's spelling (`fn*() -> void`), which isn't C for a
                // function pointer: write the C one, as a local or a field does.
                char *ctype = type_to_c_string(node->type_info, ctx->config->misra_mode);
                emit_var_decl_type(ctx, ctype, node->var_decl.name);
                zfree(ctype);
            }
            else if (node->var_decl.type_str)
            {
                emit_var_decl_type(ctx, node->var_decl.type_str, node->var_decl.name);
            }
            else
            {
                char *inferred = NULL;
                if (node->var_decl.init_expr)
                {
                    inferred = infer_type(ctx, node->var_decl.init_expr);
                }

                if (inferred && strcmp(inferred, "__auto_type") != 0)
                {
                    emit_var_decl_type(ctx, inferred, node->var_decl.name);
                }
                else
                {
                    emit_auto_type(ctx, node->var_decl.init_expr, node->token);
                    EMIT(ctx, " %s", node->var_decl.name);
                }
                if (inferred)
                {
                    zfree(inferred);
                }
            }
            if (node->var_decl.init_expr)
            {
                EMIT(ctx, " = ");
                char *tname =
                    node->var_decl.type_str
                        ? xstrdup(node->var_decl.type_str)
                        : (node->var_decl.init_expr ? infer_type(ctx, node->var_decl.init_expr)
                                                    : NULL);
                if (ctx->config->use_cpp && tname &&
                    (strchr(tname, '*') || is_enum_type_name(ctx, tname)))
                {
                    EMIT(ctx, "(%s)(", tname);
                    codegen_expression(ctx, node->var_decl.init_expr);
                    EMIT(ctx, ")");
                }
                else
                {
                    codegen_expression(ctx, node->var_decl.init_expr);
                }
                if (tname)
                {
                    zfree(tname);
                }
            }
            EMIT(ctx, ";\n");
            if (host_type)
            {
                EMIT(ctx, "#else\nextern ");
                emit_var_decl_type(ctx, host_type, node->var_decl.name);
                EMIT(ctx, ";\n#endif\n");
                zfree(host_type);
            }
            if (ctx->config->use_cpp && node->kind == NODE_VAR_DECL)
            {
                char *tname =
                    node->var_decl.type_str
                        ? xstrdup(node->var_decl.type_str)
                        : (node->var_decl.init_expr ? infer_type(ctx, node->var_decl.init_expr)
                                                    : NULL);
                if (tname)
                {
                    char *ct = tname;
                    if (strncmp(ct, "struct ", 7) == 0)
                    {
                        ct += 7;
                    }
                    ASTNode *def = find_struct_def(ctx, ct);
                    if (def && def->type_info && def->type_info->traits.has_drop)
                    {
                        EMIT(ctx, "int __z_drop_flag_%s = %d;\n", node->var_decl.name,
                             node->var_decl.init_expr ? 1 : 0);
                    }
                    zfree(tname);
                }
            }
            if (node->cfg_condition)
            {
                EMIT(ctx, "#endif\n");
            }
        }
        node = node->next;
    }
}

void emit_globals(ParserContext *ctx, ASTNode *node, VisitedModules **visited)
{
    ctx->cg.current_func_ret_type = NULL;
    ctx->cg.current_lambda = NULL;
    if (visited)
    {
        emit_globals_internal(ctx, node, visited, 0);
    }
    else
    {
        VisitedModules *local_visited = NULL;
        emit_globals_internal(ctx, node, &local_visited, 0);
        free_visited_modules(local_visited);
    }
}

// Emit function prototypes
// The prototypes emitted so far, by name and @cfg (CodegenKeySet's kind is unused: 0).
typedef CodegenKeySet EmittedProto;

static void emit_protos_internal(ParserContext *ctx, ASTNode *node, VisitedModules **visited,
                                 EmittedProto *emitted, int depth)
{
    if (depth > 1024)
    {
        zfatal("Infinite recursion detected in emit_protos (ctx, circular imports?)");
    }
    ASTNode *f = node;
    while (f)
    {
        if (f->kind == NODE_IMPORT)
        {
            if (!is_module_visited(*visited, f->import_stmt.path))
            {
                mark_module_visited(visited, f->import_stmt.path);
                emit_protos_internal(ctx, f->import_stmt.module_root, visited, emitted, depth + 1);
            }
            f = f->next;
            continue;
        }

        if (f->kind == NODE_FUNCTION)
        {
            if (f->func.name && !f->func.body)
            {
                if (strncmp(f->func.name, "_z_", 3) == 0 || strncmp(f->func.name, "_time_", 6) == 0)
                {
                    f = f->next;
                    continue;
                }
                static const char *skip_cstdlib[] = {
                    "strstr",  "strchr",   "strrchr", "strpbrk", "memchr",  "atoi",   "atol",
                    "atof",    "strtol",   "strtoul", "strtod",  "malloc",  "calloc", "realloc",
                    "free",    "memcpy",   "memmove", "memset",  "memcmp",  "strlen", "strcmp",
                    "strncmp", "strcpy",   "strncpy", "strcat",  "strncat", "printf", "fprintf",
                    "sprintf", "snprintf", "fopen",   "fclose",  "fread",   "fwrite", "fseek",
                    "ftell",   "exit",     "abort",   "abs",     NULL};
                int skip_fn = 0;
                for (int si = 0; skip_cstdlib[si]; si++)
                {
                    if (strcmp(f->func.name, skip_cstdlib[si]) == 0)
                    {
                        skip_fn = 1;
                        break;
                    }
                }
                if (skip_fn)
                {
                    f = f->next;
                    continue;
                }
            }

            if (f->cfg_condition)
            {
                EMIT(ctx, "#if %s\n", f->cfg_condition);
            }
            if (f->func.name)
            {
                int already_emitted =
                    codegen_keyset_has(emitted, 0, f->func.name, f->cfg_condition);
                if (!already_emitted)
                {
                    codegen_keyset_add(emitted, 0, f->func.name, f->cfg_condition);
                    if (ctx->config->use_cpp && f->func.is_extern)
                    {
                        EMIT(ctx, "extern \"C\" ");
                    }
                    emit_func_signature(ctx, f, NULL);
                    EMIT(ctx, ";\n");
                }
            }
            if (f->cfg_condition)
            {
                EMIT(ctx, "#endif\n");
            }
        }
        else if (f->kind == NODE_IMPL)
        {
            char *sname = f->impl.struct_name;
            if (!sname)
            {
                f = f->next;
                continue;
            }

            // Resolve opaque alias (e.g. StringView -> Slice__char)
            TypeAlias *ta = find_type_alias_node(ctx, sname);
            const char *resolved = (ta && !ta->is_opaque) ? ta->original_type : NULL;
            const char *effective_name = resolved ? resolved : sname;

            char *mangled = replace_string_type(sname);
            ASTNode *def = find_struct_def(ctx, mangled);
            if (!def && resolved)
            {
                zfree(mangled);
                mangled = replace_string_type(resolved);
                def = find_struct_def(ctx, mangled);
            }
            int skip = 0;
            if (def)
            {
                if (def->kind == NODE_STRUCT && def->strct.is_template)
                {
                    skip = 1;
                }
                else if (def->kind == NODE_ENUM && def->enm.is_template)
                {
                    skip = 1;
                }
            }
            else
            {
                char *buf = strip_template_suffix(sname);
                if (buf)
                {
                    def = find_struct_def(ctx, buf);
                    if (def && def->strct.is_template)
                    {
                        skip = 1;
                    }
                    zfree(buf);
                }
            }
            if (mangled)
            {
                zfree(mangled);
            }

            if (skip)
            {
                f = f->next;
                continue;
            }

            if (f->cfg_condition)
            {
                EMIT(ctx, "#if %s\n", f->cfg_condition);
            }
            ASTNode *m = f->impl.methods;
            while (m)
            {
                if (m->func.generic_params)
                {
                    m = m->next;
                    continue;
                }
                if (m->cfg_condition)
                {
                    EMIT(ctx, "#if %s\n", m->cfg_condition);
                }
                char *fname = m->func.name;

                // Build proto: if fname starts with sname__, replace with effective_name__
                char *proto = NULL;
                size_t slen = strlen(sname);
                if (strncmp(fname, sname, (size_t)(slen)) == 0 && fname[slen] == '_' &&
                    fname[slen + 1] == '_')
                {
                    // Replace alias prefix with resolved name
                    const char *method_part = fname + slen; // "__method"
                    proto = xmalloc(strlen(effective_name) + strlen(method_part) + 1);
                    sprintf(proto, "%s%s", effective_name, method_part);
                }
                else
                {
                    proto = xmalloc(strlen(effective_name) + strlen(fname) + 3);
                    sprintf(proto, "%s__%s", effective_name, fname);
                }

                emit_func_signature(ctx, m, proto);
                EMIT(ctx, ";\n");
                if (m->cfg_condition)
                {
                    EMIT(ctx, "#endif\n");
                }

                zfree(proto);
                m = m->next;
            }
            if (f->cfg_condition)
            {
                EMIT(ctx, "#endif\n");
            }
        }
        else if (f->kind == NODE_IMPL_TRAIT)
        {
            char *sname = f->impl_trait.target_type;
            if (!sname)
            {
                f = f->next;
                continue;
            }

            char *mangled = replace_string_type(sname);
            ASTNode *def = find_struct_def(ctx, mangled);
            int skip = 0;
            if (def)
            {
                if (def->strct.is_template)
                {
                    skip = 1;
                }
            }
            else
            {
                char *buf = strip_template_suffix(sname);
                if (buf)
                {
                    def = find_struct_def(ctx, buf);
                    if (def && def->strct.is_template)
                    {
                        skip = 1;
                    }
                    zfree(buf);
                }
            }
            if (mangled)
            {
                zfree(mangled);
            }

            if (skip)
            {
                f = f->next;
                continue;
            }

            if (f->cfg_condition)
            {
                EMIT(ctx, "#if %s\n", f->cfg_condition);
            }
            ASTNode *m = f->impl_trait.methods;
            while (m)
            {
                if (m->func.generic_params)
                {
                    m = m->next;
                    continue;
                }
                if (m->cfg_condition)
                {
                    EMIT(ctx, "#if %s\n", m->cfg_condition);
                }
                emit_func_signature(ctx, m, NULL);
                EMIT(ctx, ";\n");
                if (m->cfg_condition)
                {
                    EMIT(ctx, "#endif\n");
                }
                m = m->next;
            }
            if (f->cfg_condition)
            {
                EMIT(ctx, "#endif\n");
            }
        }
        else if (f->kind == NODE_ROOT)
        {
            emit_protos_internal(ctx, f->root.children, visited, emitted, depth + 1);
        }
        f = f->next;
    }
}

void emit_protos(ParserContext *ctx, ASTNode *node, VisitedModules **visited)
{
    EmittedProto emitted = {0};
    if (visited)
    {
        emit_protos_internal(ctx, node, visited, &emitted, 0);
    }
    else
    {
        VisitedModules *local_visited = NULL;
        emit_protos_internal(ctx, node, &local_visited, &emitted, 0);
    }
    codegen_keyset_free(&emitted);
}

// Emit VTable instances for trait implementations.
void emit_impl_vtables(ParserContext *ctx)
{
    StructRef *ref = ctx->parsed_impls_list;
    struct
    {
        char *trait;
        char *strct;
    } emitted[MAX_ERROR_MSG_LEN];
    int count = 0;

    while (ref)
    {
        ASTNode *node = ref->node;
        if (node && node->kind == NODE_IMPL_TRAIT)
        {
            char *trait = node->impl_trait.trait_name;

            // Filter generic traits (VTables for them are not emitted)
            int is_generic_trait = 0;
            StructRef *search = ctx->parsed_globals_list;
            while (search)
            {
                if (search->node && search->node->kind == NODE_TRAIT &&
                    strcmp(search->node->trait.name, trait) == 0)
                {
                    if (search->node->trait.generic_param_count > 0)
                    {
                        is_generic_trait = 1;
                    }
                    break;
                }
                search = search->next;
            }
            if (is_generic_trait)
            {
                ref = ref->next;
                continue;
            }

            char *strct = node->impl_trait.target_type;

            // Filter templates
            char *mangled = replace_string_type(strct);
            ASTNode *def = find_struct_def(ctx, mangled);
            int skip = 0;
            if (def)
            {
                if (def->kind == NODE_STRUCT && def->strct.is_template)
                {
                    skip = 1;
                }
                else if (def->kind == NODE_ENUM && def->enm.is_template)
                {
                    skip = 1;
                }
            }
            else
            {
                char *buf = strip_template_suffix(strct);
                if (buf)
                {
                    def = find_struct_def(ctx, buf);
                    if (def && def->strct.is_template)
                    {
                        skip = 1;
                    }
                    zfree(buf);
                }
            }
            if (mangled)
            {
                zfree(mangled);
            }
            if (skip)
            {
                ref = ref->next;
                continue;
            }

            // Check duplication
            int dup = 0;
            for (int i = 0; i < count; i++)
            {
                if (strcmp(emitted[i].trait, trait) == 0 && strcmp(emitted[i].strct, strct) == 0)
                {
                    dup = 1;
                    break;
                }
            }
            if (dup)
            {
                ref = ref->next;
                continue;
            }

            emitted[count].trait = trait;
            emitted[count].strct = strct;
            count++;

            if (0 == strcmp(trait, "Copy") || 0 == strcmp(trait, "Eq") ||
                0 == strcmp(trait, "Drop") || 0 == strcmp(trait, "Clone") ||
                0 == strcmp(trait, "Iterable"))
            {
                // Marker trait or statically-dispatched trait, no runtime vtable needed
                ref = ref->next;
                continue;
            }

            // --reload: the vtable's storage is the host's, like a global's
            // (reload_global_type), and a constant copy holds this version's functions, which
            // the host copies into it on every reload. A trait object made by any version
            // then calls the newest code.
            int host_kept = ctx->config->reload_mode && !ctx->config->use_cpp;
            if (host_kept)
            {
                EMIT(ctx, "#ifdef ZC_RELOAD_OWN_GLOBALS\n");
            }
            for (int copy = 0; copy < (host_kept ? 2 : 1); copy++)
            {
                if (copy == 1)
                {
                    EMIT(ctx, "#else\nextern %s_VTable %s__%s__VTable;\n#endif\n", trait, strct,
                         trait);
                    EMIT(ctx, "static const %s_VTable _zc_mf_init_%s__%s__VTable = {", trait, strct,
                         trait);
                    ctx->cg.reload_vtables =
                        xrealloc(ctx->cg.reload_vtables,
                                 sizeof(char *) * (size_t)(ctx->cg.reload_vtable_count + 2));
                    ctx->cg.reload_vtables[ctx->cg.reload_vtable_count++] = xstrdup(trait);
                    ctx->cg.reload_vtables[ctx->cg.reload_vtable_count++] = xstrdup(strct);
                }
                else
                {
                    EMIT(ctx, "%s_VTable %s__%s__VTable = {", trait, strct, trait);
                }

                ASTNode *m = node->impl_trait.methods;
                while (m)
                {
                    // Calculate expected prefix: Struct__Trait__
                    size_t pre_sz = strlen(strct) + strlen(trait) + 6;
                    char *prefix = xmalloc(pre_sz);
                    snprintf(prefix, pre_sz, "%s__%s__", strct, trait);

                    const char *orig_name = m->func.name;
                    if (strncmp(orig_name, prefix, strlen(prefix)) == 0)
                    {
                        orig_name += strlen(prefix);
                    }
                    else
                    {
                        orig_name = parse_original_method_name(m->func.name);
                    }

                    EMIT(ctx, ".%s = (__typeof__(((%s_VTable*)0)->%s))%s", orig_name, trait,
                         orig_name, m->func.name);
                    zfree(prefix);
                    if (m->next)
                    {
                        EMIT(ctx, ", ");
                    }
                    m = m->next;
                }
                EMIT(ctx, "};\n");
            }
        }
        ref = ref->next;
    }
}

// Emit test functions and runner. Returns number of tests emitted.
int emit_tests_and_runner(ParserContext *ctx, ASTNode *node)
{
    ASTNode *cur = node;
    int test_count = 0;
    while (cur)
    {
        if (cur->kind == NODE_TEST)
        {
            if (cur->cfg_condition)
            {
                EMIT(ctx, "#if %s\n", cur->cfg_condition);
            }
            EMIT(ctx, "static int _z_test_%d() {\n", test_count);
            EMIT(ctx, "fprintf(stderr, \"  TEST: %s ... \");\n", cur->test_stmt.name);
            EMIT(ctx, "int _zc_before = _zc_test_failures;\n");
            int saved = ctx->cg.defer_count;
            char *saved_ret = ctx->cg.current_func_ret_type;
            ctx->cg.current_func_ret_type = "void";
            codegen_walker(ctx, cur->test_stmt.body);
            ctx->cg.current_func_ret_type = saved_ret;
            // Run defers
            emit_defers_down_to(ctx, saved);
            ctx->cg.defer_count = saved;
            EMIT(ctx, "if (_zc_before == _zc_test_failures) { fprintf(stderr, \"OK\\n\"); } "
                      "else { fprintf(stderr, \"FAIL\\n\"); }\n");
            EMIT(ctx, "return _zc_test_failures - _zc_before;\n");
            EMIT(ctx, "}\n");
            if (cur->cfg_condition)
            {
                EMIT(ctx, "#endif\n");
            }
            test_count++;
        }
        cur = cur->next;
    }
    if (test_count > 0)
    {
        EMIT(ctx, "\nint _z_run_tests() {\n");
        emitter_indent(&ctx->cg.emitter);
        EMIT(ctx, "int _zc_total = 0;\n");
        cur = node;
        int i = 0;
        while (cur)
        {
            if (cur->kind == NODE_TEST)
            {
                if (cur->cfg_condition)
                {
                    EMIT(ctx, "#if %s\n", cur->cfg_condition);
                }
                EMIT(ctx, "_zc_total += _z_test_%d();\n", i);
                if (cur->cfg_condition)
                {
                    EMIT(ctx, "#endif\n");
                }
                i++;
            }
            cur = cur->next;
        }
        EMIT(ctx, "fprintf(stderr, \"\\n%%d test(s) failed\\n\", _zc_total);\n");
        EMIT(ctx, "return _zc_total;\n");
        emitter_dedent(&ctx->cg.emitter);
        EMIT(ctx, "}\n\n");
    }
    return test_count;
}

// Helper to emit typedefs for mangled pointer types (e.g., StringPtr for String*)
// used as generic parameters. This resolves "unknown type name StringPtr" errors.
static void emit_mangled_pointer_typedefs(ParserContext *ctx)
{
    char *emitted[2048];
    int count = 0;

    Instantiation *inst = ctx->instantiations;
    while (inst)
    {
        if (inst->concrete_arg && inst->unmangled_arg && strstr(inst->concrete_arg, "Ptr") &&
            strchr(inst->unmangled_arg, '*'))
        {
            // Check if already emitted
            int found = 0;
            for (int i = 0; i < count; i++)
            {
                if (strcmp(emitted[i], inst->concrete_arg) == 0)
                {
                    found = 1;
                    break;
                }
            }

            if (!found && count < 2048)
            {
                // In C, structs are usually typedef'd, so "typedef String* StringPtr;" is valid.
                EMIT(ctx, "typedef %s %s;\n", inst->unmangled_arg, inst->concrete_arg);
                emitted[count++] = inst->concrete_arg;
            }
        }
        inst = inst->next;
    }

    // Also scan instantiated functions which might have unique pointer arguments
    ASTNode *ifn = ctx->instantiated_funcs;
    while (ifn)
    {
        if (ifn->kind == NODE_FUNCTION && ifn->func.name && strstr(ifn->func.name, "__"))
        {
            char *mangled_part = (char *)strstr(ifn->func.name, "__") + 2;
            if (strstr(mangled_part, "Ptr"))
            {
                // This is more complex because we need the original type.
                // For now, struct instantiations cover 99% of cases via collections.
            }
        }
        ifn = ifn->next;
    }
}

// Emit type definitions-
void print_type_defs(ParserContext *ctx, ASTNode *nodes)
{
    emit_mangled_pointer_typedefs(ctx);

    if (!ctx->config->is_freestanding && !ctx->config->misra_mode)
    {
        EMIT(ctx, "typedef char* string;\n");

        EMIT(ctx, "typedef struct { void **data; int len; int cap; } Vec;\n");
        EMIT(ctx, "#define Vec_new() (Vec){.data=0, .len=0, .cap=0}\n");

        if (ctx->config->use_cpp)
        {
            EMIT(ctx, "static __attribute__((unused)) void _z_vec_push(Vec *v, void *item) { "
                      "if(v->len >= v->cap) { v->cap "
                      "= v->cap?v->cap*2:8; v->data = static_cast<void**>(realloc(v->data, "
                      "(size_t)v->cap "
                      "* sizeof(void*))); } v->data[v->len++] = item; }\n");
            EMIT(ctx,
                 "static inline __attribute__((unused)) Vec _z_make_vec(int count, ...) { Vec v = "
                 "{0}; v.cap = (count > 8 ? "
                 "count : 8); v.data = static_cast<void**>(malloc((size_t)v.cap * sizeof(void*))); "
                 "v.len = "
                 "0; va_list args; va_start(args, count); for(int i=0; i<count; i++) { "
                 "v.data[v.len++] = va_arg(args, void*); } va_end(args); return v; }\n");
        }
        else
        {
            EMIT(ctx, "static __attribute__((unused)) void _z_vec_push(Vec *v, void *item) { "
                      "if(v->len >= v->cap) { v->cap "
                      "= v->cap?v->cap*2:8; v->data = z_realloc(v->data, (size_t)v->cap * "
                      "sizeof(void*)); "
                      "} v->data[v->len++] = item; }\n");
            EMIT(ctx,
                 "static inline __attribute__((unused)) Vec _z_make_vec(int count, ...) { Vec "
                 "v = {0}; v.cap = (count "
                 "> 8 ? count : 8); v.data = z_malloc((size_t)v.cap * sizeof(void*)); v.len = 0; "
                 "va_list args; va_start(args, count); for(int i=0; i<count; i++) { "
                 "v.data[v.len++] = va_arg(args, void*); } va_end(args); return v; }\n");
        }
        EMIT(ctx, "#define Vec_push(v, i) _z_vec_push(&(v), (void*)(uintptr_t)(i))\n");
        EMIT(ctx, "static inline long _z_check_bounds(long index, long limit) { if(index < 0 || "
                  "index >= limit) { fprintf(stderr, \"Index out of bounds: %%ld (limit "
                  "%%ld)\\n\", index, limit); exit(1); } return index; }\n");
    }
    else
    {
        EMIT(ctx, "static inline long _z_check_bounds(long index, long limit) { if((index < 0) || "
                  "(index >= limit)) { __builtin_trap(); } return index; }\n");
    }

    ASTNode *local = nodes;
    while (local)
    {
        if (local->kind == NODE_STRUCT && !local->strct.is_template)
        {
            if (local->type_info && local->type_info->kind == TYPE_VECTOR)
            {
                // For vectors, we emit a custom typedef in emit_struct_defs.
                // Standard 'typedef struct Name Name' would conflict.
            }
            else if (local->strct.name && !local->strct.crepr_c_type)
            {
                const char *final_name = local->link_name ? local->link_name : local->strct.name;
                const char *keyword = local->strct.is_union ? "union" : "struct";
                EMIT(ctx, "typedef %s %s %s;\n", keyword, final_name, final_name);
            }
        }
        if (local->kind == NODE_ENUM && !local->enm.is_template && local->enm.name)
        {
            const char *final_name = local->link_name ? local->link_name : local->enm.name;

            // Only forward-declare as struct if it's an ADT-style enum (has payloads)
            int has_payload = 0;
            ASTNode *v = local->enm.variants;
            while (v)
            {
                if (v->variant.payload)
                {
                    has_payload = 1;
                    break;
                }
                v = v->next;
            }

            if (has_payload)
            {
                EMIT(ctx, "typedef struct %s %s;\n", final_name, final_name);
            }
            else
            {
                emit_simple_enum(ctx, local);
            }
        }

        local = local->next;
    }
    EMIT(ctx, "\n");

    SliceType *rev = NULL;
    SliceType *c = ctx->used_slices;
    while (c)
    {
        SliceType *next = c->next;
        c->next = rev;
        rev = c;
        c = next;
    }
    ctx->used_slices = rev;

    c = ctx->used_slices;
    while (c)
    {
        EMIT(ctx,
             "typedef struct Slice__%s Slice__%s;\nstruct Slice__%s { %s *data; int len; int cap; "
             "};\n",
             c->name, c->name, c->name, c->name);
        c = c->next;
    }

    // Tuple structs (`Tuple__...`) are part of the topologically sorted list:
    // their forward typedef comes from the loop above and their body from
    // emit_struct_defs, after the types they contain.

    // End of type definitions
}
