// SPDX-License-Identifier: MIT
#include "../parser/parser.h"

#include "codegen.h"
#include "../parser/utils/c_decls.h"
#include "zprep.h"
#include "../constants.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../plugins/plugin_manager.h"
#include "ast.h"
#include "zprep_plugin.h"
#include "codegen_internal.h"

void handle_expr_struct_init(ParserContext *ctx, ASTNode *node)
{
    const char *struct_name = node->struct_init.struct_name;
    if (strcmp(struct_name, "Self") == 0 && ctx->cg.current_impl_type)
    {
        struct_name = ctx->cg.current_impl_type;
    }

    // The name written in the C: the struct's @link_name if it has one.
    const char *c_name = struct_name;
    int is_zen_struct = 0;
    int is_union = 0;
    ASTNode *sdef = find_parsed_struct(ctx, struct_name);
    int is_vector = 0;
    if (sdef)
    {
        is_zen_struct = 1;
        if (sdef->link_name)
        {
            c_name = sdef->link_name;
        }
        if (sdef->strct.is_union)
        {
            is_union = 1;
        }
        if (sdef->type_info && sdef->type_info->kind == TYPE_VECTOR)
        {
            is_vector = 1;
        }
    }

    int in_func = (ctx->cg.current_func_ret_type != NULL || ctx->cg.current_lambda != NULL);

    int vec_size = 0;
    if (is_vector && sdef->type_info)
    {
        vec_size = sdef->type_info->array_size; // the struct found above
    }

    if (ctx->config->use_cpp)
    {
        if (in_func && !is_vector)
        {
            EMIT(ctx, "({ %s _s = %s; ", c_name, ctx->config->use_cpp ? "{}" : "{0}");
            ASTNode *f = node->struct_init.fields;
            while (f)
            {
                int skip = 0;
                if (f->var_decl.init_expr && f->var_decl.init_expr->kind == NODE_EXPR_LITERAL &&
                    f->var_decl.init_expr->literal.kind == LITERAL_INT &&
                    f->var_decl.init_expr->literal.int_val == 0)
                {
                    skip = 1;
                }
                if (!skip)
                {
                    if (f->var_decl.init_expr &&
                        f->var_decl.init_expr->kind == NODE_EXPR_ARRAY_LITERAL)
                    {
                        ASTNode *elem = f->var_decl.init_expr->array_literal.elements;
                        int idx = 0;
                        while (elem)
                        {
                            EMIT(ctx, "_s.%s[%d] = ", f->var_decl.name, idx++);
                            codegen_expression(ctx, elem);
                            EMIT(ctx, "; ");
                            elem = elem->next;
                        }
                    }
                    else
                    {
                        if (ctx->config->use_cpp)
                        {
                            EMIT(ctx, "_s.%s = (__typeof__(_s.%s))(", f->var_decl.name,
                                 f->var_decl.name);
                            codegen_expression_with_move(ctx, f->var_decl.init_expr);
                            EMIT(ctx, ")");
                        }
                        else
                        {
                            EMIT(ctx, "_s.%s = ", f->var_decl.name);
                            codegen_expression_with_move(ctx, f->var_decl.init_expr);
                        }
                        EMIT(ctx, "; ");
                    }
                }
                f = f->next;
            }
            EMIT(ctx, "_s; })");
        }
        else
        {
            EMIT(ctx, "%s { ", c_name);
            ASTNode *f = node->struct_init.fields;
            int field_count = 0;
            ASTNode *tmp = f;
            while (tmp)
            {
                field_count++;
                tmp = tmp->next;
            }

            if (is_vector && field_count == 1 && vec_size > 1)
            {
                for (int i = 0; i < vec_size; i++)
                {
                    if (i > 0)
                    {
                        EMIT(ctx, ", ");
                    }
                    codegen_expression(ctx, f->var_decl.init_expr);
                }
            }
            else
            {
                int first = 1;
                while (f)
                {
                    if (!first)
                    {
                        EMIT(ctx, ", ");
                    }
                    if (is_vector)
                    {
                        codegen_expression(ctx, f->var_decl.init_expr);
                    }
                    else
                    {
                        EMIT(ctx, ".%s = ", f->var_decl.name);
                        codegen_expression_with_move(ctx, f->var_decl.init_expr);
                    }
                    first = 0;
                    f = f->next;
                }
            }
            EMIT(ctx, " }");
        }
    }
    else
    {
        if (is_vector)
        {
            EMIT(ctx, "(%s){", c_name);
        }
        else if (is_union)
        {
            EMIT(ctx, "(union %s){", c_name);
        }
        else if (is_zen_struct)
        {
            EMIT(ctx, "(struct %s){", c_name);
        }
        else
        {
            EMIT(ctx, "(%s){", c_name);
        }

        ASTNode *f = node->struct_init.fields;
        int field_count = 0;
        ASTNode *tmp = f;
        while (tmp)
        {
            field_count++;
            tmp = tmp->next;
        }

        if (is_vector && field_count == 1 && vec_size > 1)
        {
            for (int i = 0; i < vec_size; i++)
            {
                if (i > 0)
                {
                    EMIT(ctx, ", ");
                }
                codegen_expression(ctx, f->var_decl.init_expr);
            }
        }
        else
        {
            int first = 1;
            while (f)
            {
                int skip = 0;
                if (f->var_decl.init_expr && f->var_decl.init_expr->kind == NODE_EXPR_LITERAL &&
                    f->var_decl.init_expr->literal.kind == LITERAL_INT &&
                    f->var_decl.init_expr->literal.int_val == 0)
                {
                    skip = 1;
                }
                if (!skip)
                {
                    if (!first)
                    {
                        EMIT(ctx, ", ");
                    }
                    if (!is_vector)
                    {
                        EMIT(ctx, ".%s = ", f->var_decl.name);
                    }
                    codegen_expression_with_move(ctx, f->var_decl.init_expr);
                    first = 0;
                }
                f = f->next;
            }
            if (first)
            {
                EMIT(ctx, "0");
            }
        }
        EMIT(ctx, "}");
    }
}

