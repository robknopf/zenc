# Hot reload

A program can keep running while some of its Zen C code is edited. The part that keeps
running is the *host*: it owns the window, the files and the libraries. The part that is
edited is the *module*: each time a file of it is saved, it is compiled again, in memory,
and the running program switches to it between two calls. State survives the switch,
including the values of the module's globals, and follows the code when an edit changes
its layout.

This is for development. A release build is the same source compiled ahead of time, with
none of this in it.

Everything here is checked by a test case in `tests/reload/cases/` (run by
`make test-reload`), named in brackets after the claim. `make test-reload` also runs every
compiler test program through the Reloader's own loader (`run_loader_tests.sh`), and
`make test-debugger` checks gdb, and VS Code's debug adapters where they're installed, on
reloaded code (`run_debugger_tests.sh`, `vscode_debug.py`), and
`make test-reload-soak` runs a long session of random edits checked against a model of what
must follow and what must stay (`tests/reload/soak.py`).

## Using it

The host uses `std/reload.zc`:

```zc
import "std/reload.zc"

fn main() -> int {
    let code = Reloader::new("src/game/game.zc", "build/game.c");
    if !code.build() { /* the errors are printed; fix and save */ }
    let frame: fn*() = code.symbol("game_frame");
    while running {
        // between frames, when none of the module's code is running:
        if code.poll() { frame = code.symbol("game_frame"); }
        // call(): a crash in the module goes back to the version before (see below)
        if !code.call(frame) { frame = code.symbol("game_frame"); }
    }
    code.free();
    return 0;
}
```

- `build()` runs `zc transpile --reload` on the source, then compiles the C with libtcc
  and loads it itself (see "How it works"). `poll()` does the same if any `.zc` file under the source's directory
  changed (searched recursively; `watch(dir)` sets another directory). On Linux it
  learns of changes through inotify, so a poll with nothing changed costs microseconds;
  elsewhere it checks every file's modification time, at most every 200 ms (so a save is
  seen up to that much later). `ZC_RELOAD_SCAN=1` in the environment makes it scan on
  Linux too, to test that.
- `add_include(dir)` gives tcc a directory to find the module's C headers in, and zc
  too: zc reads them to know the names they declare, so a misspelled C function or
  constant is an error at the `.zc` line, not tcc's "undefined symbol".
- `symbol(name)` gives the address of a function (or anything else) in the version loaded
  now. Look it up again after a `build()` or `poll()` that returned true.
- The host needs libtcc: zc's own, built with zc (by `make` or CMake) from
  `vendor/tinycc` into a `tcc/` beside zc (`zc --tcc-dir` names it; installed, it is in
  `share/zenc/tcc`), so no tcc need be installed. `std/reload.zc` asks for it (`//>
  include:`, `lib:`, `link:`), so `zc build host.zc` links it, and the Reloader gives it
  that directory's headers and runtime (it asks zc). A C compiler given the generated C
  needs `-I` and `-L` for that directory and `-ltcc -ldl -lpthread -lm`. Another libtcc
  works too, linked instead: the Reloader leaves it looking where it was built for (or
  `ZC_TCC_DIR`). It must be 0.9.28 or later where the Reloader loads versions itself
  (x86-64 Linux and Windows); elsewhere, where libtcc loads them, 0.9.27 works.
- The module's code can call the host's functions and any library linked into the host
  if the host exports its symbols (link it with `-rdynamic`).
- Call `poll()` only when none of the module's code is running: between frames, between
  requests.
- Call the module through `call(f)` or `call_with(f, arg)`, so a crash in it doesn't end
  the program. False means it crashed (or was skipped): look its functions up again.

`zc transpile --reload` is what the `Reloader` runs. It writes C for tcc (unless `--cc`
says otherwise), and adds what the `Reloader` needs: a description of the module's types,
globals and functions (the *manifest*), and the functions below in place of C's
constructor and destructor attributes. Without `--reload`, the output is the same as
before.

