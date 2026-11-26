#!/bin/bash
# Zen C CLI integration tests.
#
# Verifies the top-level `zc` command surface: version output, usage/help,
# and error handling for bad invocations, plus a compile+run smoke test.
#
# Usage: ./tests/scripts/run_cli_tests.sh
# Env:   ZC=path/to/zc          (default: ./zc)
#        ZC_ROOT=path/to/stdlib (set when running from outside the repo)

ZC="${ZC:-./zc}"
if [ ! -f "$ZC" ]; then
    echo "Error: zc binary not found ($ZC). Build it first." >&2
    exit 1
fi

PASSED=0
FAILED=0
FAILED_TESTS=""

check() {
    local name="$1"
    local ok="$2"
    if [ "$ok" = "1" ]; then
        echo "  [PASS] $name"
        PASSED=$((PASSED + 1))
    else
        echo "  [FAIL] $name"
        FAILED=$((FAILED + 1))
        FAILED_TESTS="$FAILED_TESTS\n- $name"
    fi
}

# --- Version ---------------------------------------------------------------

out=$("$ZC" --version 2>&1)
if echo "$out" | grep -qE '^zc v[0-9]'; then
    check "--version prints version" 1
else
    check "--version prints version" 0
fi

out=$("$ZC" -V 2>&1)
if echo "$out" | grep -qE '^zc v[0-9]'; then
    check "-V prints version" 1
else
    check "-V prints version" 0
fi

# --- Usage / help ----------------------------------------------------------

out=$("$ZC" 2>&1)
if echo "$out" | grep -q "usage: zc"; then
    check "no arguments prints usage" 1
else
    check "no arguments prints usage" 0
fi

out=$("$ZC" help 2>&1)
if echo "$out" | grep -q "usage: zc"; then
    check "zc help prints usage" 1
else
    check "zc help prints usage" 0
fi

# --- Error paths -----------------------------------------------------------

out=$("$ZC" run 2>&1)
if echo "$out" | grep -q "no input file specified"; then
    check "zc run with no file reports error" 1
else
    check "zc run with no file reports error" 0
fi

out=$("$ZC" run "$ZC/does_not_exist.zc" 2>&1)
if echo "$out" | grep -q "could not read file"; then
    check "zc run with missing file reports error" 1
else
    check "zc run with missing file reports error" 0
fi

out=$("$ZC" --definitely-not-a-flag "$ZC" 2>&1)
if echo "$out" | grep -qiE "unknown|unrecognized|error"; then
    check "unknown flag reports error" 1
else
    check "unknown flag reports error" 0
fi

# --- Compile + run smoke test ----------------------------------------------

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
printf 'fn main() {\n    println "cli-smoke-ok"\n}\n' > "$workdir/smoke.zc"

out=$("$ZC" run "$workdir/smoke.zc" -o "$workdir/smoke" 2>&1)
if echo "$out" | grep -q "cli-smoke-ok"; then
    check "zc run executes a program" 1
else
    check "zc run executes a program" 0
fi

out=$("$ZC" transpile "$workdir/smoke.zc" -o "$workdir/smoke_out.c" 2>&1)
if [ -s "$workdir/smoke_out.c" ]; then
    check "zc transpile emits C source" 1
else
    check "zc transpile emits C source" 0
fi

# --- Program output ----------------------------------------------------------

# Runs a program and compares its exact standard output (stdin from $3).
output_check() {
    local name="$1"
    local code="$2"
    local input="$3"
    local expected="$4"
    printf '%s\n' "$code" > "$workdir/output.zc"
    "$ZC" build -w "$workdir/output.zc" -o "$workdir/output_bin" >/dev/null 2>&1
    local actual
    actual=$(printf '%s' "$input" | "$workdir/output_bin" 2>/dev/null)
    if [ "$actual" = "$expected" ]; then
        check "$name" 1
    else
        check "$name" 0
    fi
}

output_check "bare strings print without semicolons" 'fn main() {
    let x = 3
    "first"
    "x is {x}"
    "last"
}' "" "first
x is 3
last"

out=$("$ZC" build "$workdir/smoke.zc" -o "$workdir/smoke_build" 2>&1)
if [ -x "$workdir/smoke_build" ]; then
    check "zc build compiles a program" 1
else
    check "zc build compiles a program" 0
fi

out=$("$ZC" "$workdir/smoke.zc" -o "$workdir/smoke_implicit" 2>&1)
if [ -x "$workdir/smoke_implicit" ]; then
    check "zc <file> builds without a command" 1