void handle_expr_binary(ParserContext *ctx, ASTNode *node)
{
    if (ctx->config->misra_mode)
    {
        EMIT(ctx, "(");
    }
    if (strncmp(node->binary.op, "??", 2) == 0 && strlen(node->binary.op) == 2)
    {
        EMIT(ctx, "({ ");
        emit_auto_type(ctx, node->binary.left, node->token);
        EMIT(ctx, " _l = (");
        codegen_expression(ctx, node->binary.left);
        EMIT(ctx, "); _l ? _l : (");
        codegen_expression(ctx, node->binary.right);
        EMIT(ctx, "); })");
    }
    else if (strcmp(node->binary.op, "?\?=") == 0)
    {
        EMIT(ctx, "({ if (!(");
        codegen_expression(ctx, node->binary.left);
        EMIT(ctx, ")) ");
        codegen_expression(ctx, node->binary.left);
        EMIT(ctx, " = (");
        codegen_expression(ctx, node->binary.right);
        EMIT(ctx, "); ");
        codegen_expression(ctx, node->binary.left);
        EMIT(ctx, "; })");
    }
    else if ((strcmp(node->binary.op, "==") == 0 || strcmp(node->binary.op, "!=") == 0))
    {
        char *t1 = infer_type(ctx, node->binary.left);
        int is_ptr = 0;
        char *fully_resolved = t1;
        char *mangle_base = t1;
        int found_opaque = 0;

        if (t1)
        {
            char *curr = t1;
            int depth = 0;
            while (depth++ < 20)
            {
                if (strchr(curr, '*'))
                {
                    is_ptr = 1;
                    break;
                }

                int resolved = 0;
                TypeAlias *ta = find_type_alias_node(ctx, curr);
                if (ta)
                {
                    if (ta->is_opaque)
                    {
                        if (!found_opaque)
                        {
                            mangle_base = ta->alias;
                            found_opaque = 1;
                        }
                    }
                    else if (!found_opaque)
                    {
                        mangle_base = ta->original_type;
                    }

                    curr = ta->original_type;
                    resolved = 1;
                }

                if (!resolved)
                {
                    break;
                }
            }
            fully_resolved = curr;
        }

        int is_basic = str_is_basic_type(fully_resolved);
        ASTNode *def = t1 ? find_struct_def(ctx, t1) : NULL;

        int is_simple_enum = 0;
        if (def && def->kind == NODE_ENUM)
        {
            is_simple_enum = 1;
            ASTNode *v = def->enm.variants;
            while (v)
            {
                if (v->variant.payload)
                {
                    is_simple_enum = 0;
                    break;
                }
                v = v->next;
            }
        }

        if (t1 && (def || found_opaque) && !is_basic && !is_ptr && !is_simple_enum)
        {
            char *base = mangle_base;
            if (strncmp(base, "struct ", 7) == 0)
            {
                base += 7;
            }

            if (strcmp(node->binary.op, "!=") == 0)
            {
                EMIT(ctx, "(!");
            }
            char meth[MAX_TYPE_NAME_LEN];
            snprintf(meth, sizeof(meth), "%s__Eq__eq", base);
            ZenSymbol *sym = find_symbol_in_all(ctx, meth);
            FuncSig *sig = sym ? sym->data.sig : NULL;
            if (!sig)
            {
                snprintf(meth, sizeof(meth), "%s__eq", base);
                sym = find_symbol_in_all(ctx, meth);
                sig = sym ? sym->data.sig : NULL;
            }

            const char *call_name = sig ? sig->name : NULL;
            if (!call_name)
            {
                snprintf(meth, sizeof(meth), "%s__Eq__eq", base);
                call_name = meth;
            }

            EMIT(ctx, "%s(", (sig && sig->link_name) ? sig->link_name : call_name);

            if (node->binary.left->kind == NODE_EXPR_VAR ||
                node->binary.left->kind == NODE_EXPR_INDEX ||
                node->binary.left->kind == NODE_EXPR_MEMBER)
            {
                EMIT(ctx, "&");
                codegen_expression(ctx, node->binary.left);
            }
            else if (ctx->config->use_cpp)
            {
                EMIT(ctx, "({ __typeof__((");
                codegen_expression(ctx, node->binary.left);
                EMIT(ctx, ")) _tmp = ");
                codegen_expression(ctx, node->binary.left);
                EMIT(ctx, "; &_tmp; })");
            }
            else
            {
                EMIT(ctx, "(__typeof__((");
                codegen_expression(ctx, node->binary.left);
                EMIT(ctx, "))[]){");
                codegen_expression(ctx, node->binary.left);
                EMIT(ctx, "}");
            }

            EMIT(ctx, ", ");

            int needs_ptr = 1;
            if (sig)
            {
                needs_ptr = (sig->total_args > 1 && sig->arg_types[1]->kind == TYPE_POINTER);
            }

            if (needs_ptr && (node->binary.right->kind == NODE_EXPR_VAR ||
                              node->binary.right->kind == NODE_EXPR_INDEX ||
                              node->binary.right->kind == NODE_EXPR_MEMBER))
            {
                EMIT(ctx, "&");
                codegen_expression(ctx, node->binary.right);
            }
            else if (needs_ptr && ctx->config->use_cpp)
            {
                EMIT(ctx, "({ __typeof__((");
                codegen_expression(ctx, node->binary.right);
                EMIT(ctx, ")) _tmp = ");
                codegen_expression(ctx, node->binary.right);
                EMIT(ctx, "; &_tmp; })");
            }
            else if (needs_ptr)
            {
                EMIT(ctx, "(__typeof__((");
                codegen_expression(ctx, node->binary.right);
                EMIT(ctx, "))[]){");
                codegen_expression(ctx, node->binary.right);
                EMIT(ctx, "}");
            }
            else
            {
                codegen_expression(ctx, node->binary.right);
            }

            EMIT(ctx, ")");
            if (strcmp(node->binary.op, "!=") == 0)
            {
                EMIT(ctx, ")");
            }
        }
        else if (t1 && (strcmp(t1, "string") == 0))
        {
            char *t2 = infer_type(ctx, node->binary.right);
            if (t2 && (strcmp(t2, "string") == 0))
            {
                EMIT(ctx, "(strcmp(");
                codegen_expression(ctx, node->binary.left);
                EMIT(ctx, ", ");
                codegen_expression(ctx, node->binary.right);
                if (strcmp(node->binary.op, "==") == 0)
                {
                    EMIT(ctx, ") == 0)");
                }
                else
                {
                    EMIT(ctx, ") != 0)");
                }
            }
            else
            {
                EMIT(ctx, "(");
                codegen_expression(ctx, node->binary.left);
                EMIT(ctx, " %s ", node->binary.op);
                codegen_expression(ctx, node->binary.right);
                EMIT(ctx, ")");
            }
        }
        else
        {
            EMIT(ctx, "(");
            codegen_expression(ctx, node->binary.left);
            EMIT(ctx, " %s ", node->binary.op);
            codegen_expression(ctx, node->binary.right);
            EMIT(ctx, ")");
        }
        if (t1)
        {
            zfree(t1);
        }
    }
    else if (strcmp(node->binary.op, "**") == 0)
    {
        EMIT(ctx, "(_zc_pow((double)(");
        codegen_expression(ctx, node->binary.left);
        EMIT(ctx, "), (double)(");
        codegen_expression(ctx, node->binary.right);
        EMIT(ctx, ")))");
    }
    else if (strcmp(node->binary.op, "**=") == 0)
    {
        EMIT(ctx, "({ ");
        codegen_expression(ctx, node->binary.left);
        EMIT(ctx, " = _zc_pow((double)(");
        codegen_expression(ctx, node->binary.left);
        EMIT(ctx, "), (double)(");
        codegen_expression(ctx, node->binary.right);
        EMIT(ctx, ")); ");
        codegen_expression(ctx, node->binary.left);
        EMIT(ctx, "; })");
    }
    else
    {
        int is_assignment =
            (node->binary.op[strlen(node->binary.op) - 1] == '=' &&
             strcmp(node->binary.op, "==") != 0 && strcmp(node->binary.op, "!=") != 0 &&
             strcmp(node->binary.op, "<=") != 0 && strcmp(node->binary.op, ">=") != 0);

        int is_drop_assignment = 0;
        char *clean_type = NULL;
        if (is_assignment && strcmp(node->binary.op, "=") == 0 && ctx->config->use_cpp &&
            node->binary.left->kind == NODE_EXPR_VAR)
        {
            char *type_name = infer_type(ctx, node->binary.left);
            if (type_name)
            {
                if (strchr(type_name, '*') == NULL)
                {
                    clean_type = xstrdup(type_name);
                    char *base = clean_type;
                    if (strncmp(base, "struct ", 7) == 0)
                    {
                        base += 7;
                    }
                    ASTNode *def = find_struct_def(ctx, base);
                    if (def && def->type_info && def->type_info->traits.has_drop)
                    {
                        is_drop_assignment = 1;
                        memmove(clean_type, base, strlen(base) + 1);
                    }
                }
                zfree(type_name);
            }
        }

        if (is_drop_assignment)
        {
            EMIT(ctx, "({ ");
            EMIT(ctx, "ZC_AUTO _z_tmp = (");
            codegen_expression_with_move(ctx, node->binary.right);
            EMIT(ctx, "); ");

            EMIT(ctx, "__typeof__((");
            codegen_expression(ctx, node->binary.left);
            EMIT(ctx, "))* _z_dest = &(");
            codegen_expression(ctx, node->binary.left);
            EMIT(ctx, "); ");

            if (node->binary.left->kind == NODE_EXPR_VAR)
            {
                EMIT(ctx, "if (__z_drop_flag_%s) %s__Drop__glue(_z_dest); ",
                     node->binary.left->var_ref.name, clean_type);
            }
            else
            {
                EMIT(ctx, "%s__Drop__glue(_z_dest); ", clean_type);
            }

            EMIT(ctx, "*_z_dest = _z_tmp; ");

            if (node->binary.left->kind == NODE_EXPR_VAR)
            {
                EMIT(ctx, "__z_drop_flag_%s = 1; ", node->binary.left->var_ref.name);
            }

            EMIT(ctx, "*_z_dest; })");
        }
        else
        {
            EMIT(ctx, "(");
            if (is_assignment)
            {
                codegen_expression(ctx, node->binary.left);
            }
            else
            {
                codegen_expression_with_move(ctx, node->binary.left);
            }

            EMIT(ctx, " %s ", node->binary.op);
            if (ctx->config->misra_mode ||
                (ctx->config->use_cpp && is_assignment && strcmp(node->binary.op, "=") == 0))
            {
                int should_cast = ctx->config->misra_mode;
                if (!should_cast && node->binary.left->type_info)
                {
                    TypeKind k = node->binary.left->type_info->kind;
                    if (k == TYPE_POINTER || k == TYPE_ENUM ||
                        is_enum_type_name(ctx, node->binary.left->type_info->name))
                    {
                        should_cast = 1;
                    }
                }

                if (should_cast)
                {
                    char *c_type = NULL;
                    if (node->binary.left->type_info)
                    {
                        c_type =
                            type_to_c_string(node->binary.left->type_info, ctx->config->misra_mode);
                    }
                    if (c_type && strcmp(c_type, "unknown") != 0 && strcmp(c_type, "void") != 0)
                    {
                        EMIT(ctx, "(%s)(", c_type);
                        codegen_expression_with_move(ctx, node->binary.right);
                        EMIT(ctx, ")");
                        zfree(c_type);
                    }
                    else
                    {
                        if (c_type)
                        {
                            zfree(c_type);
                        }
                        codegen_expression_with_move(ctx, node->binary.right);
                    }
                }
                else
                {
                    codegen_expression_with_move(ctx, node->binary.right);
                }
            }
            else
            {
                codegen_expression_with_move(ctx, node->binary.right);
            }
            EMIT(ctx, ")");
        }

        if (clean_type)
        {
            zfree(clean_type);
        }
    }
    if (ctx->config->misra_mode)
    {
        EMIT(ctx, ")");
    }
}

