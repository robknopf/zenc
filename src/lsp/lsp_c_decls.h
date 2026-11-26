// SPDX-License-Identifier: MIT
#ifndef LSP_C_DECLS_H
#ifndef ZC_ALLOW_INTERNAL
#error "lsp/lsp_c_decls.h is internal to Zen C. Include the appropriate public header instead."
#endif

#define LSP_C_DECLS_H

// The declarations of a document's C, as zc reads them for a build (the C preprocessor and
// tree-sitter, parser/utils/c_decls.h), for hover, go to definition, completion and
// diagnostics. Each open document has its own: different files include different headers.
// lsp_c_headers.c's own reading of the headers stays for what this doesn't cover: no C
// compiler, a header that doesn't preprocess, one re-exported by another module.

#include "lsp_c_headers.h"

struct cJSON;
struct ProjectFile;

/// Reads the document's C, if its includes and raw blocks changed (a write request: the
/// declarations live in the arena with the document), and makes them the context's.
void lsp_c_decls_update(struct ProjectFile *pf);

/// Makes the document's C declarations the context's (each request, before it looks).
void lsp_c_decls_use(struct ProjectFile *pf);

/// The C declaration of the name at (line, col), 0-based, as a header symbol (for
/// lsp_c_headers_hover() and lsp_c_headers_locations()). Valid until the next call.
const CHeaderSymbol *lsp_c_decls_symbol_at(struct ProjectFile *pf, int line, int col);

/// Adds the C names the word before (line, col) starts (functions with their signature,
/// types, constants, macros) to `items`. Returns 1 if there were more than it added.
int lsp_c_decls_complete(struct ProjectFile *pf, int line, int col, struct cJSON *items);

/// Adds the fields of the C struct or union `type` to `items`; 0 if it isn't one.
int lsp_c_decls_fields(struct ProjectFile *pf, const char *type, struct cJSON *items);

/// Diagnostics for the document's calls to C functions: the wrong number of arguments, a
/// value of one C enum for a parameter of another.
void lsp_c_decls_add_diagnostics(struct ProjectFile *pf, struct cJSON *diagnostics);

#endif