else
    check "zc <file> builds without a command" 0
fi

out=$("$ZC" frobnicate "$workdir/smoke.zc" -o "$workdir/smoke_unknown" 2>&1)
rc=$?
if [ "$rc" -ne 0 ] && echo "$out" | grep -q "unknown command 'frobnicate'" &&
    [ ! -e "$workdir/smoke_unknown" ]; then
    check "unknown command reports error" 1
else
    check "unknown command reports error" 0
fi

out=$("$ZC" transpile "$workdir/smoke.zc" --backend astdump -o "$workdir/smoke_bad_backend" 2>&1)
rc=$?
if [ "$rc" -ne 0 ] && echo "$out" | grep -q "unknown backend 'astdump'" &&
    echo "$out" | grep -q "did you mean 'ast-dump'" && [ ! -e "$workdir/smoke_bad_backend" ]; then
    check "unknown backend reports error and suggests a name" 1
else
    check "unknown backend reports error and suggests a name" 0
fi

"$ZC" transpile "$workdir/smoke.zc" --backend ast-dump -o "$workdir/smoke_ast" >/dev/null 2>&1
if [ -s "$workdir/smoke_ast" ]; then
    check "a known backend is selected by name" 1
else
    check "a known backend is selected by name" 0
fi

# --- Robustness: pathological inputs must exit cleanly (never crash) -------

# A crash (signal) yields exit code > 128; a clean compile or a normal
# diagnostic error stays <= 128. This guards the parser/codegen overflow
# fixes against regressions.
robust_check() {
    local name="$1"
    local code="$2"
    printf '%s\n' "$code" > "$workdir/robust.zc"
    "$ZC" build "$workdir/robust.zc" -o "$workdir/robust_out" >/dev/null 2>&1
    local rc=$?
    if [ "$rc" -le 128 ]; then
        check "$name" 1
    else
        check "$name" 0
    fi
}

robust_check "20-arg ?() scan does not crash" 'fn main() {
    let x = ?("prompt", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20);
    println "x";
}'

long_ident=$(python3 -c "print('A' * 1500)")
robust_check "very long qualified identifier does not crash" "fn main() {
    let x = Foo::$long_ident;
    println \"x\";
}"

robust_check "enum variant with 40 type args does not crash" "fn gen_enum() {
    return 0;
}
enum E { V(int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int) }
fn main() {
    let e = E::V(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40);
    println \"e\";
}"

robust_check "@derive on a struct with many long fields does not crash" "fn gen_derive() {
    return 0;
}
@derive(Eq)
struct S {
    field_0_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_1_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_2_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_3_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_4_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_5_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_6_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_7_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_8_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_9_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_10_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_11_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_12_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_13_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_14_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_15_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_16_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_17_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_18_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_19_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_20_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_21_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_22_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_23_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_24_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_25_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_26_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_27_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_28_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
    field_29_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: int;
}
fn main() {
    println \"ok\";
}"

garg=$(python3 -c "print('B' * 1500)")
robust_check "generic call with a very long type argument does not crash" "fn f<T>(x: T) -> T {
    return x;
}
fn main() {
    let y = f<$garg>(1);
    println \"y\";
}"

# --- Diagnostics: errors reported by Zen C, not by the C compiler ------------

# Builds a program that must fail with a Zen C error matching $3 (and never
# reach the C compiler).
diag_check() {
    local name="$1"
    local code="$2"
    local pattern="$3"
    printf '%s\n' "$code" > "$workdir/diag.zc"
    local out
    out=$("$ZC" build "$workdir/diag.zc" -o "$workdir/diag_out" 2>&1)
    if echo "$out" | grep -q "$pattern" && ! echo "$out" | grep -q "C compilation failed"; then
        check "$name" 1
    else
        check "$name" 0
    fi
}

diag_check "redefined function is a Zen C error" 'fn twice() -> int { return 1; }
fn twice() -> int { return 2; }
fn main() { println "{twice()}"; }' "Redefinition of function 'twice'"

diag_check "redefined method is a Zen C error" 'struct Counter { value: int; }
impl Counter { fn get(self) -> int { return 1; } }
impl Counter { fn get(self) -> int { return 2; } }
fn main() { let c = Counter { value: 0 }; println "{c.get()}"; }' "Redefinition of method 'Counter::get'"