// Struct with Drop held by an argument that references a temporary the call owns.
static char *owned_temp_drop_type(ParserContext *ctx, ASTNode *arg)
{
    if (arg->kind != NODE_EXPR_UNARY || !arg->unary.owns_temp || arg->unary.temp_id > 0)
    {
        return NULL;
    }
    ASTNode *operand = arg->unary.operand;
    char *tname = NULL;
    if (operand->type_info && operand->type_info->kind == TYPE_STRUCT && operand->type_info->name)
    {
        tname = operand->type_info->name;
    }
    else
    {
        tname = infer_type(ctx, operand);
    }
    if (!tname || strchr(tname, '*'))
    {
        return NULL;
    }
    if (strncmp(tname, "struct ", 7) == 0)
    {
        tname += 7;
    }
    ASTNode *def = find_struct_def(ctx, tname);
    if (def && def->kind == NODE_STRUCT && def->type_info && def->type_info->traits.has_drop)
    {
        return tname;
    }
    return NULL;
}

// Emits a call that borrows temporaries with Drop (e.g. `a + make()` on an overloaded
// operator): each temporary gets a name, and is dropped once the call has returned.
static int emit_call_dropping_temporaries(ParserContext *ctx, ASTNode *node)
{
    int has_temp = 0;
    ASTNode *arg = node->call.args;
    while (arg)
    {
        if (owned_temp_drop_type(ctx, arg))
        {
            has_temp = 1;
        }
        arg = arg->next;
    }
    if (!has_temp || !node->type_info)
    {
        return 0;
    }

    int returns_value = node->type_info->kind != TYPE_VOID;
    EMIT(ctx, "({ ");
    arg = node->call.args;
    while (arg)
    {
        char *drop_type = owned_temp_drop_type(ctx, arg);
        if (drop_type)
        {
            int id = ctx->cg.tmp_counter++;
            EMIT(ctx, "%s _z_temp_%d = ", drop_type, id);
            codegen_expression(ctx, arg->unary.operand);
            EMIT(ctx, "; ");
            arg->unary.temp_id = id + 1;
        }
        arg = arg->next;
    }

    // ZC_AUTO_INIT, not ZC_AUTO: tcc has no __auto_type, so there it is __typeof__.
    int result_id = ctx->cg.tmp_counter++;
    if (returns_value)
    {
        EMIT(ctx, "ZC_AUTO_INIT(_z_result_%d, ", result_id);
    }
    handle_expr_call(ctx, node);
    EMIT(ctx, "%s", returns_value ? "); " : "; ");

    arg = node->call.args;
    while (arg)
    {
        if (arg->kind == NODE_EXPR_UNARY && arg->unary.temp_id > 0)
        {
            int id = arg->unary.temp_id - 1;
            arg->unary.temp_id = 0;
            EMIT(ctx, "%s__Drop__glue(&_z_temp_%d); ", owned_temp_drop_type(ctx, arg), id);
        }
        arg = arg->next;
    }
    if (returns_value)
    {
        EMIT(ctx, "_z_result_%d; ", result_id);
    }
    EMIT(ctx, "})");
    return 1;
}

