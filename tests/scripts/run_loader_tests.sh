#!/bin/bash
# The Reloader's own loader (std/reload.zc's zl_load) over the compiler's test programs.
# For each test, zc writes the C for tcc (`zc build --cc tcc --emit-c`); that C is then
#  - built into a program by tcc (the reference), and run;
#  - compiled the way the Reloader compiles a version and loaded by its loader
#    (tests/reload/load_host.zc), its constructors and main run.
# Both with thread-locals made ordinary globals, as in a reload build. The exit codes must be
# the same, and the output too (unless the reference's own output differs from one run to
# the next, or it prints timings or what threads do in no set order: then only the exit
# code). A test whose reference doesn't build or run here is
# skipped: what's tested is the loader, not tcc or the test.
#
# Usage: tests/scripts/run_loader_tests.sh [ZC]   (default ./zc)
# Env:   TCC=path   the tcc to build the references with (default: tcc on PATH). load_host
#                   is linked against that tcc's libtcc, so both are the same version.
#        CC=...     the compiler for load_host (default cc)
#        ZC_LOADER_TESTS="a.zc b.zc"   only these
#        ZC_LOADER_VERBOSE=1           say why each skipped test was
# Skipped, successfully, when tcc or libtcc is not available (a failure with
# ZC_RELOAD_REQUIRED=1, as CI sets).

ZC="${1:-./zc}"
ZC="$(cd "$(dirname "$ZC")" && pwd)/$(basename "$ZC")"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="$(mktemp -d)"
cd "$ROOT" || exit 1
trap 'rm -rf "$WORK"' EXIT

ZC_TCC="$("$ZC" --tcc-dir 2> /dev/null)"
if [ -z "$TCC" ] && [ -n "$ZC_TCC" ]; then  # zc's own (vendor/tinycc)
    for t in "$ZC_TCC/tcc" "$ZC_TCC/tcc.exe"; do [ -x "$t" ] && TCC="$t"; done
    # (load_host compiles as the Reloader does, without asking zc where tcc's headers are)
    [ -n "$TCC" ] && export ZC_TCC_DIR="$ZC_TCC"
fi
TCC="${TCC:-$(command -v tcc)}"
if [ -z "$TCC" ]; then
    echo "Loader tests skipped: no tcc"
    [ -n "$ZC_RELOAD_REQUIRED" ] && exit 1 # (CI: a skip there is a failure)
    exit 0
fi
# Its libtcc: beside it (Windows), or in ../include and ../lib (an installed tree).
TCC_BIN_DIR="$(cd "$(dirname "$TCC")" && pwd)"
if [ -f "$TCC_BIN_DIR/libtcc.h" ]; then
    TCC_DIRS="-I$TCC_BIN_DIR -L$TCC_BIN_DIR"
else
    TCC_DIRS="-I$TCC_BIN_DIR/../include -L$TCC_BIN_DIR/../lib"
fi
CC="${CC:-cc}"
LIBS="-ltcc -ldl -lpthread -lm -rdynamic"
EXE=""
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) LIBS="-ltcc -lpthread -lm -Wl,--export-all-symbols"; EXE=".exe" ;;
esac

if ! "$ZC" transpile -q "$ROOT/tests/reload/load_host.zc" -o "$WORK/load_host.c" > "$WORK/build.log" 2>&1 ||
    ! $CC -g "$WORK/load_host.c" $TCC_DIRS $LIBS -o "$WORK/load_host$EXE" >> "$WORK/build.log" 2>&1; then
    if grep -q "libtcc" "$WORK/build.log"; then
        echo "Loader tests skipped: libtcc is not available"
        [ -n "$ZC_RELOAD_REQUIRED" ] && exit 1 # (CI: a skip there is a failure)
        exit 0
    fi
    echo "Loader tests: load_host does not build"
    sed 's/^/  /' "$WORK/build.log" | head -10
    exit 1
fi

if [ -n "$ZC_LOADER_TESTS" ]; then
    TESTS=($ZC_LOADER_TESTS)
