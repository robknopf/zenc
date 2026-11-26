#!/bin/bash
# Debugging reloaded code: gdb on the reload test host (tests/reload/host.zc) running
# tests/reload/cases/debug_frames, whose versions the Reloader registers with gdb's JIT
# interface. Checks that a breakpoint by .zc file and line binds in each version as it
# loads, set before the program starts; that the backtrace from it goes through the
# module's functions (one with a frame over 4 KB) into the host's; and that an argument and a
# local read right.
#
# Usage: tests/scripts/run_debugger_tests.sh [ZC]   (default ./zc)
# Env:   TCC_DIR=dir   the libtcc to link the host with (default zc's own), as for
#                      run_reload_tests.sh. It must
#                      be 0.9.28 or later: 0.9.27 writes stabs, which gdb doesn't bind
#                      breakpoints in (then this is skipped).
#        CC=...        the compiler for the host (default cc)
# Skipped, successfully, without gdb or a libtcc that writes DWARF.

ZC="${1:-./zc}"
ZC="$(cd "$(dirname "$ZC")" && pwd)/$(basename "$ZC")"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="$(mktemp -d)"
cd "$ROOT" || exit 1
trap 'rm -rf "$WORK"' EXIT

if ! command -v gdb > /dev/null; then
    echo "Debugger tests skipped: no gdb"
    exit 0
fi
TCC_DIRS=""
ZC_TCC="$("$ZC" --tcc-dir 2> /dev/null)"
[ -z "$TCC_DIR" ] && TCC_DIR="$ZC_TCC"  # zc's own (vendor/tinycc)
# (an installed tree: include/ and lib/; Windows': libtcc.h and libtcc.dll at the top, where
# include/ is tcc's own C headers)
if [ -n "$TCC_DIR" ] && [ -f "$TCC_DIR/libtcc.h" ]; then
    TCC_DIRS="-I$TCC_DIR -L$TCC_DIR"
elif [ -n "$TCC_DIR" ]; then
    TCC_DIRS="-I$TCC_DIR/include -L$TCC_DIR/lib"
fi
CC="${CC:-cc}"
LIBS="-ltcc -ldl -lpthread -lm -rdynamic"
EXE=""
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) LIBS="-ltcc -lpthread -lm -Wl,--export-all-symbols"; EXE=".exe" ;;
esac
# libtcc 0.9.27 defines TCC_RELOCATE_AUTO; 0.9.28, which writes DWARF, doesn't.
if ! printf '#include <libtcc.h>\n#ifdef TCC_RELOCATE_AUTO\n#error stabs\n#endif\nint main(void) { tcc_delete(tcc_new()); return 0; }\n' |
    $CC -x c - -o "$WORK/probe$EXE" $TCC_DIRS $LIBS > /dev/null 2>&1; then
    echo "Debugger tests skipped: no libtcc 0.9.28 or later (TCC_DIR)"
    exit 0
fi
if ! "$ZC" transpile -q "$ROOT/tests/reload/host.zc" -o "$WORK/host.c" > "$WORK/build.log" 2>&1 ||
    ! $CC -g "$WORK/host.c" $TCC_DIRS $LIBS -o "$WORK/host$EXE" >> "$WORK/build.log" 2>&1; then
    echo "Debugger tests: the host does not build"
    sed 's/^/  /' "$WORK/build.log" | head -10
    exit 1
fi

CASE="$ROOT/tests/reload/cases/debug_frames"
# (the return: tcc's line table also puts a function's first instructions on its second
# line, so a breakpoint there has two places)
LINE=$(grep -n 'return doubled;' "$CASE/step1.zc" | cut -d: -f1)
mkdir -p "$WORK/run"
cat > "$WORK/cmds" <<EOF
set breakpoint pending on
set pagination off
set confirm off
break step1.zc:$LINE
break step2.zc:$LINE
run
bt 4
print x
print doubled
continue
bt 3
print doubled
frame 1
info locals
delete
continue
EOF
timeout 120 gdb -q -batch -x "$WORK/cmds" --args "$WORK/host$EXE" "$ZC" "$CASE" "$WORK/run" \
    > "$WORK/gdb.out" 2>&1

passed=0; failed=0
check() {
    if grep -Eq "$2" "$WORK/gdb.out"; then
        echo "  [PASS] $1"
        passed=$((passed + 1))
    else
        echo "  [FAIL] $1 (gdb's output doesn't match: $2)"
        failed=$((failed + 1))
    fi
}
check "a breakpoint by line binds in version 1" "inner \(x=10\) at [^ ]*step1\.zc:$LINE"
check "the backtrace goes through a frame over 4 KB" "#1 .*big \(x=10\) at [^ ]*step1\.zc:"
check "and the module's entry point" "#2 .*reload_step \(_state=[^)]*\) at [^ ]*step1\.zc:"
check "and on into the host" "#3 .*zr_call \("
check "an argument reads right" '^\$1 = 10$'
check "a local reads right" '^\$2 = 20$'
check "the breakpoint binds in version 2 as it loads" "inner \(x=10\) at [^ ]*step2\.zc:$LINE"
check "and in version 2" '^\$3 = 20$'
check "a local of a caller in version 2" '^buf = '
check "the program runs to its end" "exited normally|exited with code 0"

# The same from VS Code: its debug adapters (CodeLLDB, the C/C++ extension's gdb), driven as
# VS Code drives them, where they're installed (tests/scripts/vscode_debug.py).
PY="$(command -v python3 || command -v python)"
if [ -n "$PY" ] && "$PY" -c 'import sys; sys.exit(sys.version_info < (3, 6))' 2> /dev/null; then
    winpath() { if command -v cygpath > /dev/null; then cygpath -w "$1"; else echo "$1"; fi; }
    "$PY" "$ROOT/tests/scripts/vscode_debug.py" "$(winpath "$WORK/host$EXE")" "$(winpath "$ZC")" \
        "$(winpath "$CASE")" "$(winpath "$WORK/vsrun")" > "$WORK/vscode.out" 2>&1
    grep -E '^\s+\[(PASS|FAIL|SKIP)\]' "$WORK/vscode.out"
    vp=$(sed -n 's/^vscode: \([0-9]*\) passed, \([0-9]*\) failed$/\1/p' "$WORK/vscode.out")
    vf=$(sed -n 's/^vscode: \([0-9]*\) passed, \([0-9]*\) failed$/\2/p' "$WORK/vscode.out")
    if [ -z "$vp" ]; then
        echo "  [FAIL] the VS Code checks didn't finish"; sed 's/^/      /' "$WORK/vscode.out" | tail -10
        failed=$((failed + 1))
    else
        passed=$((passed + vp)); failed=$((failed + vf))
    fi
else
    echo "  [SKIP] VS Code's debug adapters: no Python 3"
fi

echo "----------------------------------------"
echo "Results (debugger):"
echo "-> Passed:  $passed"
echo "-> Failed:  $failed"
echo "----------------------------------------"
if [ $failed -ne 0 ]; then
    sed 's/^/      gdb: /' "$WORK/gdb.out" | grep -v "New Thread\|Thread .* exited" | head -40
    exit 1
fi
exit 0