# An enum variant's constructor has no default values; too few arguments to it
# used to read the missing list and crash.
diag_check "enum variant with too few arguments is a Zen C error" 'enum Packet { Wide(int, int), Empty }
fn main() { let p = Packet::Wide(1); }' "Too few arguments"

# --- C names: what the included C declares is known to zc --------------------------------

# A header with the shapes system and library headers use.
mkdir -p "$workdir/cnames"
cat > "$workdir/cnames/lib.h" <<'EOF_H'
#ifndef LIB_H
#define LIB_H
#define LIB_LIMIT 8
#define LIB_MAX(a, b) ((a) > (b) ? (a) : (b))
typedef struct lib_vec3_t { float x, y, z; } lib_vec3_t;
typedef int (*lib_callback_t)(void *user, int code);
typedef enum { LIB_MODE_A = 1, LIB_MODE_B = 1 << 3, LIB_MODE_C } lib_mode_t;
struct lib_opaque;
extern int lib_counter;
__attribute__((visibility("default"))) int lib_add(int a, int b) __attribute__((nonnull));
const char *lib_name(const struct lib_opaque *restrict o, lib_callback_t cb, ...);
static inline int lib_twice(int v) { if (v > 0) { return v * 2; } return 0; }
#endif
EOF_H
cat > "$workdir/cnames/ok.zc" <<'EOF_Z'
include "lib.h"
include <stdio.h>
fn main() {
    let v: lib_vec3_t;
    v.x = 1.0;
    let m: lib_mode_t = LIB_MODE_C;
    let n = lib_add(LIB_LIMIT, lib_twice(2)) + lib_counter + LIB_MAX(1, 2);
    printf("%d %d %f\n", n, (int)m, v.x);
}
EOF_Z
out=$("$ZC" transpile "$workdir/cnames/ok.zc" -o "$workdir/cnames/ok.c" 2>&1)
if ! echo "$out" | grep -q "Undefined\|Unknown type"; then
    check "names and types a C header declares are known" 1
else
    check "names and types a C header declares are known" 0
    echo "$out" | head -10 | sed 's/^/      /'
fi

printf 'include "lib.h"\ninclude <stdio.h>\nfn main() {\n    pritnf("hi");\n    let n = LIB_LIMTI;\n    lib_ad(1, 2);\n}\n' > "$workdir/cnames/bad.zc"
out=$("$ZC" transpile "$workdir/cnames/bad.zc" -o "$workdir/cnames/bad.c" 2>&1)
if echo "$out" | grep -q "Undefined function 'pritnf'" && echo "$out" | grep -q "Did you mean 'printf'" &&
    echo "$out" | grep -q "Undefined name 'LIB_LIMTI'" && echo "$out" | grep -q "Did you mean 'LIB_LIMIT'" &&
    echo "$out" | grep -q "Undefined function 'lib_ad'" && echo "$out" | grep -q "bad.zc:4:5"; then
    check "a misspelled C function or constant is reported at the .zc line" 1
else
    check "a misspelled C function or constant is reported at the .zc line" 0
    echo "$out" | head -20 | sed 's/^/      /'
fi

# A header found only through -I, given either way (`-I dir` used to leave a bare -I in the
# C compiler's flags, and the check went off).
mkdir -p "$workdir/cnames/inc dir" "$workdir/cnames/src"
cp "$workdir/cnames/lib.h" "$workdir/cnames/inc dir/onlyinc.h"
printf 'include <onlyinc.h>\nfn main() {\n    lib_ad(1, 2);\n}\n' > "$workdir/cnames/src/viainc.zc"
for form in joined spaced; do
    if [ "$form" = joined ]; then
        out=$("$ZC" transpile "-I$workdir/cnames/inc dir" "$workdir/cnames/src/viainc.zc" -o "$workdir/cnames/viainc.c" 2>&1)
    else
        out=$("$ZC" transpile -I "$workdir/cnames/inc dir" "$workdir/cnames/src/viainc.zc" -o "$workdir/cnames/viainc.c" 2>&1)
    fi
    if echo "$out" | grep -q "Undefined function 'lib_ad'" && echo "$out" | grep -q "Did you mean 'lib_add'"; then
        check "a header found through -I ($form, a directory with a space) is read for its names" 1
    else
        check "a header found through -I ($form, a directory with a space) is read for its names" 0
        echo "$out" | head -8 | sed 's/^/      /'
    fi
