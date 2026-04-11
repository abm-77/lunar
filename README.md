# Lunar

## What is Lunar?
`lunar` is a framework for making graphical applications.

## Umbral
`umbral` is the language used by `lunar`. Source files use the `.um` extension; the compiler binary is `uc`.

## Building

**Prerequisites:** CMake 3.22+, Ninja, Clang, Python 3, GPU drivers with SPIR-V support.

Clone with submodules:
```sh
git clone --recurse-submodules <repo-url>
cd lunar
```

**First build** (configures and builds LLVM, GLFW, then Umbral — slow):
```sh
cmake -G Ninja -B build .
cmake --build build
```

**Rebuild only Umbral** after changing compiler/runtime code:
```sh
cmake --build build --target umbral
```

**Fastest iteration** once dependencies are installed:
```sh
ninja -C build/_b_umbral
```

The compiler binary lands at `build/_b_umbral/uc`.

## Compilation Pipeline

This is the full path from `.um` source to a running executable.

### Overview

```
.um source files
    │
    ▼
 ┌──────────────────────────────────────────────────┐
 │  uc (compiler)                                   │
 │                                                  │
 │  1. load modules    DFS import traversal, lex,   │
 │                     parse each module             │
 │  2. sema            collect → lower types →      │
 │                     method table → body check →   │
 │                     monomorphize                  │
 │  3. shader compile  BodyIR → um.shader MLIR →    │
 │     (if shaders)    SPIR-V MLIR → .umsh files    │
 │  4. asset pack      invoke ul to bundle .umsh +   │
 │     (if assets)     images/audio/fonts → .umpack  │
 │  5. codegen         LLVM IR (+ embedded .umpack    │
 │                     blob) → native .o              │
 │  6. link            cc obj.o runtime.a glfw.a     │
 │                     lz4.a -lvulkan → executable   │
 └──────────────────────────────────────────────────┘
    │
    ▼
 standalone executable (requires only libvulkan on the target)
 assets are embedded in the binary by default
```

### Step by step

**1. Module loading** (`loader.h`)

Starting from the entry `.um` file, `load_modules` performs a depth-first traversal
of `import` statements. Each module is lexed, parsed, and recorded in post-order
(dependencies before dependents, entry module last). Circular imports are rejected.
The root directory for module path resolution defaults to the entry file's parent
and can be overridden with `--root <dir>`.

**2. Semantic analysis** (`sema/sema.h`)

Runs in five sub-phases across all loaded modules:

1. **Collect** — gather all declarations into a global `SymbolTable` with per-module namespaces.
2. **Lower types** — convert type AST nodes to canonical `CTypeId`s in a shared `TypeTable`.
3. **Method table** — build a unified `MethodTable` from all `impl` blocks.
4. **Body check** — type-check every non-generic function body; record results in `BodySema` maps. Cross-module type aliases and paths are resolved here.
5. **Monomorphize** — demand-driven: each concrete use of a generic function or type queues a specialization. The queue drains iteratively until no new instantiations remain.

**3. Shader compilation** (optional, `compiler/shader/`)

Triggered when the program contains `@shader` types with `@stage` methods and
`--shader-out <dir>` is passed. Runs entirely inside `uc`:

1. `lower_to_mlir()` — BodyIR for each `@stage` method → `um.shader` MLIR dialect.
2. `run_spirv_lower()` — `um.shader` ops → MLIR `spirv` dialect via conversion patterns.
3. `collect_spirv_stages()` — serialize each `spirv.module` to in-memory SPIR-V words.
4. `collect_refl()` — extract vertex input layout from `@vs_in` pod fields → reflection data.
5. `emit_umsh()` — write stages + reflection → `<ShaderName>.umsh` in the output directory.

Stage methods are **not** compiled to native code — only to `.umsh` sidecar files.

**4. Asset packing** (optional)

After shader compilation (or when `--asset-dir <dir>` is given), `uc` invokes the
`ul` asset linker as a subprocess to produce `assets.umpack`:

```
ul --pack <out_dir>/assets.umpack --compress \
    <shader_out>/*.umsh \
    <asset_dir>/*.png <asset_dir>/*.wav ...
```

`ul` decodes assets at pack time so the runtime pays zero decode cost:

| Extension | Decode | Stored as |
|-----------|--------|-----------|
| `.png` `.jpg` `.bmp` | stb_image | raw RGBA8 pixels |
| `.wav` `.ogg` | miniaudio | float32 stereo PCM |
| `.ttf` `.otf` | FreeType + msdfgen | MTSDF atlas (RGBA8) + glyph metrics |
| `.umsh` | — | raw bytes |

The resulting `.umpack` is an LZ4-compressed archive keyed by filename. At runtime,
`@shader_ref(TypeName)` computes `fnv1a64("TypeName.umsh")` at compile time; the
runtime asset loader uses the same hash to look up entries.

By default, the `.umpack` is embedded into the binary during codegen (see step 5)
so the final executable is fully self-contained. Pass `--no-embed-assets` to keep it
as a sidecar file instead.

**5. LLVM codegen** (`codegen/codegen.h`)

1. Declare globals and functions as LLVM IR values (generic templates are skipped; only monomorphized instances are emitted).
2. Emit function bodies via `FuncEmitter`.
3. Emit a site table (`__um_sites`) for runtime allocation tracking.
4. Embed the `.umpack` as a constant byte array (`__umpack_embedded_data` / `__umpack_embedded_size`) in the LLVM module. When no assets exist, these are null/0.
5. Run LLVM optimization passes (`-O0` through `-O3`).
6. Lower to a native object file (`.o`) via the host target machine.

**6. Linking**

`uc` finds `cc` on `PATH` and invokes it to produce the final executable:

