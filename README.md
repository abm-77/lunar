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

Comments use `#`. All statements are semicolon-terminated except blocks.

### Module items

```
module_item := 'module' path ';'
             | ['pub'] 'import' path ['as' ident] ';'
             | ['pub'] 'type' ident '=' 'struct' '{' (ident ':' type ','?)* '}'
             | ['pub'] fn_decl
             | ['pub'] 'impl' ident '{' (['pub'] fn_decl)* '}'

fn_decl := 'fn' ident '(' (ident ':' type (',' ident ':' type)*)? ')' '->' type block

path := ident ('.' ident)*
```

```
# module declaration — required at the top of every file
module myapp;

# import with optional alias
import std.io;
import std.math as math;

# named struct type
type Point = struct {
    x: i32,
    y: i32,
}

# function
fn add(a: i32, b: i32) -> i32 {
    return a + b;
}

# public function
pub fn greet() -> void {}

# impl block — methods on a type
impl Point {
    fn length(self: &Point) -> i32 {
        return self.x + self.y;
    }

    pub fn origin() -> Point {
        return Point { x: 0, y: 0 };
    }
}
```

### Types

```
type := '&' ['mut'] type               # reference
      | '(' type (',' type)+ ')'       # tuple
      | 'fn' '(' (type (',' type)*)? ')' '->' type  # function type
      | ident ('::' ident)*            # named type
```

```
let a: i32;                    # named type
let b: &i32;                   # immutable reference
let c: &mut i32;               # mutable reference
let d: (i32, i32);             # tuple type
let e: fn(i32, i32) -> i32;    # function type
```

### Statements

```
stmt := 'let' ident [':' type] ['=' expr] ';'
      | 'var' ident [':' type] ['=' expr] ';'
      | 'return' [expr] ';'
      | 'if' '(' expr ')' block ('else' ('if' '(' expr ')' block | block))*
      | 'for' '(' [for_part] ';' [expr] ';' [for_part] ')' block
      | block
      | expr (assign_op expr)? ';'

for_part   := expr | expr assign_op expr
assign_op  := '=' | '+=' | '-=' | '*=' | '/='
```

```
# immutable binding — no reassignment
let x = 10;
let y: i32 = 20;
let z: i32;          # no initializer

# mutable binding
var counter = 0;
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
    let tmp = x + y;
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
         | '(' ')'                                    # unit tuple
         | '(' expr ',' expr (',' expr)* ')'          # tuple literal
         | '(' expr ')'                               # grouped expr
         | ident '{' (ident ':' expr ','?)* '}'       # struct initializer
         | ident                                      # identifier
         | 'struct' '{' (ident ':' type '=' expr ','?)* '}'  # struct expr
         | 'fn' '(' params ')' '->' type block        # lambda
```

```
# literals
let n = 42;
let s = "hello";
let b = true;

# arithmetic and comparison
let sum = a + b * 2;       # precedence: * before +
let ok = x >= 0 == true;

# unary
let neg = -x;
let inv = !flag;

# dereference and address-of
let val = *ptr;
let r = &x;
let rm = &mut x;

# field access, indexing, call
let len = point.x;
let elem = arr[i];
let result = add(1, 2);
let chained = obj.method()(0).field;

# unit and tuple literals
let unit = ();
let pair = (1, 2);
let triple = (a, b, c);

# struct initializer — construct a named type
let p = Point { x: 10, y: 20 };
let empty = Foo {};

# struct expr — anonymous struct value with inline types
let player = struct { x: i32 = 0, y: i32 = 0 };

# lambda — captures free variables from enclosing scope
let adder = fn (a: i32) -> i32 { return a + offset; };
let noop = fn () -> void {};
```