done
printf 'include <onlyinc.h>\nfn main() {\n    println "{lib_add(1, 2)}";\n}\n' > "$workdir/cnames/src/viainc_ok.zc"
printf 'int lib_add(int a, int b) { return a + b; }\n' > "$workdir/cnames/inc dir/onlyinc.c"
out=$("$ZC" run -I "$workdir/cnames/inc dir" "$workdir/cnames/src/viainc_ok.zc" "$workdir/cnames/inc dir/onlyinc.c" -o "$workdir/cnames/viainc_ok" 2>&1)
if echo "$out" | grep -q "^3$"; then
    check "a program builds with -I and a directory with a space" 1
else
    check "a program builds with -I and a directory with a space" 0
    echo "$out" | head -8 | sed 's/^/      /'
fi

# //> include: the same, with a quoted directory holding a space, and a list of two.
mkdir -p "$workdir/cnames/d1" "$workdir/cnames/d2"
printf 'int lib_one(void);\n' > "$workdir/cnames/d1/one.h"
printf 'int lib_two(void);\n' > "$workdir/cnames/d2/two.h"
printf 'int lib_one(void) { return 1; }\nint lib_two(void) { return 2; }\n' > "$workdir/cnames/d1/one.c"
cat > "$workdir/cnames/src/directive.zc" <<EOF_Z
//> include: "$workdir/cnames/inc dir"
//> include: $workdir/cnames/d1 $workdir/cnames/d2
include <onlyinc.h>
include <one.h>
include <two.h>
fn main() {
    let n = lib_add(1, 2) + lib_one() + lib_two();
    println "{n}";
}
EOF_Z
sed 's/lib_two()/lib_tow()/' "$workdir/cnames/src/directive.zc" > "$workdir/cnames/src/directive_bad.zc"
out=$("$ZC" transpile "$workdir/cnames/src/directive_bad.zc" -o "$workdir/cnames/directive_bad.c" 2>&1)
if echo "$out" | grep -q "Did you mean 'lib_two'"; then
    check "//> include: directories (quoted, with a space; and a list) are read for names" 1
else
    check "//> include: directories (quoted, with a space; and a list) are read for names" 0
    echo "$out" | head -8 | sed 's/^/      /'
fi
out=$("$ZC" run "$workdir/cnames/src/directive.zc" "$workdir/cnames/inc dir/onlyinc.c" "$workdir/cnames/d1/one.c" -o "$workdir/cnames/directive" 2>&1)
if echo "$out" | grep -q "^6$"; then
    check "a program builds with //> include: directories (quoted, with a space; and a list)" 1
else
    check "a program builds with //> include: directories (quoted, with a space; and a list)" 0
    echo "$out" | head -8 | sed 's/^/      /'
fi

# Inside an interpolated string, which reaches the C as text, the same.
printf 'include "lib.h"\nfn main() {\n    let n = 1;\n    println "{lib_ad(1, 2)} {LIB_LIMTI} {n} {lib_add(1, 2)} {LIB_LIMIT}";\n}\n' > "$workdir/cnames/interp.zc"
out=$("$ZC" transpile "$workdir/cnames/interp.zc" -o "$workdir/cnames/interp.c" 2>&1)
if echo "$out" | grep -q "Undefined function 'lib_ad'" && echo "$out" | grep -q "Undefined name 'LIB_LIMTI'" &&
    [ "$(echo "$out" | grep -c '^error')" = 2 ]; then
    check "a misspelled C name inside string interpolation is reported" 1
else
    check "a misspelled C name inside string interpolation is reported" 0
    echo "$out" | head -10 | sed 's/^/      /'
fi

out=$(ZC_NO_C_NAMES=1 "$ZC" transpile "$workdir/cnames/bad.zc" -o "$workdir/cnames/bad.c" 2>&1)
if ! echo "$out" | grep -q "Undefined"; then
    check "ZC_NO_C_NAMES=1 turns the C names check off" 1
else
    check "ZC_NO_C_NAMES=1 turns the C names check off" 0
fi

# --- Imported files: diagnostics and #line name the imported file -----------

mkdir -p "$workdir/imported"
printf 'fn make() -> int {\n    let x: int = "text";\n    return 1;\n}\n' > "$workdir/imported/broken.zc"
printf 'import "imported/broken.zc"\nfn main() {\n    println "{make()}";\n}\n' > "$workdir/imported_main.zc"
out=$("$ZC" build "$workdir/imported_main.zc" -o "$workdir/imported_out" 2>&1)
if echo "$out" | grep -q "imported/broken.zc:2:" && ! echo "$out" | grep -q "imported_main.zc:"; then
    check "error in an imported file is reported at its own path" 1