// A call's name that is the Zen C's, though no function of that name is: an enum variant
// (Mode__default, Result_Ok), or a function reached through a module's alias
// (ws2__WSAStringToAddressA, an extern fn of the module imported as ws2).
static int zen_name_declared(ParserContext *ctx, const char *name)
{
    if (strcmp(name, "panic") == 0)
    {
        return 1; // a builtin (__zenc_panic)
    }
    static const char *const seps[] = {"__", "_"};
    for (int k = 0; k < 2; k++)
    {
        const char *at = NULL;
        for (const char *p = strstr(name, seps[k]); p; p = strstr(p + 1, seps[k]))
        {
            at = p;
        }
        if (!at || at == name)
        {
            continue;
        }
        char prefix[MAX_SHORT_MSG_LEN];
        size_t n = (size_t)(at - name);
        if (n >= sizeof(prefix))
        {
            continue;
        }
        memcpy(prefix, name, n);
        prefix[n] = 0;
        ASTNode *def = find_struct_def(ctx, prefix);
        if (def && def->kind == NODE_ENUM)
        {
            return 1;
        }
    }
    const char *sep = strstr(name, "__");
    if (sep && sep > name)
    {
        char alias[MAX_SHORT_MSG_LEN];
        size_t n = (size_t)(sep - name);
        if (n < sizeof(alias))
        {
            memcpy(alias, name, n);
            alias[n] = 0;
            const char *rest = sep + 2;
            if (zmap_get(&ctx->imports.modules, alias) &&
                (is_extern_symbol(ctx, rest) || c_name_known(ctx, rest) || find_func(ctx, rest)))
            {
                return 1;
            }
        }
    }
    return 0;
}

// Whether a call to `callee` (a name) is to something the Zen C or its C declares, but for
// the symbols in scope: the part of the check that needs no scopes.
static int c_names_call_known_globally(ParserContext *ctx, ASTNode *callee)
{
    const char *name = callee->var_ref.name;
    if (find_func(ctx, name) || find_func_template(ctx, name) || find_struct_def(ctx, name) ||
        c_name_known(ctx, name) || find_symbol_in_all(ctx, name) || zen_name_declared(ctx, name) ||
        is_extern_symbol(ctx, name) || strncmp(name, "_z_", 3) == 0 || strncmp(name, "_Z", 2) == 0)
    {
        return 1;
    }
    for (char **w = ctx->config->c_function_whitelist; w && *w; w++)
    {
        if (strcmp(*w, name) == 0)
        {
            return 1;
        }
    }
    return callee->type_info && get_inner_type(callee->type_info)->kind == TYPE_FUNCTION;
}

// With the names the program's C declares known: whether a call to `callee` (a name) is to
// something the Zen C or its C declares (during codegen, with its scopes).
int c_names_call_known(ParserContext *ctx, ASTNode *callee)
{
    return c_names_call_known_globally(ctx, callee) || find_symbol_entry(ctx, callee->var_ref.name);
}

int c_names_call_known_parsed(ParserContext *ctx, ASTNode *callee)
{
    // A name with "__" is the Zen C's own (Type::method, module::name): zc resolves those
    // after the parse (an alias's or a module's methods), and C names don't look so.
    return callee->var_ref.bound || strstr(callee->var_ref.name, "__") ||
           c_names_call_known_globally(ctx, callee);
}

void c_names_report_call(ParserContext *ctx, ASTNode *callee)
{
    if (ctx->config->mode_lsp)
    {
        return; // the LSP reports these itself (lsp_c_decls.c)
    }
    char msg[MAX_SHORT_MSG_LEN], mean[MAX_SHORT_MSG_LEN];
    snprintf(msg, sizeof(msg), "Undefined function '%s'", callee->var_ref.name);
    const char *close = c_name_closest(ctx, callee->var_ref.name);
    if (close)
    {
        snprintf(mean, sizeof(mean), "Did you mean '%s'?", close);
    }
    const char *hints[] = {close ? mean
                                 : "Not declared here, nor by an included C header "
                                   "(ZC_NO_C_NAMES=1 skips this check)",
                           NULL};
    zerror_with_hints(callee->token, msg, hints);
}

// Where C declares `d`, as a hint: "declared in wgf_vec3.h:24: wgf_vec3_t wgf_vec3_make(...)".
static const char *c_decls_where(const CDecl *d, char *buf, size_t n)
{
    const char *file = d->file ? d->file : "";
    const char *slash = strrchr(file, '/');
    snprintf(buf, n, "Declared in %s:%d: %s", slash ? slash + 1 : file, d->line,
             d->text ? d->text : d->name);
    return buf;
}

// With the program's C read: a call to a function the Zen C declares (an `extern fn`, as a
// binding's, or its own) whose parameter is a C enum gets that enum, or a plain int (as in
// C): not another C enum, as for a function its C declares (below; task 34).
static void c_decls_check_sig_enums(ParserContext *ctx, ASTNode *node, FuncSig *sig,
                                    const char *name)
{
    int i = 0;
    for (ASTNode *a = node->call.args; a && i < sig->total_args; a = a->next, i++)
    {
        if (!sig->arg_types || !sig->arg_types[i])
        {
            continue;
        }
        char *param_c = type_to_c_string(sig->arg_types[i], 0);
        const char *want = param_c ? c_decl_enum_of(ctx, param_c) : NULL;
        zfree(param_c);
        if (!want)
        {
            continue;
        }
        const char *have_type = infer_type(ctx, a);
        const char *have = have_type ? c_decl_enum_of(ctx, have_type) : NULL;
        if (!have || strcmp(have, want) == 0)
        {
            continue;
        }
        char msg[MAX_SHORT_MSG_LEN];
        snprintf(msg, sizeof(msg), "Argument %d of '%s' is a '%s', but the parameter is a '%s'",
                 i + 1, name, have, want);
        zerror_at(a->token, "%s", msg);
    }
}

// With the program's C read: a call to a function its C declares (and the Zen C doesn't)
// gets as many arguments as the function takes, and an argument of one C enum isn't passed
// for a parameter of another (a plain int is, as in C).
void c_decls_check_call(ParserContext *ctx, ASTNode *node)
{
    ASTNode *callee = node->call.callee;
    if (!ctx->c_names_ready || ctx->config->mode_lsp || !callee || callee->kind != NODE_EXPR_VAR)
    {
        return;
    }
    const char *name = callee->var_ref.name;
    FuncSig *sig = find_func(ctx, name);
    if (sig)
    {
        c_decls_check_sig_enums(ctx, node, sig, name);
        return;
    }
    const CDecl *d = c_decl_find(ctx, name);
    if (!d || d->kind != CD_FUNCTION || find_func(ctx, name) || find_struct_def(ctx, name) ||
        find_symbol_entry(ctx, name) || find_symbol_in_all(ctx, name) ||
        zen_name_declared(ctx, name))
    {
        return; // (an `extern fn` is a signature of the Zen C's: find_func has it)
    }
    int given = 0;
    for (ASTNode *a = node->call.args; a; a = a->next)
    {
        given++;
    }
    char msg[MAX_SHORT_MSG_LEN], where[MAX_SHORT_MSG_LEN];
    if (d->params_known && (d->variadic ? given < d->nparams : given != d->nparams))
    {
        snprintf(msg, sizeof(msg), "'%s' takes %s%d argument%s, but %d %s given", name,
                 d->variadic ? "at least " : "", d->nparams, d->nparams == 1 ? "" : "s", given,
                 given == 1 ? "was" : "were");
        const char *hints[] = {c_decls_where(d, where, sizeof(where)), NULL};
        zerror_with_hints(callee->token, msg, hints);
        return;
    }
    int i = 0;
    for (ASTNode *a = node->call.args; a && i < d->nparams; a = a->next, i++)
    {
        const char *want = c_decl_enum_of(ctx, d->params[i].type);
        if (!want)
        {
            continue;
        }
        const char *have_type = infer_type(ctx, a);
        const char *have = have_type ? c_decl_enum_of(ctx, have_type) : NULL;
        if (!have || strcmp(have, want) == 0)
        {
            continue;
        }
        snprintf(msg, sizeof(msg), "Argument %d of '%s' is a '%s', but the parameter is a '%s'",
                 i + 1, name, have, want);
        const char *hints[] = {c_decls_where(d, where, sizeof(where)), NULL};
        zerror_with_hints(a->token, msg, hints);
    }
}

