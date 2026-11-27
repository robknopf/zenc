// SPDX-License-Identifier: MIT
#ifndef CMD_H
#ifndef ZC_ALLOW_INTERNAL
#error "utils/cmd.h is internal to Zen C. Include the appropriate public header instead."
#endif

#define CMD_H

#include <stddef.h>
#include "../compat/c23_compat.h"

typedef struct CompilerConfig CompilerConfig;
typedef struct ZenCompiler ZenCompiler;

typedef struct
{
    char **args;
    size_t count;
    size_t cap;
} ArgList;

/**
 * @brief Print compiler library search paths
 */
void print_search_paths(CompilerConfig *cfg);

/**
 * @brief zc's own tcc directory (libtcc, its headers and runtime), or NULL
 */
const char *zc_tcc_dir(CompilerConfig *cfg);

/**
 * @brief Export zc_tcc_dir() as ZC_TCC_DIR, for `${ZC_TCC_DIR}` in directives
 */
void zc_export_tcc_dir(CompilerConfig *cfg);

/**
 * @brief Print compiler version
 */
void print_version(void);

/**
 * @brief Print compiler usage string
 */
void print_usage(void);
void print_command_help(const char *command);

/**
 * @brief Initialize a new argument list
 * @param list The list to initialize
 */
void arg_list_init(ArgList *list);

/**
 * @brief Add an argument to the list
 * @param list The list to add to
 * @param arg The argument to add (will be duplicated)
 */
void arg_list_add(ArgList *list, const char *arg);

/**
 * @brief Add a formatted argument to the list
 * @param list The list to add to
 * @param fmt The format string
 * @param ... The arguments to format
 */
ZEN_FORMAT_PRINTF(2, 3) void arg_list_add_fmt(ArgList *list, const char *fmt, ...);

/**
 * @brief --reload: writes, one a line, what the C compiler would be given besides the C file
 * (include directories and defines as -I/-D, and the C files the module links), for the
 * Reloader (std/reload.zc) to compile the module the same way. Removes `path` if there's
 * nothing to say.
 */
void write_reload_build_args(ZenCompiler *compiler, const char *c_file); // (appends to it)

/**
 * @brief Free the argument list
 * @param list The list to free
 */
void arg_list_free(ArgList *list);

/**
 * @brief Run the argument list securely
 * @param list The list to run
 * @return Exit code
 */
int arg_run(ArgList *list);

/**
 * @brief Add arguments from a space-separated string to the list
 * @param list The list to add to
 * @param str The string to parse
 */
void arg_list_add_from_string(ArgList *list, const char *str);

void build_compile_arg_list(ArgList *list, const char *outfile, const char *temp_source_file,
                            ZenCompiler *compiler);

/**
 * @brief The command that runs the C preprocessor on `source` into `outfile`, with the
 * flags and include paths the C compiler is given, keeping the #defines (-E -dD).
 */
void build_preprocess_arg_list(ArgList *list, const char *outfile, const char *source,
                               ZenCompiler *compiler);

#endif
