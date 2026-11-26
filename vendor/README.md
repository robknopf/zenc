# Vendored C

Third-party C that zc is built with, kept as released (only the files zc builds are
here). It is outside `src/`, so the lint, format, complexity and copy-paste checks, which
are for this project's code, leave it alone; it builds with its own flags
(`VENDOR_CFLAGS` in the Makefile, `zc_vendor` in CMakeLists.txt, and in build.bat).

| Directory | What | Release | Licence |
|---|---|---|---|
| `tree-sitter/` | the tree-sitter runtime: `lib/src` (all but `wasm-stdlib/`) and `lib/include` | [v0.27.2](https://github.com/tree-sitter/tree-sitter/releases/tag/v0.27.2), 2026-11-07; tarball sha256 `356528363bf7fbad12a2cfeb5a2678c0118ef886a4d69ad9a379db5fdef0735f` | MIT (`tree-sitter/LICENSE`) |
| `tree-sitter-c/` | the C grammar's generated parser: `src/parser.c` and `src/tree_sitter/` | [v0.24.2](https://github.com/tree-sitter/tree-sitter-c/releases/tag/v0.24.2), 2026-04-22; tarball sha256 `2eeb4db31f8fa0865e45488503d13403923bcb485a1bdb637abff8c42dd97364` | MIT (`tree-sitter-c/LICENSE`) |

zc uses them to read the declarations of the C headers a program includes (the names,
and the types of their fields and parameters), from the C preprocessor's output:
`src/parser/utils/utils_c_names.c`. The grammar is ABI version 15; the runtime takes 13
to 15.

To update: download the release tarballs, check them, and copy the same files over.