// With the program's C read: `target.field` where target is a C struct or union whose
// fields are known names one of them.
// `target.field` where target is a struct the Zen C defines, all of whose fields it knows,
// names one of them, or a method (a build reported it only in the C, or not at all: task 26).
// Not a generic's template, a struct with `use`d structs or a parent (fields from elsewhere),
// a @crepr one (a C struct, of which it lists some fields), or one with an unnamed member.
static void zen_check_member(ParserContext *ctx, ASTNode *node, ASTNode *def, const char *sname)
{
    const char *field = node->member.field;
    if (isdigit((unsigned char)field[0]))
    {
        return; // a tuple's element (t.0), which codegen names itself
    }
    if (def->kind != NODE_STRUCT || def->strct.is_template || def->strct.used_struct_count ||
        def->strct.parent || def->strct.is_incomplete || def->strct.is_opaque ||
        def->strct.crepr_c_type)
    {
        return;
    }
    const char *best = NULL;
    int best_d = 3;
    for (ASTNode *f = def->strct.fields; f; f = f->next)
    {
        if (!f->field.name)
        {
            return;
        }
        if (strcmp(f->field.name, field) == 0)
        {
            return;
        }
        int dist = levenshtein(field, f->field.name);
        if (dist < best_d)
        {
            best_d = dist;
            best = f->field.name;
        }
    }
    // (a method: Struct__name, or a trait's, Struct__Trait__name)
    char buf[MAX_ERROR_MSG_LEN];
    snprintf(buf, sizeof(buf), "%s__%s", sname, field);
    char *mangled = merge_underscores(buf);
    int method = find_func(ctx, mangled) != NULL;
    zfree(mangled);
    for (StructRef *ref = ctx->parsed_impls_list; ref && !method; ref = ref->next)
    {
        if (ref->node && ref->node->kind == NODE_IMPL_TRAIT && ref->node->impl_trait.target_type &&
            strcmp(ref->node->impl_trait.target_type, sname) == 0)
        {
            snprintf(buf, sizeof(buf), "%s__%s__%s", sname, ref->node->impl_trait.trait_name,
                     field);
            char *trait_mangled = merge_underscores(buf);
            method = find_func(ctx, trait_mangled) != NULL;
            zfree(trait_mangled);
        }
    }
    if (method)
    {
        return;
    }
    char msg[MAX_TYPE_NAME_LEN + 512], help[MAX_SHORT_MSG_LEN];
    snprintf(msg, sizeof(msg), "'%s' has no field '%.255s'", sname, field);
    if (best)
    {
        snprintf(help, sizeof(help), "Did you mean '%s'?", best);
    }
    else
    {
        size_t k = (size_t)snprintf(help, sizeof(help), "Its fields:");
        for (ASTNode *f = def->strct.fields; f && k < sizeof(help) - 1; f = f->next)
        {
            k += (size_t)snprintf(help + k, sizeof(help) - k, " %s", f->field.name);
        }
    }
    const char *hints[] = {help, NULL};
    zerror_with_hints(node->token, msg, hints);
}

void c_decls_check_member(ParserContext *ctx, ASTNode *node, const char *target_type)
{
    if (ctx->config->mode_lsp || !target_type || !node->member.field)
    {
        return;
    }
    char base[MAX_TYPE_NAME_LEN];
    snprintf(base, sizeof(base), "%s", target_type);
    char *star = strchr(base, '*');
    if (star)
    {
        *star = 0;
    }
    size_t blen = strlen(base);
    while (blen && base[blen - 1] == ' ')
    {
        base[--blen] = 0;
    }
    ASTNode *zen_def = find_struct_def(ctx, base);
    if (zen_def)
    {
        zen_check_member(ctx, node, zen_def, base); // the Zen C's own
        return;
    }
    if (!ctx->c_names_ready)
    {
        return;
    }
    const CDecl *agg = c_decl_aggregate(ctx, base);
    const char *field = node->member.field;
    if (!agg || c_decl_field(agg, field))
    {
        return;
    }
    const char *best = NULL;
    int best_d = 3;
    for (int i = 0; i < agg->nfields; i++)
    {
        int dist = agg->fields[i].name ? levenshtein(field, agg->fields[i].name) : 99;
        if (dist < best_d)
        {
            best_d = dist;
            best = agg->fields[i].name;
        }
    }
    size_t len = strlen(base);
    while (len && base[len - 1] == ' ')
    {
        base[--len] = 0;
    }
    char msg[MAX_TYPE_NAME_LEN + 512], help[MAX_SHORT_MSG_LEN], where[MAX_SHORT_MSG_LEN];
    snprintf(msg, sizeof(msg), "'%s' has no field '%.255s'", base, field);
    if (best)
    {
        snprintf(help, sizeof(help), "Did you mean '%s'?", best);
    }
    else
    {
        size_t k = (size_t)snprintf(help, sizeof(help), "Its fields:");
        for (int i = 0; i < agg->nfields && k < sizeof(help) - 1; i++)
        {
            k += (size_t)snprintf(help + k, sizeof(help) - k, " %s", agg->fields[i].name);
        }
    }
    const char *hints[] = {help, c_decls_where(agg, where, sizeof(where)), NULL};
    zerror_with_hints(node->token, msg, hints);
}