**Platforms.** x86-64 Linux and Windows. On Windows zc and the host are built with
MinGW-w64's gcc (its C runtime, msvcrt, is the one tcc's code uses), the host linked with
`-ltcc -lpthread -lm` and, where Linux has `-rdynamic`, `-Wl,--export-all-symbols`. zc's
libtcc is static there too, so a host has no `libtcc.dll` to find. MinGW's gdb debugs it
(below). A module is compiled for msvcrt alone (tcc's C library there), without MinGW's
POSIX functions, so std's time, threads, locks and directory listing are Windows' own there
[`module_uses_std`] (`Thread::cancel` fails: Windows can't stop another thread safely). The
libraries a module links (`-lws2_32`, which every build there has) are loaded for it, so a
module can use the network in a host that doesn't. `poll()` scans
the files there (no inotify yet: up to 200 ms to see a save). Elsewhere (macOS, arm64)
libtcc loads each version, without the Reloader's own loading: old versions stay loaded,
and a debugger sees none of them. Where the Reloader loads versions itself, a version it
can't load (a relocation it doesn't know, no room left in its address space or kept
memory) is not loaded: the reload says why, and the version running stays.

Not yet: `--misra` (an error with `--reload`), and C++ output.

## What a reload keeps

### Code

- **The new code runs** from the next call through a symbol looked up again
  [`code_change`].
- **A version that doesn't build is not loaded.** The errors (from `zc` or tcc) are
  printed, and the running version stays in use; the next save tries again
  [`build_failure`].

### Globals

- **A global keeps its value** [`global_keeps_value`], also with old versions deleted
  [`global_kept_old_deleted`], **and its address**: a pointer to a global, kept anywhere,
  still points at it [`global_pointer_stable`]. Every version uses the storage of the
  version that defined the globals.
- **Globals added, retyped or removed.** Each global keeps its storage, and a pointer to
  it stays good, kept anywhere [`global_storage_kept`]. Only a global that itself changes
  (retyped, or holding a type whose layout changed) gets new storage, with its value
  moved there; one retyped to another numeric type is converted [`globals_change`]. A
  pointer to its old storage still reads the old value.
- **A global whose initial value you edit takes the new value**: that is taken to be
  what you meant. The others keep theirs [`initial_value_edited`].
- **`@reset_on_reload`** on a global puts it back to its initial value on every reload,
  for anything derived from code, like a cache [`reset_on_reload`].
- **A function's `static` locals** keep their values like globals [`static_locals_kept`].
- **The standard library's state** is ordinary globals, so it is kept too: a hash seed,
  and a map filled before a reload [`hash_seed_kept`].
- A global whose name is a variable the process already has (libc's `opterr`) gets its
  own storage, not that variable [`global_name_taken`].

### State the host keeps

A host can keep the module's state itself, in a block it passes to the module's functions:

```zc
let state = calloc(1, STATE_BYTES);
code.keep_state(state, STATE_BYTES);
...
step(state);
```

The block keeps its contents across reloads [`state_survives`]. Its type is the one the
functions the host looked up with `symbol()` take a pointer to; the host never names it.

### Layout changes

When an edit changes the layout of a type the state is made of (a global's type, a
static's, the host's block's, or a type any of them holds by value), the state is moved
to the new layout:

- **Fields by name**, into nested structs too. A new field is zero (in a global, its
  initial value); one that is gone is dropped [`struct_field_inserted`],
  [`global_type_changed`], [`global_struct_grows`].
- **Arrays element by element**: a shorter array drops the last elements; in a longer
  one the new elements are zero (in a global, its initial value) [`state_converted`].
- **Enums by variant name**, so inserting or reordering variants keeps a stored value's
  meaning [`enum_variant_inserted`]; with payloads, the payload is moved too.
- **Numbers converted** when a field's type changes from one numeric type to another
  (`int32_t` to `int64_t`, `float` to `double`, an integer to a float) [`state_converted`].

- **std containers** whose element type changed: `Vec<T>`, `Stack<T>`, `Queue<T>`,
  `Map<V>` and `Box<T>`, held by the state. Their elements are moved to the new layout
  as above, into a new buffer of the same capacity, at the same places, and the
  container keeps working [`vec_elements_moved`], [`containers_moved`]. The old buffer is
  left as it was.

All of that at once [`state_moved_deep`]. The host's block is moved in place, so the
host's pointer to it stays valid.

### Around a reload

- **A reload doesn't start the program again.** `@constructor` functions run when the
  first version loads [`constructor_runs`], and the current version's `@destructor`
  functions at `free()` [`destructor_runs`], in the order a normal build runs them
  [`constructor_order`].
- **`@before_reload`** functions run on the old version just before a reload, and
  **`@after_reload`** ones on the new version once the state has moved; neither at the
  first load [`reload_hooks`]. Use them for work to redo after an edit: rebuilding a
  table, telling a library about new callbacks.

### Code kept from an old version

A function's address, wherever the module takes it, is the function's *stub*: a jump the
Reloader keeps, which no version owns. Each reload points every stub at the new version's
function of the same name. So:

- **A function pointer kept anywhere calls the newest code**: in the host's block or a
  global [`function_pointer_kept`], in a heap object, handed to a library as a callback, a
  method kept the same way [`function_forwarded`], [`method_forwarded`], even with every
  old version deleted [`function_forwarded_old_deleted`]. What `symbol()` gives the host is
  a stub too.
- **A trait object calls the newest code**: its vtable's storage is kept like a global's,
  and its entries are stubs [`trait_object_kept`].
- **Only while the call means the same**: the same signature, and no type it takes or
  returns changed (a pointer to the host's block is fine: the block is moved). Once that
  isn't so, the stub stays on the function it had, and the reload says so: `old code still
  runs for: take_pt (it takes or returns a Pt, which changed)`
  [`function_forward_type_changed`]. That old function keeps running, and what it calls by
  name runs the newest version's code where the call still means the same
  [`old_function_calls_new`].
- **A closure kept from before runs the same lambda of the new version**: the one with the
  same body (found wherever it moved in the file), or else, if its body was edited, the one
  going to the same place, taking and capturing the same. zc names a lambda by where it
  goes: a variable (`let on_hit = fn...`), a place assigned (`state.cb = fn...`), a call's
  argument (`on(0)`, the first of `on(...)`), a struct field (`Rock{on_hit}`), a return.
  One going nowhere nameable is found as the only lambda in its function that takes and
  captures the same. An edited callback runs its new body. One whose captures or parameters
  changed, or that can't be told apart, keeps its old body, and the reload names it: `a
  lambda in setup, to state.cb (the lambda going there takes or captures something else
  now)` [`lambda_follows`].
- **A string literal** kept from an old version (or any constant) still reads the same: the
  module's literals are in a pool the Reloader keeps, one copy of each
  [`string_literal_kept`], [`string_literal_old_deleted`].

### A crash in the new code

A crash in code called through `call()` or `call_with()` (SIGSEGV, SIGBUS, SIGFPE,
SIGILL: a bad pointer, a division by zero) comes back there instead of ending the
program, and the reload that brought that version is undone [`crash_rolled_back`]:

```
reload: version 2 crashed (SIGSEGV, a bad memory access); back to version 1. What the crashed code did to the heap, globals and libraries isn't undone. Fix and save to try again
```

- The version before runs again: its functions, its vtables, the function pointers in
  the state pointed back at it, and old functions that had followed into the crashed
  version follow the one before again [`crash_forward_undone`].
- The host's block is kept as it is if the reload didn't change its type (what the
  frames that did run did to it stays); if it did, it goes back to what it was before
  that reload [`crash_state_changed`].
- The crashed version stays loaded (something may point into it); its functions run the
  version before's, where the call means the same.
- The next save that loads replaces it, as usual.
- With nothing to go back to (the first version, or one already gone back from), calls
  are skipped until a version that loads replaces it [`crash_first_version`].
- A stack overflow is caught too [`stack_overflow_caught`], and a crash in a function
  copied out of an old version [`crash_in_copied_code`].

This is best effort: what the crashed code did to the heap, to globals and to libraries
(libwgf's state, a half-drawn frame) isn't undone, and a crash inside a library called by
the module leaves that library as the crash left it. A crash outside `call()` (in the
host, or in module code it calls directly) is a crash, as without the Reloader.

### What it tells you

Each reload prints what changed, unless `verbose(false)`; `print_changes()` prints it on
request [`layout_changes_reported`]. Errors and refusals are always printed.

```
reload: version 2 of src/game/game.zc loaded (zc 7 ms, tcc 6 ms, reload 1.1 ms)
reload: changes in version 2:
reload:   GameState: size 256 -> 260 bytes, lives_bonus added at byte 8, 14 fields shifted down 4 bytes (rock_prefabs to ship)
reload:   globals: game's type GameState changed
reload: moved the globals: game.lives_bonus added
```

The lines:

| Line | What it says |
|---|---|
| `changes in version N:` | the types and globals that changed, one type a line |
| `added at byte N`, `moved from byte A to B`, `shifted down/up N bytes` | a field's offset in the new layout ("down" is toward higher offsets) |
| `resized`, `retyped` | a field's size or type changed |
| `renumbered from A to B` | an enum variant's value changed |
| `also changed: X (holds Y)` | X is the same, but holds a Y, which changed, by value |
| `globals: ...` | globals added, removed, retyped, or whose type changed |
| `moved the globals: ...`, `moved the state (T) to the new layout: ...` | what moving did that wasn't simply keeping a value: added, dropped, converted, not carried |
| `back to their initial values: ...` | globals reset: `@reset_on_reload`, or an edited initial value |
| `function pointers in the state: ...` | which now call the new code, and which still call the old, and why |
| `old code still runs for: ...` | old functions that don't run the new code, and why: signature changed, a type they take or return changed, or gone |

`type_changed("T")` says whether the type `T` changed, itself or through what it holds.

## What a reload refuses

Some edits leave state that can't be moved safely. A version that would need one is
**not loaded**: the running version stays in use, unchanged, nothing has been touched
(every check is made before anything is), and the reload says why. Restart the program
to use the edit, or undo it; the next save tries again. `refused()` counts these.

| Refused when | The message | Why | What to do |
|---|---|---|---|
| The state holds a pointer to a type whose layout changed [`state_pointer_to_changed`] | `this version is not loaded: state.node points to a Node, whose layout changed` | what the pointer points to is in the old layout, and how much of it there is isn't known | restart; or hold such things by value, or by index or handle |
| A function the host looked up now takes or returns other types [`state_signature_changed`] | `reload_step now takes or returns other types, and the host calls it the old way` | the host would call it with the old arguments | restart (the host must change too) |
| The host's block is now a different type | `the state is now a Game (it was a GameState)` | there's no field-by-field match between two types | restart |
| The host's block's type grew past the block | `the state (GameState) no longer fits: N bytes, and the host's block is M` | moving it would write past the end | give `keep_state` a bigger block; restart |
| The functions the host looked up take pointers to several types, and one changed | `the state can't be moved: ... which is the state isn't known` | the host's block can't be identified | take one state type; restart |
| A type the host gave `require_same()` changed [`state_type_required`] | `T changed, so this version is not loaded: ...` | the host asked for it (`require_same` is a lower-level call; the checks above make it rarely needed) | restart |
| The module doesn't build | `src/x.zc did not build` / `did not compile`, after the errors | | fix and save |

A pointer to a type that *didn't* change is kept as it is, and so are pointers in types
the state doesn't hold. A std container's buffer isn't such a pointer: it is moved (see
"Layout changes").

## What a reload doesn't handle

These don't stop a reload. They are what to keep in mind.

**What an old version costs: nothing, after the next reload.** Only two versions stay
loaded: the one running and the one a crash goes back to. Every older one is released
whole, its memory and its registration with debuggers [`old_versions_released`]. Nothing
can still reach it: function pointers are stubs, literals are the pool's, globals the
store's. The one exception is a function a stub stays on (above): it is copied out of its
version when the stub stops following, a few hundred bytes, and kept. So a long session's
memory grows only with those copies, the pool's new literals and the stubs (16 bytes for
each function whose address was taken), not with the size of the module: at 1000 generated
modules, over 40 reloads, the host stays between 92 and 132 MB (with shared libraries it
grew 28 MB a reload; shrunk, 2.3 MB). Nothing is written to disk but the C file.
`keep_old(false)` releases the version replaced too, so a crash in the new code has nothing
to go back to.

Not caught this way, and so stale once the version that made them is released: a pointer
kept into an old version's own data other than the above (a C `static` in a `raw` block, a
const table that holds pointers), and the address of something by section and offset
rather than by name (C the Reloader didn't see, assembly). A released version's addresses
stay reserved for a long while (the space goes round once before reusing them), so using
one faults, and in code called through `call()` that is a crash rolled back like any other
[`stale_pointer_faults`].

**Values that are not carried** when a type's layout changes, and are left at zero (or a
global's initial value); each is in the "moved" line [`state_converted`]. Zero is a real
value: for an enum, the variant numbered 0.

- a field whose type changed to something that isn't a number, or from one struct type
  to another;
- an enum value whose variant is gone;
- a bit-field of a struct that changed;
- a union or a vector type that changed (which member is in use isn't known);
- a field renamed: it is a field dropped and a field added.

**Things a reload doesn't do again.** It changes what the next call runs; it doesn't make
that call. Code that has done its job, like `main`, a constructor, or the setup of the
current level, picks up an edit only when the program runs it again. Use `@after_reload`
for that.

**Kept by the module, not the host** (so they start again on every reload):

- a global or `static` local whose type only the C compiler works out (`let x = f();`
  with `f`'s type unknown to Zen C): write its type;
- a thread-local in a raw C block (`_Thread_local`, `__thread`): a reload build makes it
  an ordinary global, shared by the module's threads (libtcc can't give a module
  thread-local storage), and `zc transpile --reload` says so. A `@thread_local static` is
  per thread, and the host keeps it: zc gives it storage per thread from the host
  (`zr_tls_get`), by its function and name, so a thread's value lasts across reloads, and a
  new thread starts from the initial value of the version running then
  [`thread_local_per_thread`].

**Other limits.**

- A crash is caught only in code called through `call()` or `call_with()`, and only what
  is listed above is undone.
- Data reached only through pointers (heap objects) is never moved, only the pointers
  checked; the std containers above are the exception.
- Types are matched by their C name. Two structs of the same name under different `@cfg`
  are one entry (only one is compiled).

## Debugging

Each version is registered with debuggers as it loads, through the JIT interface gdb reads
(the one JIT compilers use: the version's object file, its sections at the addresses they
were loaded at), so gdb and lldb see the module's code as they see a library's, with no
file of it on disk. So does VS Code, through either of its debuggers that drive them:
CodeLLDB (`"type": "lldb"`), or the C/C++ extension with gdb (`"type": "cppdbg"`,
`"MIMode": "gdb"`). The C/C++ extension's Visual Studio debugger (`cppvsdbg`) and WinDbg
read only PDBs, and see none of it. A breakpoint in a `.zc` file needs VS Code to take
breakpoints there: the Zen C extension does (or `"debug.allowBreakpointsEverywhere"`).

- **Breakpoints by `.zc` file and line** bind when a version loads, in every version that
  has that line: set one in `src/game/rock.zc` before the program starts, or while it runs,
  and it stops there in the version running now and in the next one after a save.
- **Backtraces** go from the module's functions, at their `.zc` lines, into the host's,
  with their arguments.
- **Variables** (arguments, locals, globals), **stepping** by `.zc` line, and `list` (the
  C's `#line` directives give the `.zc` files by their absolute path).

It needs libtcc 0.9.28 (zc's is), which writes DWARF (`-gdwarf`).

A released version (see "What an old version costs") is unregistered: a breakpoint in it
goes with it, and stays set for the versions that have that line. A function copied out of
an old version (a stub that stays on it) has no debug information: a backtrace through it
shows its address. Compiling with debug
information costs a little per reload: about 50 ms at 1000 generated modules, 230 ms at
3000.

A module's global or function of the same name as one in the host or libc is the module's
own: its names bind to its own definitions first [`global_name_taken`].

On Windows, MinGW-w64's gdb (10.2 tried) reads it the same way: tcc's object files for
Windows have no frame information for gdb, so the Reloader writes it (every tcc function
starts the same way), and gives the system the version's unwind table (`.pdata`).

In VS Code, a `cppdbg` launch configuration for the host is enough; breakpoints in `.zc`
files show as unbound until the module loads. For gdb on the command line, `set breakpoint
pending on` lets a breakpoint wait for the module. lldb reads the same interface once
`settings set plugin.jit-loader.gdb.enable on` is set; that hasn't been tried yet.

## Writing code that reloads well

- **Keep state in globals, or in one struct the host keeps.** Both survive, and follow
  layout changes.
- **Hold things by value, in std containers, or by index or handle, not by pointer to a
  type you'll edit.** A `Vec<Rock>` in the state moves when `Rock` changes; libwgf's
  actors are integer handles, so a game's state of handles moves freely; a `Rock*` stops
  the reload when `Rock` changes.
- **Write the types of globals and statics** (`let speed: f32 = 3.0;`), so the host can
  keep them.
- **Keep lambdas thin**: `fn(x: int) { on_hit(x); }` calls `on_hit` by name, which runs
  the newest `on_hit`; edit that.
- **Put derived data behind `@reset_on_reload`, and rebuilding in `@after_reload`.**
- **Don't change the signature of a function the host looks up** without changing the
  host; that needs a restart anyway.
- **Pass one state type** to the host's entry points, if the host keeps the state.

## How it works

- **The manifest.** `zc transpile --reload` emits `_zc_manifest()`: every struct, union
  and enum the module defines (sizes, alignment and offsets as the C compiler laid them
  out: they are `sizeof`, `__alignof__` and `offsetof` expressions)
  [`manifest_describes_types`], every global with its type, address and initial value,
  every function with its address and C signature, and every lambda (`_zc_manifest_lambdas`:
  where it is, what it takes and captures, a hash of its C).
- **Loading.** Each version is compiled by libtcc to an object file (`-gdwarf`), which the
  `Reloader` reads, removes and loads itself, as a linker would: its sections into memory
  of its own, its names resolved (the store's globals first, then the host's and its
  libraries', then libtcc's runtime helpers, such as `__va_arg`), its relocations applied.
  tcc's object files are ELF on every system tcc runs on, so this part is the same
  everywhere; the relocations are the processor's (x86-64 for now). The object file, its
  sections given their addresses and its debug sections' relocations applied (tcc writes
  them the way gdb doesn't read), is what gets registered with debuggers.
- **One address space.** Every version, and everything that outlives one, is in one
  reservation of 1.5 GB of address space, so a 32-bit PC-relative reference between any
  two reaches. Its first 256 MB are kept memory, never released: the stubs, the pool, the
  functions copied out of old versions. The versions go in the rest, each where the last
  one ended, around and around, so a released version's addresses aren't reused until the
  space has gone round once [`space_wraps`]. When kept memory has no room for a version's
  stubs or literals, that version isn't loaded and the reload says so
  [`kept_memory_full`]. (`ZC_RELOAD_SPACE_KB` makes the space smaller, for those tests.)
- **Storage.** Each mutable global, `static` local and vtable is emitted twice, behind
  `ZC_RELOAD_OWN_GLOBALS`: defined, and `extern`. The `Reloader` compiles the first
  version to define them, and every later version with them `extern`, pointed at the
  storage kept (`tcc_add_symbol`), and checks by address that each one is. When the
  globals change, the new version is first compiled to define them, which shows what they
  are now; each one that changed gets storage of its own (allocated by the `Reloader`,
  holding its initial value), and the version is compiled against the kept storage again,
  so a reload that changes the globals compiles three times. A version that defines
  globals has its writable data apart from the rest; when it is released, that data stays
  (it is the store's storage) until `free()`.
- **Moving.** The `Reloader` keeps a copy of each version's manifest. On a reload it
  compares the two, checks everything that would have to move (a dry run), and only then
  moves it: globals into their new storage, the host's block in place through a copy.
- **Stubs.** Loading notes every place a version takes the address of one of its own
  functions rather than calling it: a pointer in its data (the manifest's aside), a GOT
  entry, a `lea`. Once the version is in use, each of them is pointed at the function's
  stub, 16 bytes of kept memory: `jmp *8(%rip)` and the address it jumps to. A reload
  decides, for each stub, whether its name follows (the manifest's signatures and types;
  for a lambda, the manifest's lambdas), and writes the new address there, or pins it.
  Direct calls stay direct. What `symbol()` returns is a stub as well.
- **The pool.** A read-only object with no relocation in it (a string literal, a number, a
  table of them) is loaded from a pool in kept memory: one copy of each, by its bytes and,
  if it has one, its name.
- **Releasing.** After a reload, every old version but the one a crash goes back to is
  released: unregistered, its memory given back and left reserved. A function a stub was
  pinned to was copied out of it first, when the stub was pinned: its bytes into kept
  memory, its relocations applied again there, a call to a function of the same version
  going through that function's stub if the call still means the same (the same
  signature, no type changed since), the rest copied with it.
- **Vtables** are refilled from a constant copy of the new version's (stubs too).
- **Crashes.** `call()` sets a `sigsetjmp` point and runs the function; handlers for
  SIGSEGV, SIGBUS, SIGFPE and SIGILL (on an alternate stack, so a stack overflow is caught
  too) jump back to it while module code runs, and otherwise do what the handler there
  before would have. Each reload keeps what undoing it needs: the version replaced (kept
  loaded), a copy of the host's block before the move, and where each stub it moved or
  pinned pointed before.
- **Order of a reload:** build and compile the new version; check its state can move (or
  refuse); the old version's `@before_reload`; switch; move the state; reset what is
  `@reset_on_reload` or has an edited initial value; refill vtables; move the stubs, and
  point the new version's references at them; copy out what a stub was pinned to; the new
  version's `@after_reload`; release the versions older than the one replaced.

## Reference

**Attributes** (they change nothing without `--reload`): `@reset_on_reload` on a global;
`@before_reload` and `@after_reload` on a function with no parameters.

**`Reloader`** (`std/reload.zc`):

| Call | |
|---|---|
| `Reloader::new(source, c_file)` | a reloader for the module whose root file is `source`; its C goes to `c_file` |
| `set_zc(path)`, `set_zc_flags(flags)`, `set_source(source)`, `watch(dir)`, `add_include(dir)` | where things are |
| `set_tcc_dir(dir)` | tcc's directory (its headers and runtime), for a libtcc other than zc's that doesn't find it; or `ZC_TCC_DIR` in the environment |
| `keep_old(bool)` | keep the version replaced loaded, for a crash to go back to (the default); or release it too |
| `verbose(bool)` | print a line for each reload and what it changed (the default) |
| `keep_state(ptr, size)` | the block the host passes to the module's functions |
| `require_same(type)` | refuse a version that changes `type` |
| `build()`, `poll()` | build now; build if a watched file changed. True if a new version loaded |
| `symbol(name)` | an address in the version loaded now (for a function, its stub, which follows its name) |
| `print_changes()`, `type_changed(type)` | what the last reload changed |
| `manifest()` | the current version's `const ZcManifest*` |
| `version()`, `kept()`, `refused()` | versions loaded, old ones still loaded (at most one), builds not loaded |
| `call(f)`, `call_with(f, arg)` | call a function of the module, catching a crash in it (see "A crash in the new code"); false if it crashed or was skipped |
| `crashes()` | crashes caught |
| `free()` | the destructors, then every version released |
