// SPDX-License-Identifier: MIT
#ifndef ZC_C_DECLS_H
#define ZC_C_DECLS_H

// The declarations of the C a program includes, read from the C preprocessor's output with
// tree-sitter's C grammar (utils_c_decls.c): what each name is, its C type, a function's
// parameters, a struct's fields, an enum constant's enum, and where it is declared. Types
// are C's, as text ("const char *", "struct wgf_vec3"), for the C zc writes and for what it
// shows (hover); zc's own types aren't made from them.

struct ParserContext;

typedef enum
{
    CD_FUNCTION = 1,
    CD_VARIABLE,
    CD_TYPEDEF,
    CD_STRUCT, // a tag: "struct NAME" in the table
    CD_UNION,  // "union NAME"
    CD_ENUM,   // "enum NAME"
    CD_ENUM_CONSTANT,
    CD_MACRO
} CDeclKind;

typedef struct
{
    char *name; // NULL for an unnamed parameter
    char *type; // C's spelling, without the name ("int", "char *", "int (*)(void *)")
} CDeclField;

typedef struct CDecl
{
    CDeclKind kind;
    char *name;
    char *file; // the header it is declared in, as the preprocessor names it (or NULL)
    int line;
    int end_line; // the declaration's last line (in `file`)
    char *text;   // the declaration, on one line, for showing it (hover)
    // A variable's or typedef's type; a function's return type; an enum constant's enum
    // ("enum wgf_actor_kind", or the typedef that names an anonymous one; "int" otherwise).
    char *type;
    CDeclField *params; // a function's
    int nparams;
    int variadic;       // ends with ...
    int params_known;   // the parameters were written (not `f()`, which says nothing)
    CDeclField *fields; // a struct's or union's (also a typedef of one): anonymous members
    int nfields;        // flattened in, as C reaches their fields directly
    int complete;       // the fields were read whole (no parse error, a body seen)
    char *aggregate;    // a typedef of a struct or union: "struct NAME", or "" if anonymous
} CDecl;

/// Reads the preprocessed C `text` (with its # lines: line markers, -dD's #defines): every
/// declaration at file scope, into the context's table, and every name into its name set.
void c_decls_extract(struct ParserContext *ctx, const char *text);

/// The declaration of `name` (a function, variable, typedef, enum constant or macro), or of a
/// tag when `name` is written "struct X", "union X" or "enum X". NULL if C declares none.
const CDecl *c_decl_find(struct ParserContext *ctx, const char *name);

/// Every declaration, for saving the table (count in *n).
const CDecl *const *c_decl_all(struct ParserContext *ctx, unsigned *n);

/// Adds `d` (copied) to the table, as when it is loaded from the cache.
void c_decl_add(struct ParserContext *ctx, const CDecl *d);

// ---- Types, for zc's inference and checks (NULL: C declares nothing zc can use) ----------

/// A C type as zc writes one ("const char*", "wgf_vec3_t"), or NULL if it can't be written
/// as `T name` (an array, a function pointer, an anonymous struct).
const char *c_decl_writable(const char *ctype);

/// The type of the value `name` names: a variable's, an enum constant's enum, a numeric
/// macro's (`#define W 800` is int). Writable, or NULL.
const char *c_decl_value_type(struct ParserContext *ctx, const char *name);

/// A C function's return type, writable, or NULL (not a C function, or not writable).
const char *c_decl_return_type(struct ParserContext *ctx, const char *name);

/// The struct or union declaration whose fields a value of type `type` has ("vec2",
/// "struct vec2", "const vec2"; typedefs followed to the tag), with its fields read whole;
/// NULL if it isn't a C struct or union, or its fields aren't known.
const CDecl *c_decl_aggregate(struct ParserContext *ctx, const char *type);

/// The field `field` of the C aggregate `agg` (anonymous members' included), or NULL.
const CDeclField *c_decl_field(const CDecl *agg, const char *field);

/// The C enum `type` is ("enum color", or the typedef naming an anonymous one), or NULL.
const char *c_decl_enum_of(struct ParserContext *ctx, const char *type);

#endif