```
cc umbral_<pid>.o umbral_runtime.a umbral_glfw.a umbral_lz4.a \
    -lvulkan -ldl -lpthread -lX11 -lm -lz -o <output>
```

The runtime library, GLFW, and LZ4 are embedded as byte blobs inside `uc` at build
time and written to temp files before linking. The resulting executable is
self-contained — only `libvulkan` (and X11/Cocoa on the respective platform) must be
present on the target system.

### Compiler flags

| Flag | Effect |
|------|--------|
| `<file>.um` | entry source file |
| `-o <path>` | output executable path (default: `a.out`) |
| `--root <dir>` | module root for import resolution (default: entry file's parent) |
| `--shader-out <dir>` | emit `.umsh` files and `assets.umpack` to this directory |
| `--asset-dir <dir>` | include images, audio, and fonts from this directory in the asset pack |
| `--no-embed-assets` | keep `.umpack` as a sidecar file instead of embedding in the binary |
| `--dump-ir` | print LLVM IR to stdout and stop (no object/link) |
| `--dump-shader-mlir` | print um.shader MLIR to stdout and stop |
| `-g` | emit DWARF debug info |
| `-O0` `-O1` `-O2` `-O3` | optimization level |

### Typical invocations

```sh
# compile and run a simple program (no shaders, no assets)
uc main.um --root umbral/source/std -o main && ./main

# compile with shaders and an asset directory
uc main.um --root umbral/source/std \
    --shader-out build/shaders --asset-dir assets/ \
    -o build/game

# inspect the generated LLVM IR
uc main.um --root umbral/source/std --dump-ir

# inspect the shader MLIR before SPIR-V lowering
uc main.um --root umbral/source/std --dump-shader-mlir
```

## Running Compiler Tests

Unit tests live in `umbral/source/compiler/test/unit/` and are built with the inner `umbral` build.

**Build and run all unit tests:**
```sh
ctest --test-dir build/_b_umbral -V
```

Or run individual test executables:
```sh
ninja -C build/_b_umbral sema_tests meta_tests runtime_tests lexer_tests parser_tests
./build/_b_umbral/sema_tests
```

**Run lit functional tests** (requires `uc` to be built):
```sh
ninja -C build/_b_umbral check-lit
```

Lit test sources live in `umbral/source/compiler/test/`. Each file must have a `# RUN:` directive. Use `%S` to reference paths relative to the test file's directory and `%uc` for the compiler.

---

## Umbral Language Reference

Comments use `#`. All module-level items end with an optional `;`. Statements inside function bodies require `;` except blocks.

All declarations (functions, types, globals) are unified `const`/`var` bindings. Function literals and struct types are first-class expressions.

### Module Items

```
module_item := 'import' path ['=>' ident] ';'
             | annotation* ('const' | 'var') ident ['<' generic_params '>']
                   (':=' expr | [':' type] ['=' expr]) [';']
             | '@extern' ('const' | 'var') ident ':' type [';']
             | 'impl' ident ['<' ident (',' ident)* '>'] '{' impl_method* '}'

annotation     := '@pub' | '@gen'
path           := ident ('.' ident)*
generic_params := ident ':' ('type' | type) (',' ident ':' ('type' | type))*
impl_method    := ['@gen'] ('const' | 'var') ident ':=' expr [';']
```

```
# import with optional alias (defaults to last path segment)
import sys.io.fmt;           # alias: fmt
import sys.mem => mem;       # explicit alias

# named struct type
const Point := struct {
    x: i32,
    y: i32,
}

# function
const add := fn(a: i32, b: i32) -> i32 {
    return a + b;
}

# public function — exported to importing modules
@pub const greet := fn() -> void {}

# extern declaration — symbol defined outside this module (e.g. in C runtime)
@extern const abs : fn(v: i32) -> i32;
@extern const errno : i32;

# function type alias — FnType expression (param names optional, no body)
const BinaryOp := fn(i32, i32) -> i32;
const BinaryOpNamed := fn(a: i32, b: i32) -> i32;  # names discarded

# var with explicit type and no initializer
var counter: i32;

# const with explicit type and initializer
const max_size: i32 = 1024;
```

### Types

```
type := '&' ['mut'] type                             # reference
      | '[]' type                                    # slice: fat pointer { ptr, len }
      | '[' (int | ident) ']' type                  # array: [N]T
      | '(' type (',' type)+ ')'                    # tuple
      | 'vec' '<' type ',' int '>'                  # builtin vector: vec<T, N> (N = 2/3/4)
      | 'mat' '<' type ',' int ',' int '>'          # builtin matrix: mat<T, N, M> (cols, rows)
      | 'fn' '(' ([ident ':'] type (',' [ident ':'] type)*)? ')' '->' type  # function type; param names optional
      | ident ['<' type (',' type)* '>']            # named type, optional generic args
```

```
const a: i32;                       # named type
const b: &i32;                      # immutable reference
const c: &mut i32;                  # mutable reference
const d: (i32, i32);                # tuple type
const e: fn(i32, i32) -> i32;       # function type
const f: [10]i32;                   # array of 10 i32s
const g: [N]T;                      # array with const-generic count
const h: []i32;                     # slice of i32
const j: vec<f32, 3>;                # builtin vector type (3 x f32)
const k: Option<i32>;               # generic named type
const m: Array<i32, 10>;            # generic type with const-generic arg
```

### Generics

Type parameters are declared with `Name: type`. Const-generic parameters use `Name: IntType`.

```
# generic struct
const Option<T: type> := struct {
    has_value: bool,
    value: T,
}

# multiple type and const-generic params
const Array<T: type, N: u32> := struct {
    data: [N]T,
    count: u32,
}

# generic function
const identity<T: type> := fn(x: T) -> T {
    return x;
}
```

### Type Aliases

Use `:=` for simple type aliases and `const Name : type = T;` for generic types.

```
# simple alias — := form works fine
const Handle := u64;

# generic alias — explicit `: type =` required
const MaybeFoo : type = Option<Foo>;

# methods on the aliased type resolve through the alias
const val := MaybeFoo::some(my_foo);
```

### impl Blocks

Methods are defined in `impl` blocks. Instance methods take `&self` or `&mut self` as the first parameter. Static methods do not. Methods are always accessible from any module that can see the type — visibility lives on the type declaration, not on individual methods.

```
impl Option<T> {
    const create := fn(v: T) -> Option<T> {
        return Option<T> { has_value = true, value = v };
    }

    const none := fn() -> Option<T> {
        return Option<T> { has_value = false };
    }
}

impl Array<T, N> {
    const create := fn() -> Array<T, N> {
        return Array<T, N> { data = [N]T{}, count = 0 };
    }

    const push := fn(&mut self, v: T) -> void {
        if (self.count >= N) return;
        self.data[self.count] = v;
        self.count += 1;
    }
}
```

### Enums

```
const Color := enum {
    Red,
    Green,
    Blue,
}

const is_red := fn(c: Color) -> bool {
    return c == Color::Red;
}
```

### Statements

```
stmt := 'const' ident (':=' expr | [':' type] ['=' expr]) ';'
      | 'var'   ident (':=' expr | [':' type] ['=' expr]) ';'
      | 'return' [expr] ';'
      | 'if' '(' expr ')' block ('else' ('if' '(' expr ')' block | block))*
      | 'for' '(' [for_part] ';' [expr] ';' [for_part] ')' block
      | 'for' '(' ('const' | 'var') ident ':=' expr ')' block   # range-for
      | '@if' '(' expr ')' block ('@else' block)?               # compile-time if
      | '@assert' '(' expr [',' string_lit] ')' ';'             # compile-time assert
      | block
      | expr (assign_op expr)? ';'

for_part   := ('const' | 'var') ident ':=' expr | expr assign_op expr
assign_op  := '=' | '+=' | '-=' | '*=' | '/=' | '%=' | '&=' | '|=' | '^='
```

```
# immutable binding — inferred type with :=
const x := 10;
const y: i32 = 20;   # explicit type
const z: i32;        # explicit type, no initializer

# mutable binding
var counter := 0;

# assignment and compound assignment
counter = 1;
counter += 2;

# return
return;
return counter + 1;

# if / else if / else
if (x > 0) {
    return x;
} else if (x == 0) {
    return 0;
} else {
    return -x;
}

# classic for loop — all three parts optional
for (var i := 0; i < 10; i += 1) {}
for (; done == false;) {}
for (;;) {}   # infinite loop

# range-for — iterate over an @iter(array_or_slice) or a slice directly
const arr := [3]i32{ 1, 2, 3 };
for (const x := @iter(arr)) {
    # x is each i32 element
}

# compile-time if — condition must be evaluable at compile time
@if(N <= 8) {
    # emitted only when N <= 8
}
@else {
    # emitted only when N > 8
}

# compile-time assertion — fails compilation with optional message
@assert(N > 0, "N must be positive");
@assert(0 < 1);   # always passes
```

### Expressions

Operator precedence (low to high):

| Operators             | Precedence |
|-----------------------|------------|
| `\|\|`                | 30         |
| `&&`                  | 35         |
| `\|` (bitwise OR)     | 36         |
| `^` (bitwise XOR)     | 37         |
| `&` (bitwise AND)     | 38         |
| `==` `!=`             | 40         |
| `<` `<=` `>` `>=`     | 50         |
| `+` `-`               | 60         |
| `*` `/` `%`           | 70         |
| unary `-` `!` `*` `&` | 80         |
| postfix `.` `[]` `()` | 90         |

```
primary := int_lit
         | float_lit
         | string_lit
         | 'true' | 'false'
         | '(' ')'                                                      # unit tuple
         | '(' expr ',' expr (',' expr)* ')'                            # tuple literal
         | '(' expr ')'                                                 # grouped expr
         | ident ['<' type (',' type)* '>'] '{' (ident '=' expr ','?)* '}'  # struct init (named fields)
         | ident ['<' type (',' type)* '>'] '(' (expr ','?)* ')'           # struct init (positional)
         | ident ['<' type (',' type)* '>'] '::' ident (...)            # path expression
         | ident                                                         # identifier
         | '[' (int | ident) ']' type '{' (expr ','?)* '}'             # array literal
         | '[]' type '{' (expr ','?)* '}'                              # slice literal
         | 'struct' '{' (ident ':' type ','?)* '}'                     # StructType
         | 'struct' '{' (ident ':' type '=' expr ','?)* '}'            # StructExpr (anon struct value)
         | 'fn' '(' (ident ':' type ','?)* ')' '->' type block         # FnLit
         | 'fn' '(' (type ','?)* ')' '->' type                         # FnType
         | 'enum' '{' (ident ','?)* '}'                                # EnumType
```

**Disambiguation:**
- After `fn(`, if the first param is `ident :` (or `&self` / `&mut self`) → FnLit with body. Otherwise → FnType.
- After `struct {`, if the first field has `= expr` after its type → StructExpr. Otherwise → StructType.
- After `Ident <`, if `>` is followed by `{` or `::` → generic type args. Otherwise → binary `<`.

**Numeric literals:**
- Decimal: `42`, `1_000_000` (underscores are ignored and may appear anywhere within digit sequences)
- Hex: `0xFF`, `0xFF_00`
- Binary: `0b1010`, `0b1111_0000`
- Type suffix on integers: `u8` `u16` `u32` `u64` `i8` `i16` `i32` `i64` — e.g. `255u8`, `100_000u32`
- Type suffix on floats: `f32` `f64` — e.g. `3.14f32`, `2.718_281_828f64`
- Without a suffix, integer literals default to `i32` and float literals to `f64` if the type cannot be inferred from context.

```
# struct initializer — fields assigned by name
const p := Point { x = 10, y = 20 };
const empty := Foo {};

# struct initializer — fields assigned by position (no names, strict order)
const p2 := Point(10, 20);

# anonymous struct value (StructExpr)
var origin := struct { x: i32 = 0, y: i32 = 0 };

# generic struct initializer
const opt := Option<i32> { has_value = true, value = 42 };

# path expressions
const v := Color::Red;                       # enum variant
const q := Quad::max(a, b);                  # static method
const arr := Array<i32, 10>::create();       # static method with type args

# array literals
const buf := [3]i32{ 1, 2, 3 };   # with values
const zero := [10]i32{};           # zero-initialized
const gen := [N]T{};               # const-generic count

# slice literal — stack-allocates elements, returns { ptr, len }
const sl: []i32 = []i32{ 1, 2, 3 };

# logical operators
if (x > 0 && y > 0) {}
if (done || failed) {}

# StructType expression
const Pair := struct { x: i32, y: i32 };

# StructExpr — anonymous struct value with field defaults
const pt := struct { x: i32 = 1, y: i32 = 2 };

# FnLit
const adder := fn(a: i32) -> i32 { return a + 1; };

# FnType
const BinaryOp := fn(i32, i32) -> i32;
```

### Reference Expressions

```
const r  := &x;       # immutable reference
const rm := &mut x;   # mutable reference
const v  := *ptr;     # dereference
```

### Slices

A slice `[]T` is a fat pointer `{ ptr: *T, len: u64 }`. Arrays coerce implicitly to slices.

```
const arr := [4]i32{ 10, 20, 30, 40 };
const sl: []i32 = arr;    # array coerces to slice

# slice literal (stack-allocates)
const sl2: []i32 = []i32{ 1, 2, 3 };

# indexing works on both arrays and slices
const elem := sl[1];
```

### Imports and Cross-Module Access

```
import game.ecs.world;         # alias = last segment: world
import game.ecs.world => w;    # explicit alias

const e := world::spawn(1);    # call exported function
```

Pass `--root <dir>` to `uc` to set the module root (defaults to the entry file's parent directory).

---

### Built-in Intrinsics

All `@name` forms are intrinsics — there are no separate keywords for visibility or extern.

**Declaration annotations** (appear before `const`/`var` at module level or in impl blocks):

| Annotation | Description |
|------------|-------------|
| `@pub` | export this declaration; required for cross-module access |
| `@gen` | enable compile-time metaprogramming in this declaration |
| `@extern const name : type` | declare an externally-defined symbol (no body emitted) |
| `@shader` | mark a struct as a shader descriptor (holds IO field groups) |
| `@shader_pod` | mark a struct as a plain-old-data shader IO type |
| `@stage(vertex\|fragment)` | mark an impl method as a shader entry point (skipped in native codegen) |

**Shader field annotations** (on fields inside `@shader_pod` structs):

| Annotation | Description |
|------------|-------------|
| `@vs_in` | vertex shader input field (read-only in vertex stage) |
| `@vs_out` | vertex shader output field (write-only in vertex stage) |
| `@fs_in` | fragment shader input field (read-only in fragment stage) |
| `@fs_out` | fragment shader output field (write-only in fragment stage) |
| `@draw_data` | per-draw structured data field (read-only in all stages) |
| `@location(n)` | bind this IO field to vertex attribute / fragment output location `n` |
| `@builtin(position)` | bind this IO field to `gl_Position` (vertex output built-in) |

**Expression intrinsics** (used inside function bodies or type positions):

| Intrinsic | Description |
|-----------|-------------|
| `@size_of(T)` | size of type T in bytes → `u64` |
| `@align_of(T)` | alignment of type T in bytes → `u64` |
| `@as(expr, T)` | numeric / pointer cast to type T |
| `@bitcast(expr, T)` | reinterpret bits as type T (same size) |
| `@slice_cast(expr, T)` | reinterpret slice element type (same total bytes) |
| `@iter(arr_or_slice)` | wrap array or slice in a runtime iterator for range-for |
| `@site_id()` | compile-time call-site identifier → `u32` (used for allocation tagging) |
| `@memcpy(dst, src, n)` | copy `n` bytes from `src` to `dst` |
| `@memmov(dst, src, n)` | move `n` bytes (handles overlap) |
| `@memset(dst, val, n)` | fill `n` bytes at `dst` with `val` |
| `@memcmp(a, b, n)` | compare `n` bytes; returns `i32` (same as C `memcmp`) |
| `@shl(a, b)` | left bit shift `a << b` → same type as `a` |
| `@shr(a, b)` | right bit shift `a >> b` → same type as `a` (logical for unsigned, arithmetic for signed) |

**Shader expression intrinsics** (valid only inside `@stage` methods):

| Intrinsic | Description |
|-----------|-------------|
| `@texture2d(idx: u32)` | opaque texture handle at descriptor index `idx` |
| `@sampler(idx: u32)` | opaque sampler handle at descriptor index `idx` |
| `@sample(tex, samp, uv)` | sample `tex` with `samp` at UV coordinates → `vec4` |
| `@draw_id()` | current draw index (`gl_DrawID`) → `u32` |
| `@vertex_id()` | current vertex index (`gl_VertexIndex`) → `u32`; use for procedural vertex-pull |
| `@draw_packet(id: u32)` | opaque per-draw data packet handle |
| `@frame_read<T>(offset: u32)` | read `T` from the per-frame data buffer at byte `offset` |
| `@shader_ref(TypeName)` | CPU-side; returns `u64` FNV-1a 64-bit hash of `"TypeName.umsh"`; use with `gfx::pipeline_create` |

---

### Compile-Time Metaprogramming (`@gen`)

`@gen` marks a declaration as a metaprogramming construct. Inside an `@gen` body, compile-time intrinsics (`@if`, `@assert`, `@fields`, `@field`) are available.

#### `@gen` type — conditional struct shape

```
@gen const SmallVec<T: type, N: u32> := {
    @assert(N > 0, "N must be positive");
    @if(N <= 8) {
        struct { data: [8]T, len: u32 }
    }
    @else {
        struct { data: []T, len: u32 }
    }
}
```

The block evaluates at monomorphization time: `@assert` runs, then the first matching `@if` branch determines the struct layout.

#### `@gen` function — conditional code paths

```
impl SmallVec<T, N> {
    @gen const push := fn(&mut self, x: T) -> void {
        @if(N <= 8) {
            self.data[self.len] = x;
            self.len += 1;
        }
        @else {
            self.grow_and_push(x);
        }
    }
}
```

Only the live branch is emitted in codegen; dead branches are discarded.

#### `@fields` and `@field` — compile-time field iteration

```
const Player := struct {
    x: i32,
    y: i32,
    health: u32,
    name: []u8,
};

@gen const print_field_names := fn(p: &Player) -> void {
    for (const field := @fields(Player)) {
        fmt::println("{}", []fmt::Arg{ fmt::arg_bytes(field.name) });
    }
}

const Vec2 := struct { x: i32, y: i32 };

@gen const sum_fields := fn(v: &Vec2) -> i32 {
    var total: i32 = 0;
    for (const field := @fields(Vec2)) {
        total += @field(v, field);   # accesses the concrete field each iteration
    }
    return total;
}
```

`@fields(T)` is a compile-time-only iterator: the loop body is unrolled once per field at codegen time. `field.name` gives the field's name as `[]u8`; `@field(obj, field)` reads the field's value with its concrete type.

---

### Shader System

Umbral shaders are written in the same language as regular Umbral code. The compiler
emits a `.umshaders` sidecar file alongside the native object; the `ul` asset linker
converts it to SPIR-V and UMRF vertex-reflection blobs.

#### Struct roles

- `@shader_pod` — a plain-old-data struct carrying vertex IO fields. Fields carry
  `@location(n)` or `@builtin(position)` plus a directional annotation:
  `@vs_in` (vertex input), `@vs_out`/`@fs_in` (vertex→fragment interpolant),
  `@fs_out` (fragment output), or `@draw_data` (per-draw SSBO).
- `@shader` — a descriptor struct grouping the `@shader_pod` fields used by a draw
  call. One field per role: `vin`, `vout`, `fin`, `fout`.

#### Stage methods

Shader entry points are `impl` methods annotated with `@stage(vertex)` or
`@stage(fragment)`. They take `&mut self` where `self` is the `@shader` type.
The sema layer enforces access rules:

- `@vs_in` / `@fs_in` / `@draw_data` fields: read-only (cannot be assigned).
- `@vs_out` / `@fs_out` fields: write-only (cannot be read in an expression).
- `@vs_in` / `@vs_out` fields are inaccessible from the fragment stage and vice versa.

Stage methods are **not** compiled to native code — only to the `.umshaders` sidecar.

#### Example

```
import math.types;
import sys.gfx.shader;

@shader_pod const SpriteVertex := struct {
    @location(0) pos: types::vec2,
    @location(1) uv:  types::vec2,
};

@shader_pod const SpriteVsOut := struct {
    @builtin(position) clip: types::vec4,
    @location(0)       uv:   types::vec2,
};

@shader_pod const SpriteFsOut := struct {
    @location(0) color: types::vec4,
};

@shader const SpriteShader := struct {
    @vs_in  vin:  SpriteVertex,
    @vs_out vout: SpriteVsOut,
    @fs_in  fin:  SpriteVsOut,
    @fs_out fout: SpriteFsOut,
};

impl SpriteShader {
    @stage(vertex)
    const vert := fn(&mut self) -> void {
        self.vout.clip = types::vec4(self.vin.pos.x, self.vin.pos.y, 0.0, 1.0);
        self.vout.uv   = self.vin.uv;
    };

    @stage(fragment)
    const frag := fn(&mut self) -> void {
        const tex  := @texture2d(0);
        const samp := @sampler(0);
        self.fout.color = @sample(tex, samp, self.fin.uv);
    };
}
```

---

### Standard Library

The standard library lives in `umbral/source/std/` and is available with `--root <path/to/std>`.

#### `sys.io.fmt` — formatted output

```
import sys.io.fmt;

fmt::println("{} + {} = {}", []fmt::Arg{
    fmt::arg_i32(a),
    fmt::arg_i32(b),
    fmt::arg_i32(a + b),
});
```

`fmt::Arg` constructors: `arg_i8`, `arg_i16`, `arg_i32`, `arg_i64`, `arg_u8`, `arg_u16`, `arg_u32`, `arg_u64`, `arg_f32`, `arg_f64`, `arg_bytes` (for `[]u8`).

#### `sys.mem` — tracked heap allocation

```
import sys.mem => mem;

# allocate N elements of type T (tagged for debugging)
const alloc := mem::Alloc<i32>::create(N as u64, 0);
const slice := alloc.slice_mut();   # []i32, mutable view
# ...
alloc.destroy();
```

#### `sys.window` — window creation and lifecycle

Requires a display with a GPU that supports SPIR-V. The produced binary is self-contained — no GLFW install needed on the target system.

```
import sys.window => window;

var win := window::Window::create("My App", 1280, 720);

for (; !win.should_close();) {
    window::Window::poll_events();
    # ...game loop...
}

win.destroy();
```

`Window` methods:

| Method | Description |
|--------|-------------|
| `Window::create(title: []u8, width: i32, height: i32) -> Window` | create and show a window |
| `Window::poll_events()` | process OS events; call once per frame (static) |
| `win.should_close() -> bool` | true when the OS or user has requested close |
| `win.request_close()` | programmatically set the close flag |
| `win.destroy()` | destroy the window and release resources |

#### `sys.input` — keyboard and mouse input

Must import `sys.window` as well. Call `input::begin_frame` once per frame before any queries.

```
import sys.window => window;
import sys.input => input;

var win := window::Window::create("test", 640, 480);

for (; !win.should_close();) {
    window::Window::poll_events();
    input::begin_frame(&win);

    if (input::key_pressed(&win, input::Key::Space)) { ... }
    if (input::mouse_pressed(&win, input::MouseButton::Left)) {
        const x := input::mouse_x(&win);
        const y := input::mouse_y(&win);
    }
}
```

Key query functions (all take `w: &Window` and a `Key`):

| Function | Description |
|----------|-------------|
| `key_down(w, k)` | true while key is held |
| `key_up(w, k)` | true while key is not held |
| `key_pressed(w, k)` | true on the frame the key went down |
| `key_released(w, k)` | true on the frame the key went up |

Mouse button query functions (take `w: &Window` and a `MouseButton`): `mouse_down`, `mouse_up`, `mouse_pressed`, `mouse_released` — same semantics as key equivalents.

Mouse position functions (take `w: &Window`):

| Function | Return | Description |
|----------|--------|-------------|
| `mouse_x(w)` | `f64` | cursor X in window pixels |
| `mouse_y(w)` | `f64` | cursor Y in window pixels |
| `mouse_delta_x(w)` | `f64` | X movement since last frame |
| `mouse_delta_y(w)` | `f64` | Y movement since last frame |
| `mouse_wheel_x(w)` | `f32` | horizontal scroll delta this frame |
| `mouse_wheel_y(w)` | `f32` | vertical scroll delta this frame |

**`Key` variants:** `Unknown`, `A`–`Z`, `Num0`–`Num9`, `Space`, `Enter`, `Tab`, `Backspace`, `Escape`, `Left`, `Right`, `Up`, `Down`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`, modifier keys (`LeftShift`, `RightShift`, `LeftControl`, `RightControl`, `LeftAlt`, `RightAlt`, `LeftSuper`, `RightSuper`), punctuation, `F1`–`F12`, keypad keys (`Kp0`–`Kp9`, `KpDecimal`, `KpDivide`, `KpMultiply`, `KpSubtract`, `KpAdd`, `KpEnter`), `Count`.

**`MouseButton` variants:** `Unknown`, `Left`, `Right`, `Middle`, `Count`.

#### `sys.time` — monotonic clock, wall clock, and sleep

```
import sys.time => time;

const start := time::Instant::now();
# ... work ...
const elapsed := start.elapsed();
const ms := elapsed.as_ms();

time::sleep(time::Duration::from_ms(16));

const wall := time::WallTime::now_utc();
const unix_ns := wall.as_unix_ns();
```

| Type | Key methods |
|------|-------------|
| `Duration` | `from_ns`, `from_us`, `from_ms`, `from_secs`; `as_ns`, `as_us`, `as_ms`, `as_secs`, `as_secs_f32` |
| `Instant` | `now()`, `elapsed() -> Duration`, `elapsed_since(other: Instant) -> Duration` |
| `WallTime` | `now_utc()`, `as_unix_ns() -> u64` |

Free functions: `sleep(d: Duration)`.

Type aliases `MaybeDuration` and `MaybeInstant` wrap `Option<Duration>` and `Option<Instant>` respectively. `Duration` and `Instant` support checked and saturating arithmetic.

#### `math.types` — vector, matrix types, and arithmetic helpers

`vec` and `mat` are builtin keyword types. `math.types` re-exports standard aliases and utility functions.

```
import math.types => m;

const pos  := m::vec2(1.0, 2.0);                    # positional constructor
const clip := m::vec4(pos.x, pos.y, 0.0, 1.0);
const col  := m::mat4(c0, c1, c2, c3);              # column-major from 4 vec4s
const transformed := mvp * m::vec4(x, y, z, 1.0);   # mat * vec -> vec
```

**Builtin vector type `vec<T, N>`:** fields `.x/.y/.z/.w` (aliases `.r/.g/.b/.a`). Operators: `vec + vec`, `vec - vec`, `vec * scalar`, `scalar * vec`. CPU maps to LLVM `<N x T>` SIMD; GPU maps to SPIR-V vector.

**Builtin matrix type `mat<T, N, M>`:** fields `.col0` through `.col3`. Operators: `mat * vec -> vec`, `mat * mat -> mat`. CPU maps to an array of column vectors; GPU maps to SPIR-V matrix.

| Alias | Expands to | Fields | Size |
|-------|------------|--------|------|
| `vec2` | `vec<f32, 2>` | `x, y` | 8 B |
| `vec3` | `vec<f32, 3>` | `x, y, z` | 12 B |
| `vec4` | `vec<f32, 4>` | `x, y, z, w` | 16 B |
| `mat4` | `mat<f32, 4, 4>` | `col0..col3: vec4` | 64 B (column-major) |

Additional utilities: `U64_MAX`, `saturating_add_u64(a, b)`, `saturating_sub_u64(a, b)`.

---

#### `sys.asset` — asset pack loading

Assets are stored in `.umpack` v2 bundles. Images (PNG/JPEG/BMP) and audio (WAV/OGG) are decoded at pack time by `ul` and stored as raw pixels/PCM. `pack.load(name)` returns decompressed bytes directly — zero runtime decode cost.

By default, `uc` embeds the `.umpack` directly into the executable. Use `Pack::embedded()` to load it with no file I/O:

```
import sys.asset;

# load from the embedded asset pack (recommended)
var pack := asset::Pack::embedded();

# or load from a file (for mods, hot-reloading, --no-embed-assets)
# var pack := asset::Pack::init("assets.umpack");

# load decoded RGBA8 pixels directly
const pixels := pack.load("texture.png");
const meta := pack.image_meta("texture.png");  # ImageMeta { width, height }

# load decoded float32 stereo PCM directly
const pcm := pack.load("sound.wav");
const ameta := pack.audio_meta("sound.wav");  # AudioMeta { frame_count, channels, sample_rate }

# load a shader by @shader_ref
const pipeline := assets::pipeline_create<MyShader>(dev, pack);

pack.cleanup();  # frees decompressed buffers (embedded data itself is never freed)
```

`Pack::embedded()` returns a null pack (handle = 0) if no assets were embedded at compile time.

---

#### `sys.audio` — audio playback

Two-phase API: build a graph, compile it, create a context, play clips.

```
import sys.audio;

# default graph: music/sfx/ui → master (with limiter)
var graph := audio::AudioGraph::default();
const sfx := graph.bus_by_name("sfx");

var ctx := audio::AudioContext::create(graph,
    audio::AudioContextDesc { sample_rate = 48000, block_frames = 512, max_voices = 16 });

# play decoded PCM from the asset pack
const voice := ctx.play_clip_raw(@as(pcm.ptr, u64), ameta.frame_count, sfx, 0.0);

# control
ctx.set_voice_gain_db(voice, -6.0);
ctx.set_bus_gain_db(sfx, -3.0);
ctx.stop_voice(voice);

ctx.destroy();
graph.destroy();
```

Custom graphs: `AudioGraphBuilder::create()` → `create_bus` → `create_route` → `add_limiter`/`add_lowpass`/`add_compressor` → `set_output_bus` → `audio::compile_graph(builder)`.

---

#### `sys.gfx` — graphics API

All GPU state is hidden behind opaque `u64` handles. Import `sys.gfx` for the full API; submodules are re-exported.

**Typical per-frame sequence:**
```
import sys.gfx => gfx;
import sys.window => window;

var cfg := gfx::GfxConfig::default();
cfg.enable_validation = true;
cfg.enable_depth = true;

var win := window::Window::create("My App", 1280, 720);
const dev := gfx::init(win.handle, cfg);

# @shader_ref(TypeName) computes fnv1a64("TypeName.umsh") at compile time
const sh   := gfx::Shader { id = @shader_ref(SpriteShader) };
const pipe := gfx::pipeline_create(dev, sh);

const tex  := gfx::texture2d_from_rgba8(dev, 64, 64, rgba_bytes);
const samp := gfx::sampler_linear(dev);

for (; !win.should_close();) {
    window::Window::poll_events();
    const cmd := gfx::begin_frame(dev);

    # write per-draw constants into the frame arena
    const fa := gfx::frame_alloc(dev, @size_of(SpriteData), 256);
    # cast fa.ptr to &mut SpriteData and write fields ...

    var ds := gfx::DrawStream::begin(dev, cmd, pipe);
    ds.push_sprite(tex, samp, fa.offset, 1, 0);
    ds.submit();

    gfx::end_frame(dev, cmd);
}

gfx::pipeline_destroy(dev, pipe);
gfx::texture_destroy(dev, tex);
gfx::sampler_destroy(dev, samp);
gfx::shutdown(dev);
```

**`gfx` module functions:**

| Function | Description |
|----------|-------------|
| `init(window_handle: u64, cfg: GfxConfig) -> Device` | create the graphics device and swapchain |
| `shutdown(dev: Device)` | destroy all graphics resources |
| `begin_frame(dev: Device) -> Cmd` | acquire swapchain image; returns null Cmd on resize (skip frame) |
| `end_frame(dev: Device, cmd: Cmd)` | submit and present |
| `frame_alloc(dev: Device, size: u64, align: u64) -> FrameAlloc` | sub-allocate from the per-frame upload arena |
| `pipeline_create(dev: Device, sh: Shader) -> Pipeline` | load `.umsh` asset and compile a raster pipeline |
| `pipeline_destroy(dev: Device, pipe: Pipeline)` | destroy pipeline (caller ensures not in-flight) |
| `texture2d_from_rgba8(dev: Device, w: u32, h: u32, bytes: []u8) -> Texture2d` | synchronous RGBA8 upload |
| `texture_destroy(dev: Device, tex: Texture2d)` | deferred destroy (safe while in-flight) |
| `sampler_linear(dev: Device) -> Sampler` | bilinear clamp-to-edge sampler |
| `sampler_destroy(dev: Device, samp: Sampler)` | deferred destroy |

**`GfxConfig` fields:**

| Field | Type | Notes |
|-------|------|-------|
| `frames_in_flight` | `u32` | simultaneous GPU frames; [1, 3]; 2 recommended |
| `max_textures` | `u32` | bindless texture array capacity; ≤ 4096 |
| `max_samplers` | `u32` | bindless sampler array capacity; ≤ 256 |
| `frame_arena_bytes` | `u64` | per-frame transient upload arena; multiple of 256 |
| `draw_packets_max` | `u32` | max `DrawPacket`s per submit call |
| `enable_depth` | `bool` | creates a D32_SFLOAT depth buffer; enables depth test/write with LESS compare |
| `enable_validation` | `bool` | enables GPU validation layers; disable in release |
| `present_mode` | `PresentMode` | `Fifo` / `Immediate` / `Mailbox`; runtime falls back to Fifo |

**`DrawStream` methods (`sys.gfx.draw`):**

| Method | Description |
|--------|-------------|
| `DrawStream::create(dev, max_packets: u64) -> DrawStream` | allocate a reusable draw stream with the given capacity |
| `DrawStream::begin(dev, cmd, pipe) -> DrawStream` | open a draw stream for the current frame |
| `push_sprite(tex, samp, draw_data_offset, instance_count, first_instance)` | append a textured procedural quad (6 non-indexed vertices) |
| `push_draw_packet(packet: DrawPacket)` | append a fully-specified draw packet verbatim |
| `reset()` | clear accumulated packets for reuse between frames without reallocating |
| `submit()` | upload all accumulated packets and record draw calls |

**`FrameAlloc` fields:**

| Field | Type | Description |
|-------|------|-------------|
| `offset` | `u32` | byte offset into `frame_arena_ssbo` (set=0 binding=2); use as `DrawPacket.draw_data_offset` |
| `ptr` | `u64` | opaque CPU write pointer; cast to `&mut T` before writing; valid until the frame slot recycles |

**Bindless descriptor layout** (fixed; matches GPU shader bindings):

| Set | Binding | Array | Contents |
|-----|---------|-------|----------|
| 0 | 0 | `textures_2d[]` | all uploaded 2D textures; indexed by lower 32 bits of `Texture2d.handle` |
| 0 | 1 | `samplers[]` | all samplers; indexed by lower 32 bits of `Sampler.handle` |
| 0 | 2 | `frame_arena_ssbo` | per-frame transient data; indexed by `draw_data_offset` |
| 0 | 3 | `draw_packets_ssbo` | `DrawPacket[]`; indexed by `gl_DrawID` in the vertex shader |

---

### Examples

Example programs live in `examples/` and are built via `ninja build_examples`. CMake tracks `.um` source files and asset directories — touching a source or asset triggers a rebuild.

| Example | Description |
|---------|-------------|
| `examples/triangle/` | procedural triangle with solid color |
| `examples/cube/` | 3D spinning cube with storage buffer API, `mat * vec`, depth buffer |
| `examples/interactive_cube/` | WASD + mouse camera with delta-time |
| `examples/sine_wave/` | audio: procedural 440 Hz sine wave via `sys.audio` |
| `examples/textured_quad/` | textured quad loaded from `.umpack` asset pipeline |
| `examples/sound_player/` | audio: play a `.wav` file from the asset pack |

---

### Asset Linker (`ul`)

`ul` bundles assets into `.umpack` v2 archives. Image files (`.png`/`.jpg`/`.jpeg`/`.bmp`) are decoded to RGBA8 at pack time via stb_image. Audio files (`.wav`/`.ogg`) are decoded to float32 stereo PCM via miniaudio. All other files (`.umsh`, etc.) are stored as raw bytes. LZ4 compression is applied with `--compress`.

```sh
# pack shaders + assets together (manual invocation)
ul --pack build/assets.umpack --compress \
    build/MyShader.umsh assets/texture.png assets/sound.wav

# the uc driver invokes ul automatically when --shader-out is given:
uc main.um --root std --shader-out build/ --asset-dir assets/ -o build/game
# this compiles shaders to .umsh, packs them + asset files into assets.umpack,
# then embeds the pack into the binary. use --no-embed-assets to keep it external.
```

**Output formats:**

The shader pipeline runs inside `uc` (not `ul`) via `shader_compile()` in `compiler/dsl/`:

1. **`lower_to_mlir()`** — BodyIR for each `@stage` / `@shader_fn` → `um.shader` MLIR dialect ops (high-level: `load_input`, `store_output`, `draw_id`, `vertex_id`, `sample`, etc.)
2. **`run_spirv_lower()`** — `um.shader` → MLIR `spirv` dialect via `OpConversionPattern`s; IO variables and builtins are declared as `spirv.GlobalVariable` with `built_in` / `Location` decorations
3. **`emit_spirv_binaries()`** — serialize each `spirv.module` → `<ShaderName>.vert.spv` / `.frag.spv`
4. **`emit_umrf()`** — read `@vs_in` pod field layout from `LoadedModule` TypeAst → `<ShaderName>.umrf`

`ul --shader-pack <name>` bundles pre-built `.spv` + `.umrf` files into a `.umshader` asset. `ul --pack <out.umpack>` bundles arbitrary files into a `.umpack` archive.

**`.umpack` format:** little-endian; magic `0x554D504B` ("UMPK"), version 1. 16-byte header (magic, version, endian sentinel `0x1234`, flags, entry count) followed by a manifest of variable-length entries (u32 name length, name bytes, u64 data offset, u32 compressed length, u32 original length) and then the data region. `UMPACK_FLAG_COMPRESSED` is set in the header when LZ4 compression was requested; `compressed_len == original_len` per entry means that entry is stored raw (compression did not reduce its size).

---

## Keywords

`fn` `const` `var` `mut` `if` `else` `for` `return` `struct` `enum` `type` `impl` `import` `true` `false` `as` `self` `vec` `mat`

`pub` and `extern` are **not** keywords — they are `@pub` and `@extern` intrinsic annotations.
