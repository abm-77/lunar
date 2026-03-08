# Lunar

## What is Lunar?
`lunar` is a framework for making graphical applications

## Umbral
`umbral` is the language used by `lunar`

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
ninja -C build/_b_umbral interner_tests lexer_tests parser_tests
./build/_b_umbral/interner_tests
./build/_b_umbral/lexer_tests
./build/_b_umbral/parser_tests
```

**Run lit functional tests** (requires `uc` to be built):
```sh
ninja -C build/_b_umbral check-lit
```

## Umbral Language Grammar

Comments use `#`. All module-level items end with an optional `;`. Statements inside function bodies require `;` except blocks.

All declarations (functions, types, globals) are unified `const`/`var` bindings.
Function literals and struct types are first-class expressions.

### Module items

```
module_item := 'module' path ';'
             | ['pub'] 'import' path ['as' ident] ';'
             | ['pub'] ('const' | 'var') ident (':=' expr | [':' type] ['=' expr]) [';']

path := ident ('.' ident)*
```

```
# module declaration — required at the top of every file
module myapp;

# import with optional alias
import std.io;
import std.math as math;

# named struct type — const binding holding a StructType expression
const Point := struct {
    x: i32,
    y: i32,
}

# function — const binding holding a FnLit expression
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
type := '&' ['mut'] type               # reference
      | '(' type (',' type)+ ')'       # tuple
      | 'fn' '(' (type (',' type)*)? ')' '->' type  # function type
      | ident ('::' ident)*            # named type
```

```
const a: i32;                    # named type
const b: &i32;                   # immutable reference
const c: &mut i32;               # mutable reference
const d: (i32, i32);             # tuple type
const e: fn(i32, i32) -> i32;    # function type
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

for_part   := expr | expr assign_op expr
assign_op  := '=' | '+=' | '-=' | '*=' | '/='
```

```
# immutable binding — inferred type with :=
const x := 10;
const y: i32 = 20;   # explicit type
const z: i32;        # explicit type, no initializer

# mutable binding
var counter := 0;
var name: i32;

# assignment and compound assignment
counter = 1;
counter += 2;
counter -= 1;
counter *= 3;
counter /= 2;

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

# for — all three parts are optional
for (var i = 0; i < 10; i += 1) {}   # classic counted loop
for (; done == false;) {}             # condition only
for (;;) {}                           # infinite loop

# bare block
{
    const tmp := x + y;
}

# expression statement
foo(x, y);
```

### Expressions

Operator precedence (low to high):

| Operators             | Precedence |
|-----------------------|------------|
| `==`  `!=`            | 40         |
| `<` `<=` `>` `>=`     | 50         |
| `+`  `-`              | 60         |
| `*`  `/`              | 70         |
| postfix `.` `[]` `()` | 90         |

```
expr    := unary (infix_op unary)*
unary   := '-' expr | '!' expr | '*' expr | '&' ['mut'] expr | postfix
postfix := primary ('.' ident | '[' expr ']' | '(' args ')')*

primary := int_lit
         | string_lit
         | 'true' | 'false'
         | '(' ')'                                           # unit tuple
         | '(' expr ',' expr (',' expr)* ')'                # tuple literal
         | '(' expr ')'                                      # grouped expr
         | ident '{' (ident ':' expr ','?)* '}'              # struct initializer
         | ident                                             # identifier
         | 'struct' '{' (ident ':' type ','?)* '}'           # StructType expression
         | 'struct' '{' (ident ':' type '=' expr ','?)* '}'  # StructExpr (has =)
         | 'fn' '(' (ident ':' type ','?)* ')' '->' type block  # FnLit (named params)
         | 'fn' '(' (type ','?)* ')' '->' type               # FnType (bare types, no body)
```

Disambiguation: after `fn(`, if the first param has the form `ident :` it's a FnLit (has a body block). Otherwise it's a FnType (no body). For `struct {`, if the first field has `=` after its type it's a StructExpr; otherwise a StructType. An empty `struct {}` is always a StructType.

```
# literals
const n := 42;
const s := "hello";
const b := true;

# arithmetic and comparison
const sum := a + b * 2;       # precedence: * before +
const ok := x >= 0 == true;

# unary
const neg := -x;
const inv := !flag;

# dereference and address-of
const val := *ptr;
const r := &x;
const rm := &mut x;

# field access, indexing, call
const len := point.x;
const elem := arr[i];
const result := add(1, 2);
const chained := obj.method()(0).field;

# unit and tuple literals
const unit := ();
const pair := (1, 2);
const triple := (a, b, c);

# struct initializer — construct a named type
const p := Point { x: 10, y: 20 };
const empty := Foo {};

# StructType expression — struct type with field names and types (no values)
const Point := struct { x: i32, y: i32 };

# StructExpr — anonymous struct value with inline types and values
const player := struct { x: i32 = 0, y: i32 = 0 };

# FnLit — function with named params and a body block
const adder := fn (a: i32) -> i32 { return a + offset; };
const noop := fn () -> void {};

# FnType — function type expression (no body, bare param types)
const BinaryOp := fn(i32, i32) -> i32;
var callback: fn(i32) -> void;
```
