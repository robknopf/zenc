#!/bin/bash
# Hot reload tests: each case in tests/reload/cases/ is a sequence of module versions
# (step1.zc, step2.zc, ...) loaded one after another by tests/reload/host.zc (on std/reload.zc), with the
# output compared to the case's `expected`.
#
# A case with an `xfail` file is known not to work yet; the file names the task that
# should fix it (see the hot reload task list). It reports XFAIL, and XPASS when it
# starts to pass, which means its xfail file can go. The host keeps every previous module
# loaded; a case with a `delete-old` file is run with it deleting them instead.
#
# A case with an `env` file runs its host with those NAME=VALUE settings.
#
# A step that does not build fails the case, even an xfail one, unless `expected` says it
# does not build.
#
# Where the Reloader doesn't load versions itself (not x86-64 Linux or Windows: libtcc loads
# them, so there are no stubs, and every old function that can't follow is reported), a
# case's `expected-memory`, if it has one, is compared instead, and a case with a
# `loader-only` file (it needs what only the Reloader's own loading does: stubs, versions
# released whole) is skipped.

ZC="${1:-./zc}"
ZC="$(cd "$(dirname "$ZC")" && pwd)/$(basename "$ZC")"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CASES="$ROOT/tests/reload/cases"
WORK="$(mktemp -d)"
cd "$ROOT" || exit 1
trap 'rm -rf "$WORK"' EXIT

TCC_DIRS=""
ZC_TCC="$("$ZC" --tcc-dir 2> /dev/null)"
if [ -n "$TCC_DIR" ]; then
    TCC_DIRS="-I$TCC_DIR/include -L$TCC_DIR/lib"
elif [ -n "$ZC_TCC" ]; then  # zc's own (vendor/tinycc)
    TCC_DIRS="-I$ZC_TCC -L$ZC_TCC"
elif [ -f /usr/local/lib/libtcc.a ]; then
    TCC_DIRS="-I/usr/local/include -L/usr/local/lib"
fi
CC="${CC:-cc}"
LIBS="-ltcc -ldl -lpthread -lm"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) # Windows: no libdl; the host exports its functions to the module
        LIBS="-ltcc -lpthread -lm -Wl,--export-all-symbols" ;;
esac
if ! printf '#include <libtcc.h>\nint main(void) { tcc_delete(tcc_new()); return 0; }\n' |
    $CC -x c - -o "$WORK/probe" $TCC_DIRS $LIBS > "$WORK/probe.log" 2>&1; then
    echo "Reload tests skipped: libtcc is not available"
    if [ -n "$ZC_RELOAD_REQUIRED" ]; then # (CI: a skip there is a failure, and why)
        echo "  zc --tcc-dir: ${ZC_TCC:-(none)}; compiled with: $CC $TCC_DIRS $LIBS"
        [ -n "$ZC_TCC" ] && ls "$ZC_TCC" | head -20 | sed 's/^/    /'
        sed 's/^/    /' "$WORK/probe.log" | head -20
        exit 1
    fi
    exit 0
fi
if ! "$ZC" transpile -q "$ROOT/tests/reload/host.zc" -o "$WORK/host.c" \
        > "$WORK/host_build.log" 2>&1 ||
    ! $CC "$WORK/host.c" $TCC_DIRS $LIBS -o "$WORK/host" \
        >> "$WORK/host_build.log" 2>&1; then
    echo "Reload tests: the host does not build"
    sed 's/^/  /' "$WORK/host_build.log" | head -10
    exit 1
fi

passed=0; failed=0; xfail=0; xpass=0
failed_names=""
# The Reloader loads versions itself on x86-64 Linux and Windows (std/reload.zc's ZR_LOADER).
OWN_LOADER=0
case "$(uname -s)/$(uname -m)" in
    Linux/x86_64|MINGW*/x86_64|MSYS*/x86_64|CYGWIN*/x86_64) OWN_LOADER=1 ;;
