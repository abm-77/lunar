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
ninja -C build/_b_umbral sema_tests lexer_tests parser_tests
./build/_b_umbral/sema_tests
./build/_b_umbral/lexer_tests
./build/_b_umbral/parser_tests
```

**Run lit functional tests** (requires `uc` to be built):
```sh
ninja -C build/_b_umbral check-lit
```

---

## Umbral Language Reference

Comments use `#`. All module-level items end with an optional `;`. Statements inside function bodies require `;` except blocks.

All declarations (functions, types, globals) are unified `const`/`var` bindings. Function literals and struct types are first-class expressions.

### Module Items

```
module_item := ['pub'] 'import' path ['as' ident] ';'
             | ['pub'] ('const' | 'var') ident ['<' generic_params '>']
                   (':=' expr | [':' type] ['=' expr]) [';']
             | 'impl' ident ['<' ident (',' ident)* '>'] '{' impl_method* '}'

path          := ident ('.' ident)*
generic_params := ident ':' ('type' | type) (',' ident ':' ('type' | type))*
impl_method   := ['pub'] ('const' | 'var') ident ':=' expr [';']
```

```
# import with optional alias
import std.io;
import std.math as math;

# named struct type
const Point := struct {
    x: i32,
    y: i32,
}

# function
const add := fn(a: i32, b: i32) -> i32 {
    return a + b;
}

# public function
pub const greet := fn() -> void {}

# function type alias — FnType expression (bare param types, no body)
const BinaryOp := fn(i32, i32) -> i32;

# var with explicit type and no initializer
var counter: i32;

# const with explicit type and initializer
const max_size: i32 = 1024;
```

### Types

```
type := '&' ['mut'] type                        # reference
      | '[' (int | ident) ']' type              # array: [N]T
      | '(' type (',' type)+ ')'                # tuple
      | 'fn' '(' (type (',' type)*)? ')' '->' type   # function type
      | ident ['<' type (',' type)* '>']        # named type, optional generic args
```

```
const a: i32;                       # named type
const b: &i32;                      # immutable reference
const c: &mut i32;                  # mutable reference
const d: (i32, i32);                # tuple type
const e: fn(i32, i32) -> i32;       # function type
const f: [10]i32;                   # array of 10 i32s
const g: [N]T;                      # array with const-generic count
const h: Option<i32>;               # generic named type
const k: Array<i32, 10>;            # generic type with const-generic arg
```

### Generics

Type parameters are declared with `Name: type`. Const-generic parameters use `Name: IntType`.

```
# Generic struct
const Option<T: type> := struct {
    has_value: bool,
    value: T,
}

# Multiple type and const-generic params
const Array<T: type, N: u32> := struct {
    data: [N]T,
    count: u32,
}

# Generic function
const identity<T: type> := fn(x: T) -> T {
    return x;
}
```

### impl Blocks

Methods are defined in `impl` blocks. Instance methods take `&self` or `&mut self` as the first parameter. Static methods do not. All methods can be marked `pub`.

```
impl Option<T> {
    pub const create := fn(v: T) -> Option<T> {
        return Option<T> { has_value = true, value = v };
    }

    pub const nullopt := fn() -> Option<T> {
        return Option<T> { has_value = false };
    }
}

impl Array<T, N> {
    pub const create := fn() -> Array<T, N> {
        return Array<T, N> { data = [N]T{}, count = 0 };
    }

    pub const push := fn(&mut self, v: T) -> void {
        if (self.count >= N) return;
        self.data[self.count] = v;
        self.count += 1;
    }

    pub const get := fn(&self, idx: u32) -> Option<T> {
        if (idx >= self.count) return Option<T>::nullopt();
        return Option<T>::create(self.data[idx]);
    }

    pub const get_count := fn(&self) -> u32 {
        return self.count;
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

# for — all three parts optional
for (var i := 0; i < 10; i += 1) {}   # classic counted loop
for (; done == false;) {}              # condition only
for (;;) {}                            # infinite loop
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
         | string_lit
         | 'true' | 'false'
         | '(' ')'                                                    # unit tuple
         | '(' expr ',' expr (',' expr)* ')'                          # tuple literal
         | '(' expr ')'                                               # grouped expr
         | ident ['<' type (',' type)* '>'] '{' (ident '=' expr ','?)* '}'  # struct init
         | ident ['<' type (',' type)* '>'] '::' ident (...)          # path expression
         | ident                                                       # identifier
         | '[' (int | ident) ']' type '{' (expr ','?)* '}'           # array literal
         | 'struct' '{' (ident ':' type ','?)* '}'                   # StructType
         | 'struct' '{' (ident ':' type '=' expr ','?)* '}'          # StructExpr
         | 'fn' '(' (ident ':' type ','?)* ')' '->' type block       # FnLit
         | 'fn' '(' (type ','?)* ')' '->' type                       # FnType
         | 'enum' '{' (ident ','?)* '}'                              # EnumType
```

**Disambiguation:**
- After `fn(`, if the first param is `ident :` (or `&self` / `&mut self`) → FnLit with body. Otherwise → FnType.
- After `struct {`, if the first field has `= expr` after its type → StructExpr. Otherwise → StructType.
- After `Ident <`, if `>` is followed by `{` or `::` → generic type args. Otherwise → binary `<`.

```
# struct initializer — fields use '='
const p := Point { x = 10, y = 20 };
const empty := Foo {};

# generic struct initializer
const opt := Option<i32> { has_value = true, value = 42 };

# path expressions
const v := Color::Red;                       # enum variant
const q := Quad::max(a, b);                  # static method
const arr := Array<i32, 10>::create();       # static method with type args
const elem := Option<i32>::create(5);        # static method with type args

# array literals
const buf := [3]i32{ 1, 2, 3 };   # with values
const zero := [10]i32{};           # zero-initialized
const gen := [N]T{};               # const-generic count

# logical operators
if (x > 0 && y > 0) {}
if (done || failed) {}

# StructType expression
const Point := struct { x: i32, y: i32 };

# StructExpr — anonymous struct value
const player := struct { x: i32 = 0, y: i32 = 0 };

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

### Imports and Cross-Module Access

```
import game.ecs.world;         # alias = last segment: world
import game.ecs.world as w;    # explicit alias

const e := world::spawn(1);    # call exported function
```

---

## Keywords

`fn` `const` `var` `mut` `if` `else` `for` `return` `struct` `enum` `type` `impl` `import` `pub` `true` `false` `as` `self`