else
    check "error in an imported file is reported at its own path" 0
fi

mkdir -p "$workdir/lib"
printf 'struct Counter { hits: int; }\nimpl Counter {\n    fn bump(self) { self.hits = self.hits + 1; }\n}\n' > "$workdir/lib/counter.zc"
printf 'import "lib/counter.zc"\nfn main() {\n    let c = Counter { hits: 0 };\n    c.bump();\n    println "{c.hits}";\n}\n' > "$workdir/lib_main.zc"
"$ZC" build --emit-c "$workdir/lib_main.zc" -o "$workdir/lib_out" >/dev/null 2>&1
line=$(awk '/#line/{l=$0} /^ZC_FUNC void Counter__bump\(Counter\* self\)$/{print l; exit}' "$workdir/lib_out.c" 2>/dev/null)
if echo "$line" | grep -q 'lib/counter.zc"'; then
    check "emit-c #line of an imported method names the imported file" 1
else
    check "emit-c #line of an imported method names the imported file" 0
fi

# --- --reload ------------------------------------------------------------

# A module for a reload host: its constructors and destructors become calls in
# _zc_module_init and _zc_module_fini (a @cfg one under its #if), and C's own
# attributes are left off. Without --reload, nothing of that is emitted.
printf '@constructor\nfn up() { }\n@cfg(RELOAD_X)\n@constructor\nfn up_x() { }\n@destructor\nfn down() { }\nfn main() { }\n' > "$workdir/reload_mod.zc"
"$ZC" transpile --reload -q "$workdir/reload_mod.zc" -o "$workdir/reload_mod.c" >/dev/null 2>&1
init=$(sed -n '/^void _zc_module_init(void)$/,/^}$/p' "$workdir/reload_mod.c" 2>/dev/null)
fini=$(sed -n '/^void _zc_module_fini(void)$/,/^}$/p' "$workdir/reload_mod.c" 2>/dev/null)
if echo "$init" | grep -q '^    up();$' && echo "$init" | grep -q '^#if defined(ZC_CFG_RELOAD_X)$' &&
    echo "$fini" | grep -q '^    down();$' &&
    ! grep -q '__attribute__((constructor\|__attribute__((destructor' "$workdir/reload_mod.c"; then
    check "--reload runs constructors and destructors from _zc_module_init/_fini" 1
else
    check "--reload runs constructors and destructors from _zc_module_init/_fini" 0
fi
"$ZC" transpile -q "$workdir/reload_mod.zc" -o "$workdir/plain_mod.c" >/dev/null 2>&1
if grep -q '__attribute__((constructor' "$workdir/plain_mod.c" &&
    ! grep -q '_zc_module_init' "$workdir/plain_mod.c"; then
    check "without --reload, constructors keep their C attribute" 1
else
    check "without --reload, constructors keep their C attribute" 0
fi

# --reload also describes the module's types, for a reload host to compare versions:
# _zc_manifest(). The numbers are the C compiler's, so the test compiles the module
# and prints the tables.
cat > "$workdir/manifest_mod.zc" <<'ZCEOF'
enum Code { A = 5, B, C = 10 }
enum Shape { Circle(f32), Rect(f32, f32), None }
struct Vec2 { x: f32, y: f32 }
struct Box<T> { v: T, n: int }
struct Flags { a: u32 : 3, b: u32 : 5 }
@cfg(RELOAD_NEVER_DEFINED) struct Hidden { q: i64 }
struct State { pos: Vec2, code: Code, shape: Shape, b: Box<int>, flags: Flags, name: char[16] }
fn tick(s: State*) -> int { return 0; }
ZCEOF
cat > "$workdir/manifest_print.c" <<'CEOF'
#include <stdio.h>
#include "manifest_mod.c"
int main(void)
{
    const ZcManifest *m = _zc_manifest();
    printf("format %u\n", m->format);
    for (unsigned i = 0; i < m->ntypes; i++)
    {
        const ZcMfType *t = &m->types[i];
        printf("%s (%s) kind %u size %u align %u\n", t->c_name, t->zc_name, t->kind, t->size,
               t->align);
        for (unsigned j = 0; j < t->nfields; j++)
        {
            printf("  %s: %s @%u size %u bits %u\n", t->fields[j].name, t->fields[j].type,
                   t->fields[j].offset, t->fields[j].size, t->fields[j].bits);
        }
        for (unsigned j = 0; j < t->nvariants; j++)
        {
            printf("  %s = %lld %s\n", t->variants[j].name, t->variants[j].value,
                   t->variants[j].payload ? t->variants[j].payload : "-");
        }
    }
    return 0;
}
CEOF
"$ZC" transpile --reload --cc "${CC:-cc}" -q "$workdir/manifest_mod.zc" \
    -o "$workdir/manifest_mod.c" >/dev/null 2>&1
