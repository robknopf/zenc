# Vendored C

Third-party C that zc is built with, kept as released (only the files zc builds are
here). It is outside `src/`, so the lint, format, complexity and copy-paste checks, which
are for this project's code, leave it alone; it builds with its own flags
(`VENDOR_CFLAGS` in the Makefile, `zc_vendor` in CMakeLists.txt, and in build.bat).

| Directory | What | Release | Licence |
|---|---|---|---|
| `tree-sitter/` | the tree-sitter runtime: `lib/src` (all but `wasm-stdlib/`) and `lib/include` | [v0.27.2](https://github.com/tree-sitter/tree-sitter/releases/tag/v0.27.2), 2026-11-07; tarball sha256 `356528363bf7fbad12a2cfeb5a2678c0118ef886a4d69ad9a379db5fdef0735f` | MIT (`tree-sitter/LICENSE`) |
| `tinycc/` | Tiny C Compiler: its sources, `include/`, `lib/` and `win32/` (all but `tests/` and `examples/`) | mob branch at [e9e9e7f](https://repo.or.cz/tinycc.git/commit/e9e9e7f6236895a32c5e905aada486bab6671e95), 2026-11-23 (no release since 0.9.27); three changes, below | LGPL 2.1 (`tinycc/COPYING`; `tinycc/RELICENSING`) |
| `tree-sitter-c/` | the C grammar's generated parser: `src/parser.c` and `src/tree_sitter/` | [v0.24.2](https://github.com/tree-sitter/tree-sitter-c/releases/tag/v0.24.2), 2026-04-22; tarball sha256 `2eeb4db31f8fa0865e45488503d13403923bcb485a1bdb637abff8c42dd97364` | MIT (`tree-sitter-c/LICENSE`) |

zc uses tree-sitter and its grammar to read the declarations of the C headers a program includes (the names,
and the types of their fields and parameters), from the C preprocessor's output:
`src/parser/utils/utils_c_names.c`. The grammar is ABI version 15; the runtime takes 13
to 15.

To update: download the release tarballs, check them, and copy the same files over.

## tinycc

zc's own tcc: `make` builds it with tinycc's own build (`configure` and its Makefile, in
`obj/tinycc`) and gathers libtcc, tcc and tcc's runtime in `tcc/` (`make install`: in
SHAREDIR/tcc), which `zc --tcc-dir` names. The REPL's JIT and std/reload.zc's Reloader
use it, so neither needs a tcc or a libtcc of the system's, and the libtcc a reloading
program is built with is the one its versions are compiled by. libtcc is built static on
every OS (on Windows too, where tinycc's default is a DLL).

Three changes. Two so tinycc builds outside its source tree: `lib/Makefile`'s rule for
`bcheck_run.o` compiles `$<`, not `bcheck.c`; and `configure` links into the build
directory only the Makefiles that are here (not `tests/`', which on Windows, where it
copies them, fails). And in `tcc.h`, built by gcc for 64-bit Windows, `setjmp` gives the C
runtime no frame to unwind to: libtcc's errors return by longjmp, and MinGW's, unwinding
(RtlUnwind), crashed from a libtcc linked into a program.

LGPL 2.1: zc and the programs that link libtcc (development builds that reload: release
builds don't) are linked with it statically; whoever distributes one must let its users
relink it with another libtcc (the LGPL's section 6), as the object files and `zc build`
do.

To update: check out the commit, copy its tracked files over (without `tests/` and
`examples/`), and make the changes again.
