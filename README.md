# Lunar

## What is Lunar?
`lunar` is a framework for making graphical applications.

## Umbral
`umbral` is the language used by `lunar`. Source files use the `.um` extension; the compiler binary is `uc`.

## Building

**Prerequisites:** CMake 3.22+, Ninja, Clang, Python 3, Vulkan SDK.

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
             | '@extern' ident ':' type [';']
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
@extern abs : fn(v: i32) -> i32;
@extern errno : i32;

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
assign_op  := '=' | '+=' | '-=' | '*=' | '/='
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
| `==` `!=`             | 40         |
| `<` `<=` `>` `>=`     | 50         |
| `+` `-`               | 60         |
| `*` `/`               | 70         |
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
         | ident ['<' type (',' type)* '>'] '{' (ident '=' expr ','?)* '}'  # struct init
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

```
# struct initializer — fields use '='
const p := Point { x = 10, y = 20 };
const empty := Foo {};

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
| `@extern name : type` | declare an externally-defined symbol (no body emitted) |

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

Requires a Vulkan-capable display. The produced binary is self-contained — no GLFW install needed on the target system.

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

---

## Keywords

`fn` `const` `var` `mut` `if` `else` `for` `return` `struct` `enum` `type` `impl` `import` `true` `false` `as` `self`

`pub` and `extern` are **not** keywords — they are `@pub` and `@extern` intrinsic annotations.