out=$("${CC:-cc}" -w -I"$workdir" "$workdir/manifest_print.c" -o "$workdir/manifest_print" 2>&1 &&
    "$workdir/manifest_print")
manifest_ok=1
for want in 'format 5' \
    'Vec2 (Vec2) kind 1 size 8 align 4' '  x: float @0 size 4 bits 0' \
    '  y: float @4 size 4 bits 0' 'Box__int32_t (Box<int32_t>) kind 1 size 8 align 4' \
    'Code (Code) kind 3 size 4 align 4' '  A = 5 -' '  B = 6 -' '  C = 10 -' \
    'Shape (Shape) kind 4 size 12 align 4' '  Circle = 0 float' \
    '  Rect = 1 Tuple__float__float' '  None = 2 -' '  a: uint32_t @0 size 0 bits 3' \
    '  b: uint32_t @0 size 0 bits 5' '  pos: Vec2 @0 size 8 bits 0' \
    '  shape: Shape @12 size 12 bits 0' '  name: char[16] @36 size 16 bits 0'; do
    if ! printf '%s\n' "$out" | grep -qxF -- "$want"; then
        manifest_ok=0
        echo "    missing: $want"
    fi
done
if printf '%s\n' "$out" | grep -q '^Hidden '; then
    manifest_ok=0
    echo "    a type left out by @cfg is described"
fi
check "--reload describes the module's types in _zc_manifest()" $manifest_ok
[ $manifest_ok = 1 ] || printf '%s\n' "$out" | head -40
# A global is defined when the host compiles the module to own its globals, and extern
# otherwise, so a later version can use the first one's storage.
printf 'let count: int = 3;\nfn tick() { count = count + 1; }\n' > "$workdir/reload_global.zc"
"$ZC" transpile --reload -q "$workdir/reload_global.zc" -o "$workdir/reload_global.c" >/dev/null 2>&1
if grep -qx '#ifdef ZC_RELOAD_OWN_GLOBALS' "$workdir/reload_global.c" &&
    grep -qx 'extern int32_t count;' "$workdir/reload_global.c" &&
    grep -qF '{"count", "int32_t", sizeof(count), (void *)&count, "3", &_zc_mf_init_count, 0},' \
        "$workdir/reload_global.c"; then
    check "--reload lets the host keep a global's storage" 1
else
    check "--reload lets the host keep a global's storage" 0
fi
if grep -q '_zc_manifest\|ZcManifest' "$workdir/plain_mod.c"; then
    check "without --reload, no manifest is emitted" 0
else
    check "without --reload, no manifest is emitted" 1
fi
if "$ZC" transpile --reload --misra -q "$workdir/reload_mod.zc" -o "$workdir/reload_misra.c" 2>&1 |
    grep -q 'does not support MISRA'; then
    check "--reload with --misra is a clear error" 1
else
    check "--reload with --misra is a clear error" 0
fi

# --- Deeply nested imports -----------------------------------------------

# A chain of imports (d0 imports d1, which imports d2...) puts the parser on the
# stack again for every level. MAX_IMPORT_DEPTH (256) levels must work; one more
# must be a clear error at the import, not a stack overflow.
make_import_chain() {
    local dir="$1" n="$2" i
    mkdir -p "$dir"
    for ((i = 0; i < n; i++)); do
        if [ $((i + 1)) -lt "$n" ]; then
            printf 'import "d%d.zc"\n' $((i + 1)) > "$dir/d$i.zc"
        else
            : > "$dir/d$i.zc"
        fi
        printf 'fn d%d_f(x: int) -> int { return x + 1; }\n' "$i" >> "$dir/d$i.zc"
    done
    printf 'import "d0.zc"\nfn main() { println "deep-ok {d%d_f(1)}"; }\n' $((n - 1)) > "$dir/main.zc"
}