void handle_expr_call(ParserContext *ctx, ASTNode *node)
{
    if (emit_call_dropping_temporaries(ctx, node))
    {
        return;
    }

    emit_source_mapping(ctx, node);

    if (node->call.callee->kind == NODE_EXPR_MEMBER)
    {
        Type *callee_ti = get_inner_type(node->call.callee->type_info);
        if (callee_ti && callee_ti->kind == TYPE_FUNCTION)
        {
            goto skip_method_mangling;
        }

        ASTNode *target = node->call.callee->member.target;
        char *method = node->call.callee->member.field;

        if (strcmp(method, "len") == 0)
        {
            if (target->type_info && target->type_info->kind == TYPE_ARRAY)
            {
                if (target->type_info->array_size > 0)
                {
                    EMIT(ctx, "%d", target->type_info->array_size);
                }
                else
                {
                    codegen_expression(ctx, target);
                    EMIT(ctx, ".len");
                }
                return;
            }
        }

        if (target->kind == NODE_EXPR_VAR)
        {
            char type_name[MAX_TYPE_NAME_LEN];
            strncpy(type_name, target->var_ref.name, sizeof(type_name));
            type_name[sizeof(type_name) - 1] = 0;

            char *mangled_type = type_name;

            ASTNode *def = find_struct_def(ctx, mangled_type);
            if (def && def->kind == NODE_ENUM)
            {
                char mangled[MAX_MANGLED_NAME_LEN];
                const char *ename_for_mangling = (def->link_name) ? def->link_name : mangled_type;
                snprintf(mangled, sizeof(mangled), "%s__%s", ename_for_mangling, method);
                FuncSig *sig = find_func(ctx, mangled);
                if (sig)
                {
                    const char *emit_name = (sig->link_name) ? sig->link_name : mangled;
                    EMIT(ctx, "%s(", emit_name);
                    ASTNode *arg = node->call.args;
                    int arg_idx = 0;
                    while (arg)
                    {
                        if (arg_idx > 0)
                        {
                            EMIT(ctx, ", ");
                        }

                        Type *param_t =
                            (arg_idx < sig->total_args) ? sig->arg_types[arg_idx] : NULL;

                        if (param_t && param_t->kind == TYPE_STRUCT &&
                            strncmp(param_t->name, "Tuple__", 7) == 0 && sig->total_args == 1 &&
                            node->call.count > 1)
                        {
                            EMIT(ctx, "(%s){", param_t->name);
                            int first = 1;
                            while (arg)
                            {
                                if (!first)
                                {
                                    EMIT(ctx, ", ");
                                }
                                first = 0;
                                codegen_expression(ctx, arg);
                                arg = arg->next;
                            }
                            EMIT(ctx, "}");
                            break;
                        }

                        codegen_expression(ctx, arg);
                        arg = arg->next;
                        arg_idx++;
                    }
                    EMIT(ctx, ")");
                    return;
                }
            }
        }

        char *type = infer_type(ctx, target);
        if (type)
        {
            char *clean = xstrdup(type);
            char *ptr = (char *)strchr(clean, '*');
            if (ptr)
            {
                *ptr = '\0';
            }

            char *base = clean;
            if (strncmp(base, "struct ", 7) == 0)
            {
                base += 7;
            }

            if (ctx)
            {
                TypeAlias *ta = find_type_alias_node(ctx, base);
                const char *alias = (ta && !ta->is_opaque) ? ta->original_type : NULL;
                if (alias)
                {
                    base = (char *)alias;
                }
            }

            const char *normalized = normalize_type_name(base);
            char *mangled_base = (char *)normalized;
            char base_buf[MAX_ERROR_MSG_LEN];

            char *lt = (char *)strchr(base, '<');
            if (lt)
            {
                char *gt = (char *)strrchr(base, '>');
                if (gt)
                {
                    ptrdiff_t prefix_len = lt - base;
                    char prefix[MAX_TYPE_NAME_LEN];
                    if (prefix_len >= 255)
                    {
                        prefix_len = 255;
                    }
                    strncpy(prefix, base, (size_t)(prefix_len));
                    prefix[prefix_len] = 0;

                    char *p_end = prefix + strlen(prefix);
                    while (p_end > prefix && *(p_end - 1) == '_')
                    {
                        *(--p_end) = '\0';
                    }

                    char *args_ptr = xstrdup(lt + 1);
                    char *args_end = (char *)strrchr(args_ptr, '>');
                    if (args_end)
                    {
                        *args_end = 0;
                    }

                    char *clean_arg = sanitize_mangled_name(args_ptr);
                    snprintf(base_buf, sizeof(base_buf), "%s__%s", prefix, clean_arg);
                    mangled_base = base_buf;

                    zfree(args_ptr);
                    zfree(clean_arg);
                }
            }

            if (!strchr(type, '*') &&
                (target->kind == NODE_EXPR_CALL || target->kind == NODE_EXPR_LITERAL ||
                 target->kind == NODE_EXPR_BINARY || target->kind == NODE_EXPR_UNARY ||
                 target->kind == NODE_EXPR_CAST || target->kind == NODE_EXPR_STRUCT_INIT))
            {
                char *type_mangled = (char *)normalize_type_name(type);
                if (type_mangled != type)
                {
                    mangled_base = type_mangled;
                }

                char type_buf[MAX_ERROR_MSG_LEN];
                char *t_lt = (char *)strchr(type, '<');

                if (t_lt)
                {
                    char *t_gt = (char *)strrchr(type, '>');
                    if (t_gt)
                    {
                        ptrdiff_t p_len = t_lt - type;
                        char prefix[MAX_TYPE_NAME_LEN];
                        if (p_len >= 255)
                        {
                            p_len = 255;
                        }
                        strncpy(prefix, type, (size_t)(p_len));
                        prefix[p_len] = 0;

                        char *p_end = prefix + strlen(prefix);
                        while (p_end > prefix && *(p_end - 1) == '_')
                        {
                            *(--p_end) = '\0';
                        }

                        char *args_ptr = xstrdup(t_lt + 1);
                        char *args_end = (char *)strrchr(args_ptr, '>');
                        if (args_end)
                        {
                            *args_end = 0;
                        }

                        char *clean_arg = sanitize_mangled_name(args_ptr);
                        snprintf(type_buf, sizeof(type_buf), "%s__%s", prefix, clean_arg);
                        type_mangled = type_buf;

                        zfree(args_ptr);
                        zfree(clean_arg);
                    }
                }

                emit_mangled_name(ctx, mangled_base, method);
                EMIT(ctx, "(");
                if (ctx->config->use_cpp)
                {
                    EMIT(ctx, "({ __typeof__((");
                    codegen_expression(ctx, target);
                    EMIT(ctx, ")) _tmp = ");
                    codegen_expression(ctx, target);
                    EMIT(ctx, "; &_tmp; })");
                }
                else
                {
                    EMIT(ctx, "((%s[]){", type_mangled);
                    codegen_expression(ctx, target);
                    EMIT(ctx, "})");
                }
                ASTNode *arg = node->call.args;
                while (arg)
                {
                    EMIT(ctx, ", ");
                    codegen_expression_with_move(ctx, arg);
                    arg = arg->next;
                }
                EMIT(ctx, ")");
            }
            else
            {
                char *call_base = mangled_base;

                int need_cast = 0;
                char mixin_func_base[MAX_MANGLED_NAME_LEN * 2];
                snprintf(mixin_func_base, sizeof(mixin_func_base), "%s__%s", call_base, method);
                char *mixin_func_name_ptr = merge_underscores(mixin_func_base);
                char mixin_func_name[MAX_MANGLED_NAME_LEN * 2];
                strncpy(mixin_func_name, mixin_func_name_ptr, sizeof(mixin_func_name) - 1);
                mixin_func_name[sizeof(mixin_func_name) - 1] = 0;
                zfree(mixin_func_name_ptr);

                char *resolved_method_suffix = NULL;

                if (!find_func(ctx, mixin_func_name))
                {
                    TypeAlias *ta = ctx->type_aliases;
                    while (ta)
                    {
                        if (strcmp(ta->original_type, call_base) == 0)
                        {
                            char alias_func_base[MAX_ERROR_MSG_LEN];
                            snprintf(alias_func_base, sizeof(alias_func_base), "%s__%s", ta->alias,
                                     method);
                            char *alias_func_name = merge_underscores(alias_func_base);
                            if (find_func(ctx, alias_func_name))
                            {
                                zfree(alias_func_name);
                                break;
                            }
                            zfree(alias_func_name);
                        }
                        ta = ta->next;
                    }
                    StructRef *ref = ctx->parsed_impls_list;
                    while (ref)
                    {
                        if (ref->node && ref->node->kind == NODE_IMPL_TRAIT &&
                            strcmp(ref->node->impl_trait.target_type, base) == 0)
                        {
                            char trait_base[MAX_MANGLED_NAME_LEN];
                            snprintf(trait_base, sizeof(trait_base), "%s__%s__%s", base,
                                     ref->node->impl_trait.trait_name, method);
                            char *trait_mangled = merge_underscores(trait_base);
                            if (find_func(ctx, trait_mangled))
                            {
                                char suffix_base[MAX_MANGLED_NAME_LEN];
                                snprintf(suffix_base, sizeof(suffix_base), "%s__%s",
                                         ref->node->impl_trait.trait_name, method);
                                resolved_method_suffix = merge_underscores(suffix_base);
                                zfree(trait_mangled);
                                break;
                            }
                            zfree(trait_mangled);
                        }
                        ref = ref->next;
                    }

                    if (!resolved_method_suffix)
                    {
                        GenericImplTemplate *it = ctx->impl_templates;
                        while (it)
                        {
                            char *tname = NULL;
                            if (it->impl_node && it->impl_node->kind == NODE_IMPL_TRAIT)
                            {
                                tname = it->impl_node->impl_trait.trait_name;
                                char trait_base[MAX_ERROR_MSG_LEN];
                                snprintf(trait_base, sizeof(trait_base), "%s__%s__%s", base, tname,
                                         method);
                                char *trait_mangled = merge_underscores(trait_base);
                                if (find_func(ctx, trait_mangled))
                                {
                                    char suffix_base[MAX_ERROR_MSG_LEN];
                                    snprintf(suffix_base, sizeof(suffix_base), "%s__%s", tname,
                                             method);
                                    resolved_method_suffix = merge_underscores(suffix_base);
                                    zfree(trait_mangled);
                                    break;
                                }
                                zfree(trait_mangled);
                            }
                            it = it->next;
                        }
                    }

                    if (resolved_method_suffix)
                    {
                        method = resolved_method_suffix;
                    }
                    else
                    {
                        ASTNode *def = find_struct_def(ctx, base);
                        if (def && def->kind == NODE_STRUCT && def->strct.used_structs)
                        {
                            for (int k = 0; k < def->strct.used_struct_count; k++)
                            {
                                char mixin_base[MAX_ERROR_MSG_LEN];
                                snprintf(mixin_base, sizeof(mixin_base), "%s__%s",
                                         def->strct.used_structs[k], method);
                                char *mixin_check = merge_underscores(mixin_base);
                                if (find_func(ctx, mixin_check))
                                {
                                    call_base = def->strct.used_structs[k];
                                    need_cast = 1;
                                    zfree(mixin_check);
                                    break;
                                }
                                zfree(mixin_check);
                            }
                        }
                    }
                }

                emit_mangled_name(ctx, call_base, method);
                EMIT(ctx, "(");
                if (need_cast)
                {
                    EMIT(ctx, "(%s*)%s", call_base, strchr(type, '*') ? "" : "&");
                }
                else if (!strchr(type, '*'))
                {
                    EMIT(ctx, "&");
                }
                codegen_expression(ctx, target);
                ASTNode *arg = node->call.args;
                while (arg)
                {
                    EMIT(ctx, ", ");
                    codegen_expression_with_move(ctx, arg);
                    arg = arg->next;
                }
                EMIT(ctx, ")");

                if (resolved_method_suffix)
                {
                    zfree(resolved_method_suffix);
                }
            }
            zfree(clean);
            zfree(type);
            return;
        }
        if (type)
        {
            zfree(type);
        }
    }

skip_method_mangling:

    if (node->call.callee->kind == NODE_EXPR_VAR)
    {
        ASTNode *def = find_struct_def(ctx, node->call.callee->var_ref.name);
        if (def && def->kind == NODE_STRUCT)
        {
            EMIT(ctx, "(struct %s){0}", node->call.callee->var_ref.name);
            return;
        }
    }

    Type *callee_ti = get_inner_type(node->call.callee->type_info);
    if (callee_ti && callee_ti->kind == TYPE_FUNCTION && !callee_ti->is_raw)
    {
        EMIT(ctx, "({ z_closure_T _c = ");
        codegen_expression(ctx, node->call.callee);
        EMIT(ctx, "; ");

        Type *ft = callee_ti;
        char *ret = type_to_c_string(ft->inner, ctx->config->misra_mode);
        if (strcmp(ret, "string") == 0)
        {
            zfree(ret);
            ret = xstrdup("char*");
        }
        if (strcmp(ret, "unknown") == 0)
        {
            zfree(ret);
            ret = xstrdup("void*");
        }

        EMIT(ctx, "((%s (*)(void*", ret);
        for (int i = 0; i < ft->count; i++)
        {
            char *as = type_to_c_string(ft->args[i], ctx->config->misra_mode);
            if (strcmp(as, "unknown") == 0)
            {
                zfree(as);
                as = xstrdup("void*");
            }
            EMIT(ctx, ", %s", as);
            zfree(as);
        }
        if (ft->is_varargs)
        {
            EMIT(ctx, ", ...");
        }
        EMIT(ctx, "))_c.func)(_c.ctx");

        ASTNode *arg = node->call.args;
        while (arg)
        {
            EMIT(ctx, ", ");
            codegen_expression_with_move(ctx, arg);
            arg = arg->next;
        }
        EMIT(ctx, "); })");
        zfree(ret);
        return;
    }

    if (node->call.callee->kind == NODE_EXPR_VAR &&
        strcmp(node->call.callee->var_ref.name, "panic") == 0)
    {
        EMIT(ctx, "__zenc_panic");
        goto skip_callee_gen;
    }
    else if (node->call.callee->kind == NODE_EXPR_VAR)
    {
        char *name = node->call.callee->var_ref.name;
        char *underscore = (char *)strchr(name, '_');
        if (underscore && underscore != name && *(underscore + 1) != '_' &&
            strstr(name, "__") == NULL)
        {
            char base[MAX_TYPE_NAME_LEN];
            size_t len = (size_t)(underscore - name);
            if (len < sizeof(base))
            {
                strncpy(base, name, (size_t)(len));
                base[len] = 0;
                ASTNode *def = find_struct_def(ctx, base);
                int is_common_enum =
                    (strncmp(base, "Result", 6) == 0 || strncmp(base, "Option", 6) == 0 ||
                     strncmp(base, "JsonType", 8) == 0);
                if (is_common_enum || (def && def->kind == NODE_ENUM))
                {
                    emit_mangled_name(ctx, base, underscore + 1);
                    goto skip_callee_gen;
                }
            }
        }
    }

    g_emitting_callee = 1;
    codegen_expression(ctx, node->call.callee);
    g_emitting_callee = 0;
skip_callee_gen:
    EMIT(ctx, "(");

    if (node->call.arg_names && node->call.callee->kind == NODE_EXPR_VAR)
    {
        ASTNode *arg = node->call.args;
        int first = 1;
        while (arg)
        {
            if (!first)
            {
                EMIT(ctx, ", ");
            }
            first = 0;
            codegen_expression_with_move(ctx, arg);
            arg = arg->next;
        }
    }
    else
    {
        FuncSig *sig = NULL;
        if (node->call.callee->kind == NODE_EXPR_VAR)
        {
            sig = find_func(ctx, node->call.callee->var_ref.name);
            if (sig)
            {
                c_decls_check_call(ctx, node); // (its C enum parameters: an extern fn's)
            }
            if (!sig && !find_struct_def(ctx, node->call.callee->var_ref.name))
            {
                const char *name = node->call.callee->var_ref.name;
                c_decls_check_call(ctx, node);

                // (the LSP checks C names itself, after the parse: lsp_c_decls.c)
                int c_names_on = ctx->c_names_ready && !ctx->config->mode_lsp;
                int has_c_interop = ctx->has_external_includes;
                // With the names the program's C declares known, only those are C's; a
                // symbol (a local holding a function, say) is the Zen C's.
                if (c_names_on)
                {
                    has_c_interop = c_name_known(ctx, name) || find_symbol_entry(ctx, name) ||
                                    find_symbol_in_all(ctx, name) || zen_name_declared(ctx, name);
                }

                if (!has_c_interop && !c_names_on)
                {
                    zmap_iter_ModMap it = zmap_iter_init(ModMap, &ctx->imports.modules);
                    const char *alias;
                    Module *mod;
                    while (zmap_iter_next(&it, &alias, &mod))
                    {
                        if (mod->is_c_header)
                        {
                            has_c_interop = 1;
                            break;
                        }
                    }
                }

                if (!has_c_interop && !c_names_on)
                {
                    zmap_iter_FileSet it = zmap_iter_init(FileSet, &ctx->imports.imported_files);
                    const char *key;
                    const char *val;
                    while (zmap_iter_next(&it, &key, &val))
                    {
                        if (key && strstr(key, "/std/"))
                        {
                            has_c_interop = 1;
                            break;
                        }
                    }
                }

                int is_internal = strncmp(name, "_z_", 3) == 0 || strncmp(name, "_Z", 2) == 0;
                int is_extern = is_extern_symbol(ctx, name);
                int is_whitelisted = 0;
                if (ctx->config->c_function_whitelist)
                {
                    char **w = ctx->config->c_function_whitelist;
                    while (*w)
                    {
                        if (strcmp(*w, name) == 0)
                        {
                            is_whitelisted = 1;
                            break;
                        }
                        w++;
                    }
                }

                if (!has_c_interop && !is_internal && !is_extern && !is_whitelisted &&
                    !(node->call.callee->type_info &&
                      get_inner_type(node->call.callee->type_info)->kind == TYPE_FUNCTION))
                {
                    Token t = node->call.callee->token;
                    char msg[MAX_SHORT_MSG_LEN];
                    snprintf(msg, sizeof(msg), "Undefined function '%s'", name);
                    const char *hint = get_missing_function_hint(ctx, name);
                    char mean[MAX_SHORT_MSG_LEN];
                    const char *close = c_names_on ? c_name_closest(ctx, name) : NULL;
                    if (close)
                    {
                        snprintf(mean, sizeof(mean), "Did you mean '%s'?", close);
                        hint = mean;
                    }
                    if (c_names_on)
                    {
                        // Checked against what C declares: not a guess, so an error (the C
                        // compiler or the linker would stop on it too, less clearly).
                        const char *hints[] = {hint ? hint
                                                    : "Not declared here, nor by an included "
                                                      "C header (ZC_NO_C_NAMES=1 skips this "
                                                      "check)",
                                               NULL};
                        zerror_with_hints(t, msg, hints);
                    }
                    else
                    {
                        zwarn_diag(DIAG_INTEROP_UNDEF_FUNC, t, msg, hint);
                    }
                }
            }
        }

        ASTNode *arg = node->call.args;
        int arg_idx = 0;
        while (arg)
        {
            int handled = 0;
            if (sig && arg_idx < sig->total_args)
            {
                Type *param_t = sig->arg_types[arg_idx];
                Type *arg_t = arg->type_info;

                if (param_t && param_t->kind == TYPE_ARRAY && param_t->array_size == 0 && arg_t &&
                    arg_t->kind == TYPE_ARRAY && arg_t->array_size > 0)
                {
                    char *inner = type_to_c_string(param_t->inner, ctx->config->misra_mode);
                    EMIT(ctx, "(Slice__%s){.data = ", inner);
                    codegen_expression(ctx, arg);
                    EMIT(ctx, ", .len = %d, .cap = %d}", arg_t->array_size, arg_t->array_size);
                    zfree(inner);
                    handled = 1;
                }
                else if (param_t && param_t->kind == TYPE_STRUCT &&
                         strncmp(param_t->name, "Tuple__", 7) == 0 && sig->total_args == 1 &&
                         node->call.count > 1)
                {
                    EMIT(ctx, "(%s){", param_t->name);
                    ASTNode *curr = arg;
                    int first_field = 1;
                    while (curr)
                    {
                        if (!first_field)
                        {
                            EMIT(ctx, ", ");
                        }
                        first_field = 0;
                        codegen_expression_with_move(ctx, curr);
                        curr = curr->next;
                    }
                    EMIT(ctx, "}");
                    handled = 1;
                    arg = NULL;
                }
            }

            if (handled)
            {
                if (arg == NULL)
                {
                    break;
                }
            }
            else
            {
                if (arg->kind == NODE_RAW_STMT)
                {
                    int is_fmt_arg = 0;
                    if (sig && sig->is_varargs && arg_idx == sig->total_args - 1)
                    {
                        is_fmt_arg = 1;
                    }
                    else if (node->call.callee->kind == NODE_EXPR_VAR)
                    {
                        const char *callee_name = node->call.callee->var_ref.name;
                        is_fmt_arg = (strcmp(callee_name, "printf") == 0 && arg_idx == 0) ||
                                     (strcmp(callee_name, "fprintf") == 0 && arg_idx == 1) ||
                                     (strcmp(callee_name, "sprintf") == 0 && arg_idx == 1) ||
                                     (strcmp(callee_name, "snprintf") == 0 && arg_idx == 2) ||
                                     (strcmp(callee_name, "dprintf") == 0 && arg_idx == 1);
                    }
                    if (is_fmt_arg)
                    {
                        EMIT(ctx, "\"%%s\", ");
                    }
                }
                if (ctx->config->use_cpp && sig && arg_idx < sig->total_args)
                {
                    Type *param_t = sig->arg_types[arg_idx];
                    if (param_t && (param_t->kind == TYPE_POINTER || param_t->kind == TYPE_ENUM))
                    {
                        char *c_type = type_to_c_string(param_t, ctx->config->misra_mode);
                        EMIT(ctx, "(%s)(", c_type);
                        codegen_expression_with_move(ctx, arg);
                        EMIT(ctx, ")");
                        zfree(c_type);
                    }
                    else
                    {
                        codegen_expression_with_move(ctx, arg);
                    }
                }
                else
                {
                    codegen_expression_with_move(ctx, arg);
                }
            }

            if (arg && arg->next)
            {
                EMIT(ctx, ", ");
            }
            if (arg)
            {
                arg = arg->next;
            }
            arg_idx++;
        }
    }
    EMIT(ctx, ")");
}
