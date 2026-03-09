# TODO

03/08/26
- [x] make helper for getting fresh type lowerer
- [] add pub to globals
- [x] codegen
- [] add a helper for converin u32 ls = ..., n = ... std::vector...
- [] get fmt::println working
- [] add metaprogramming pass in meta/
  - [] lowers intrinsics: @gen, @for_fields, @if, etc
  - [] handles println desugar
      @println(fmt: []u8, ...) -> sys.io.fmt::println(fmt: []u8, []sys.io.fmt::Arg)
  

03/07/26
- [x] implement multi-module 
- [x] implement semantic analysis on AST
- [x] implement generics 


03/04/26
- [x] implement unified error/result type
- [x] add source locations to error messages
- [x] handle parsing generics
- [x] handle parsing character, hex, and float literals