zc_abs="$(cd "$(dirname "$ZC")" && pwd)/$(basename "$ZC")"
make_import_chain "$workdir/chain_ok" 256
out=$(cd "$workdir/chain_ok" && "$zc_abs" run main.zc -o chain_ok 2>&1)
if echo "$out" | grep -q "deep-ok 2"; then
    check "imports nested 256 deep compile" 1
else
    check "imports nested 256 deep compile" 0
    echo "$out" | head -5
fi

make_import_chain "$workdir/chain_deep" 257
out=$(cd "$workdir/chain_deep" && "$zc_abs" transpile main.zc -o chain_deep.c 2>&1)
rc=$?
if [ "$rc" -eq 1 ] && echo "$out" | grep -q "Imports are nested more than 256 levels deep" &&
    echo "$out" | grep -q "d255.zc:1:8"; then
    check "imports nested 257 deep are an error at the import" 1
else
    check "imports nested 257 deep are an error at the import" 0
    echo "rc=$rc"
    echo "$out" | head -5
fi

# --- @packed and @align under tcc --------------------------------------------

# tcc can't make a packed struct more aligned, so zc says so when it writes C for tcc,
# and only then.
printf '@packed @align(4)\nstruct Both { a: u8; b: u32; }\nfn main() { let x: Both; }\n' \
    > "$workdir/packed_align.zc"
out_tcc=$("$ZC" transpile --cc tcc "$workdir/packed_align.zc" -o "$workdir/packed_align_tcc.c" 2>&1)
out_gcc=$("$ZC" transpile --cc gcc "$workdir/packed_align.zc" -o "$workdir/packed_align_gcc.c" 2>&1)
if echo "$out_tcc" | grep -q "tcc can't make a @packed struct more aligned: 'Both'" &&
    ! echo "$out_gcc" | grep -q "tcc can't"; then
    check "@packed with @align warns only when writing C for tcc" 1
else
    check "@packed with @align warns only when writing C for tcc" 0
fi

# An imported file's build directives apply, as the main file's do: its `//> link:`
# reaches the link (here a library that doesn't exist, so the linker names it).
mkdir -p "$workdir/linklib"
printf '//> link: -lzc_no_such_library\nfn helper() -> int { return 1; }\n' > "$workdir/linklib/needs_lib.zc"
printf 'import "linklib/needs_lib.zc"\nfn main() {\n    println "{helper()}";\n}\n' > "$workdir/link_main.zc"
out=$("$ZC" build "$workdir/link_main.zc" -o "$workdir/link_out" 2>&1)
if echo "$out" | grep -q "zc_no_such_library"; then
    check "an imported file's //> link: reaches the link" 1
else
    check "an imported file's //> link: reaches the link" 0
fi

# --- Installed layout ------------------------------------------------------

# `make install` puts the binary in PREFIX/bin and the stdlib in
# PREFIX/share/zenc. A copy of that layout in a temporary directory must find
# its own stdlib, without -I and whatever ZEN_SHARE_DIR it was built with. It
# runs from the temporary directory so that "./std" cannot be found instead.
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
prefix="$workdir/prefix"
mkdir -p "$prefix/bin" "$prefix/share/zenc"
cp "$ZC" "$prefix/bin/zc"
ln -s "$repo_root/std" "$prefix/share/zenc/std"
ln -s "$repo_root/std.zc" "$prefix/share/zenc/std.zc"
printf 'import "std/vec.zc"\nfn main() {\n    let v = Vec<int>::new();\n    v.push(7);\n    println "installed-std-ok {v.len}";\n}\n' > "$workdir/uses_std.zc"
out=$(cd "$workdir" && env -u ZC_ROOT "$prefix/bin/zc" run uses_std.zc -o uses_std 2>&1)
if echo "$out" | grep -q "installed-std-ok 1"; then
    check "installed zc finds its stdlib in PREFIX/share/zenc" 1
else
    check "installed zc finds its stdlib in PREFIX/share/zenc" 0
    echo "$out" | head -5
fi

echo "----------------------------------------"
echo "Results (CLI):"
echo "-> Passed:  $PASSED"
echo "-> Failed:  $FAILED"
echo "----------------------------------------"

if [ "$FAILED" -ne 0 ]; then
    echo -e "Failed tests:$FAILED_TESTS"
    exit 1
fi
echo "All CLI tests passed!"
exit 0
