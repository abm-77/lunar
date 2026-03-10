# TODO

03/09/26
- [x] emit executable files
- [] get fmt::println working
- [] accept initializing an array from a slice []T{...}, the array owns the data 
- [] allow implicit conversion of array to slice
      this enables:
      for (var it := array) {}
      for (var it := slice) {}
- [] use Error in driver and loader code
- [] add anonymous struct syntax:
    var anon := struct {
        x: i32,
        y: i32,
    }{
          x = 10,
          y = 13,
    };
- [] add metaprogramming pass in meta/
  - [] lowers high-level intrinsics: 
    - [] @gen
      A @gen is an annotation applied to a construct to enable compile-time metaprogramming.
      @gen can be applied to a type declaration to enable the production of a different type based on compile conditions.
      @gen can be applied to impl to access compile-time generation inside the impl block
    - [] @for_fields(struct)
      Can only appear in an @gen block. iterates over fields of struct
    - [] @field(s, f)
      access field f of struct s
    - [] @if, @else_if, @else
    - [] @assert
    - [] @println(fmt: []u8, ...) -> sys.io.fmt::println(fmt: []u8, []sys.io.fmt::Arg)
  - [] desugar:
    - [] handle obj.method(...) -> Obj::method(&obj) desugar
- [] handle low-level intrinsics 
    - [] @bitcast
    - [] @as
    - [] @unreachable
    - [] @size_of, @align_of, @offset_of
  
03/08/26
- [x] make helper for getting fresh type lowerer
- [] add pub to globals
- [x] codegen

03/07/26
- [x] implement multi-module 
- [x] implement semantic analysis on AST
- [x] implement generics 

03/04/26
- [x] implement unified error/result type
- [x] add source locations to error messages
- [x] handle parsing generics
- [x] handle parsing character, hex, and float literals