esac
skipped=0
for dir in "$CASES"/*/; do
    name="$(basename "$dir")"
    if [ $OWN_LOADER -eq 0 ] && [ -f "$dir/loader-only" ]; then
        skipped=$((skipped + 1))
        continue
    fi
    mkdir -p "$WORK/$name"
    export ERR="$WORK/$name/err"
    # A case's `env` file: NAME=VALUE lines for its host (ZC_RELOAD_SPACE_KB, ...).
    case_env=()
    [ -f "$dir/env" ] && mapfile -t case_env < <(grep -v '^#' "$dir/env" | grep '=')
    flag=""
    [ -f "$dir/delete-old" ] && flag="--delete-old"
    # A crash is the expected result of some xfail cases. The inner bash, which waits for
    # the host instead of becoming it ("; exit"), reports it on its own stderr, discarded,
    # so it does not appear in the output; the host's stderr still goes to err.
    bash -c '"$@" 2> "$ERR"; exit $?' _ env "${case_env[@]}" timeout 60 "$WORK/host" "$ZC" "$dir" "$WORK/$name" \
        $flag > "$WORK/$name/out" 2> /dev/null
    rc=$?
    want="$dir/expected"
    [ $OWN_LOADER -eq 0 ] && [ -f "$dir/expected-memory" ] && want="$dir/expected-memory"
    want_failed=$(grep -c "^(build failed" "$want")
    got_failed=$(grep -c "^(build failed" "$WORK/$name/out")
    if [ "$got_failed" -gt "$want_failed" ]; then
        echo "  [FAIL] $name (a step did not build)"
        sed 's/^/      stderr: /' "$WORK/$name/err" | head -10
        failed=$((failed + 1))
        failed_names="$failed_names $name"
    elif [ $rc -eq 0 ] && diff -u "$want" "$WORK/$name/out" > "$WORK/$name/diff"; then
        if [ -f "$dir/xfail" ]; then
            echo "  [XPASS] $name (now passes: remove its xfail file; with libtcc 0.9.28 a" \
                "dangling pointer can also pass by luck, see std/reload.zc)"
            xpass=$((xpass + 1))
        else
            echo "  [PASS] $name"
            passed=$((passed + 1))
        fi
    elif [ -f "$dir/xfail" ]; then
        echo "  [XFAIL] $name: $(head -1 "$dir/xfail")"
        xfail=$((xfail + 1))
    else
        echo "  [FAIL] $name (host exit $rc)"
        diff -u "$want" "$WORK/$name/out" | sed 's/^/      /' | head -20
        sed 's/^/      stderr: /' "$WORK/$name/err" | head -10
        failed=$((failed + 1))
        failed_names="$failed_names $name"
    fi
done

# poll(): tests/reload/poll_host.zc changes files itself and checks each answer.
mkdir -p "$WORK/poll"
if "$ZC" transpile -q "$ROOT/tests/reload/poll_host.zc" -o "$WORK/poll_host.c" \
        > "$WORK/poll/build.log" 2>&1 &&
    $CC "$WORK/poll_host.c" $TCC_DIRS $LIBS -o "$WORK/poll_host" \
        >> "$WORK/poll/build.log" 2>&1 &&
    timeout 60 "$WORK/poll_host" "$ZC" "$WORK/poll" > "$WORK/poll/out" 2>&1 &&
    rm -rf "$WORK/poll/mod" &&
    ZC_RELOAD_SCAN=1 timeout 60 "$WORK/poll_host" "$ZC" "$WORK/poll" >> "$WORK/poll/out" 2>&1; then
    echo "  [PASS] poll (inotify, and scanning)"
    passed=$((passed + 1))
else
    echo "  [FAIL] poll"
    sed 's/^/      /' "$WORK/poll/build.log" "$WORK/poll/out" | head -20
    failed=$((failed + 1))
    failed_names="$failed_names poll"
fi

echo "----------------------------------------"
echo "Results (reload):"
echo "-> Passed:  $passed"
echo "-> Failed:  $failed"
echo "-> Known not to work yet (XFAIL): $xfail"
[ $skipped -ne 0 ] && echo "-> Skipped (they need the Reloader's own loading): $skipped"
[ $xpass -ne 0 ] && echo "-> Unexpectedly passing (XPASS): $xpass"
echo "----------------------------------------"
[ $failed -ne 0 ] && { echo "Failed:$failed_names"; exit 1; }
exit 0