else
    mapfile -t TESTS < <(find tests -name "*.zc" -not -name "_*.zc" -not -path "*/backends/*" \
        -not -path "*/diagnostics_output/*" -not -path "tests/reload/*" -not -path "tests/misra/*" | sort)
fi

passed=0; failed=0; skipped=0; failed_names=""
skip() { skipped=$((skipped + 1)); [ -n "$ZC_LOADER_VERBOSE" ] && echo "  [SKIP] $t: $1"; }
for t in "${TESTS[@]}"; do
    # What run_tests.sh skips for tcc, and what isn't a program to run.
    case "$t" in
        *test_intel.zc|*test_attributes.zc|*test_simd_native.zc|*_arm64.zc|*_aarch64.zc)
            skip "not for tcc, or this processor"; continue ;;
    esac
    if grep -q "// EXPECT: FAIL\|// REQUIRE:" "$t"; then
        skip "expected to fail, or needs something"; continue
    fi
    d="$WORK/t"
    rm -rf "$d"; mkdir -p "$d"
    # The C, and the command zc would build it with.
    if ! "$ZC" build --cc tcc -v -w --emit-c "$t" -o "$d/prog" > "$d/zc.log" 2>&1; then
        skip "zc build --cc tcc fails"; continue
    fi
    cmd=$(grep -m1 "Command " "$d/zc.log" | sed 's/^ *Command //')
    flags=()
    sources=()
    skip_next=0
    for w in $cmd; do
        if [ $skip_next -eq 1 ]; then skip_next=0; continue; fi
        case "$w" in
            -o) skip_next=1 ;;
            -I*|-D*|-l*|-L*|-W*) flags+=("$w") ;;
            *.c) [ "$(basename "$w")" != prog.c ] && sources+=("$w") ;; # C sources it links too
        esac
    done
    # The reference: the same C, thread-locals as in a reload build.
    if ! "$TCC" -D_Thread_local= -D__thread= "${flags[@]}" -o "$d/ref$EXE" "$d/prog.c" "${sources[@]}" > "$d/ref.log" 2>&1; then
        skip "the reference doesn't build"; continue
    fi
    (cd "$d" && timeout 30 "./ref$EXE" < /dev/null > ref1.out 2> /dev/null); rc1=$?
    (cd "$d" && timeout 30 "./ref$EXE" < /dev/null > ref2.out 2> /dev/null); rc2=$?
    if [ $rc1 -ge 124 ] || [ $rc1 -ne $rc2 ]; then
        skip "the reference exits $rc1, then $rc2"; continue
    fi
    loader_flags=()
    for f in "${flags[@]}"; do
        case "$f" in -I*|-D*|-l*) loader_flags+=("$f") ;; esac  # (-l: the libraries it loads)
    done
    (cd "$d" && timeout 30 "$WORK/load_host$EXE" "$d/prog.c" "${sources[@]}" "${loader_flags[@]}" < /dev/null \
        > loaded.out 2> loaded.err); rc=$?
    same_out=1
    case "$t" in tests/bench/*|*thread*|*concurren*|*sync*|*std_expansion*) ;; # (timings, threads' order)
        *) if cmp -s "$d/ref1.out" "$d/ref2.out" && ! cmp -s "$d/ref1.out" "$d/loaded.out"; then same_out=0; fi ;;
    esac
    if [ $rc -eq $rc1 ] && [ $same_out -eq 1 ]; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
        failed_names="$failed_names $t"
        echo "  [FAIL] $t (exit $rc, the program's $rc1)"
        sed 's/^/      stderr: /' "$d/loaded.err" | head -4
        [ $same_out -eq 0 ] && diff "$d/ref1.out" "$d/loaded.out" | head -6 | sed 's/^/      /'
    fi
done

echo "----------------------------------------"
echo "Results (loader):"
echo "-> Passed:  $passed"
echo "-> Failed:  $failed"
echo "-> Skipped: $skipped"
echo "----------------------------------------"
[ $failed -ne 0 ] && { echo "Failed:$failed_names"; exit 1; }
exit 0
