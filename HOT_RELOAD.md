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
`make test-reload`), named in brackets after the claim.

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
        frame();
    }
    code.free();
    return 0;
}
```

- `build()` runs `zc transpile --reload` on the source, then compiles the C into memory
  with libtcc. `poll()` does the same if any `.zc` file under the source's directory
  changed (searched recursively; `watch(dir)` sets another directory). On Linux it
  learns of changes through inotify, so a poll with nothing changed costs microseconds;
  elsewhere it checks every file's modification time.
- `symbol(name)` gives the address of a function (or anything else) in the version loaded
  now. Look it up again after a `build()` or `poll()` that returned true.
- The host needs libtcc (0.9.27 or 0.9.28). `std/reload.zc` asks for it (`//> link:`), so
  `zc build host.zc` links it; a C compiler given the generated C needs `-ltcc -ldl
  -lpthread -lm`. Either needs `-L` for a libtcc outside the system's library path.
- The module's code can call the host's functions and any library linked into the host
  if the host exports its symbols (link it with `-rdynamic`).
- Call `poll()` only when none of the module's code is running: between frames, between
  requests.

`zc transpile --reload` is what the `Reloader` runs. It writes C for tcc (unless `--cc`
says otherwise), and adds what the `Reloader` needs: a description of the module's types,
globals and functions (the *manifest*), and the functions below in place of C's
constructor and destructor attributes. Without `--reload`, the output is the same as
before.

Not yet: Windows, `--misra` (an error with `--reload`), and C++ output.

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
- **Globals added, retyped or removed.** The new version gets new storage, with the
  values of those whose name and type are the same; one retyped to another numeric type
  is converted [`globals_change`]. A pointer to an old global then points at the old
  storage, which stays loaded.
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

- **A trait object calls the newest code**, even with old versions deleted. Its vtable's
  storage is kept like a global's and filled with the new version's functions on every
  reload [`trait_object_kept`].
- **A function pointer in the state** (a field of a function pointer type, in the host's
  block or a global, reached by value) **is pointed at the new version's function of the
  same name**. If that function's signature changed, it keeps calling the old one
  [`function_pointer_kept`].
- **Any other call into old code runs the new code**: a function pointer in a heap object,
  one handed to a library as a callback, a method kept the same way [`function_forwarded`],
  [`method_forwarded`]. Each old function jumps to the newest version's function of the
  same name. It follows its name from version to version only while the call means the
  same: the same signature, and no type it takes or returns changed (a pointer to the
  host's block is fine: the block is moved). Once that isn't so, it stays with the last
  version that matched, and the reload says so: `old code still runs for: take_pt (it
  takes or returns a Pt, which changed)` [`function_forward_type_changed`]. Needs old
  versions kept (the default); x86-64 only for now.

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
the state doesn't hold.

## What a reload doesn't handle

These don't stop a reload. They are what to keep in mind.

**Code that keeps running the old version.** Old versions stay loaded, by default until
`free()`, so all of these keep working, but run the code they were made with:

- a lambda: it keeps the body it was made with, as in hotreload-hx; what it calls by
  name runs the new code;
- a function whose signature changed, or that takes or returns a type that changed,
  called through a pointer kept from before (reported, see above);
- a string literal: it points into the old version's memory [`string_literal_kept`].

`keep_old(false)` deletes each old version when the next one loads, and old functions
then don't run the new code. Use it only when nothing like that is kept: a call through
one of them then crashes. Each old version costs its code's memory (printed: "N old
versions kept loaded (K KB)" with libtcc 0.9.27).

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
- a thread-local: libtcc can't give code compiled into memory thread-local storage, so in
  a reload build a thread-local is an ordinary global, shared by the module's threads (a
  release build keeps them per thread).

**Other limits.**

- A crash in the new code is a crash of the program: nothing catches it.
- Data reached only through pointers (heap objects) is never moved, only the pointers
  checked.
- Types are matched by their C name. Two structs of the same name under different `@cfg`
  are one entry (only one is compiled).
- Globals added change the storage's address (see above).

## Writing code that reloads well

- **Keep state in globals, or in one struct the host keeps.** Both survive, and follow
  layout changes.
- **Hold things by value, or by index or handle, not by pointer to a type you'll edit.**
  libwgf's actors are integer handles, so a game's state of handles moves freely; a
  pointer to a `Rock` stops the reload when `Rock` changes.
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
  every function with its address and C signature.
- **Storage.** Each mutable global, `static` local and vtable is emitted twice, behind
  `ZC_RELOAD_OWN_GLOBALS`: defined, and `extern`. The `Reloader` compiles the first
  version to define them, and every later version with them `extern`, pointed at that
  version's storage (`tcc_add_symbol`), and checks by address that each one is. When the
  globals change, the new version defines them again, and the values are moved into it.
  A version that owns storage is never deleted before `free()`.
- **Moving.** The `Reloader` keeps a copy of each version's manifest. On a reload it
  compares the two, checks everything that would have to move (a dry run), and only then
  moves it: globals into their new storage, the host's block in place through a copy.
- **Code pointers.** Vtables are refilled from a constant copy of the new version's.
  Function pointers in the state are matched by address against every loaded version's
  functions and pointed at the new version's of the same name and signature.
- **Forwarding.** The manifest lists every function and method with its C signature. On
  a reload, each function of the version replaced, and each older one still following its
  name, gets a 5-byte jump to the new version's function at its entry (tcc starts every
  function with an 11-byte prologue). Each version's code is mapped with mmap, so the
  versions are near each other and a 32-bit jump reaches; one that doesn't goes through a
  stub. Whether a name follows is decided once a reload, so a call through old code is one
  jump however old it is. At 3000 generated modules (about 12,000 functions) this adds
  about 15 ms to a reload.
- **Order of a reload:** build and compile the new version; check its state can move (or
  refuse); the old version's `@before_reload`; switch; move the state; reset what is
  `@reset_on_reload` or has an edited initial value; refill vtables, re-point function
  pointers; forward old functions; the new version's `@after_reload`.

## Reference

**Attributes** (they change nothing without `--reload`): `@reset_on_reload` on a global;
`@before_reload` and `@after_reload` on a function with no parameters.

**`Reloader`** (`std/reload.zc`):

| Call | |
|---|---|
| `Reloader::new(source, c_file)` | a reloader for the module whose root file is `source`; its C goes to `c_file` |
| `set_zc(path)`, `set_zc_flags(flags)`, `set_source(source)`, `watch(dir)`, `add_include(dir)` | where things are |
| `keep_old(bool)` | keep old versions loaded until `free()` (the default), or delete each |
| `verbose(bool)` | print a line for each reload and what it changed (the default) |
| `keep_state(ptr, size)` | the block the host passes to the module's functions |
| `require_same(type)` | refuse a version that changes `type` |
| `build()`, `poll()` | build now; build if a watched file changed. True if a new version loaded |
| `symbol(name)` | an address in the version loaded now |
| `print_changes()`, `type_changed(type)` | what the last reload changed |
| `manifest()` | the current version's `const ZcManifest*` |
| `version()`, `kept()`, `refused()` | versions loaded, old ones kept, builds not loaded |
| `free()` | the destructors, then every version deleted |
