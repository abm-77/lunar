# TODO
- [ ] add gfx runtime support
  - [ ] open windows
  - [ ] create shaders
  - [ ] draw to screen 
- [ ] add audio runtime support
  - [ ] play sounds 
  - [ ] mix audio 
- [ ] add file i/o runtime support
  - [ ] read files
  - [ ] write files
  - [ ] more print variants (sprintf, fprintf, etc);
- [ ] builtin @asset management
  - [ ] @asset is a builtin that embeds an asset within 
        the produced executable
  - [ ] we need to create a pseudo file system for asset management
  - [ ] compression ?
  - [ ] asset atlas
  - [ ] handle tracking
- [ ] get gamepad input

03/23/26
- [x] add input runtime support
  - [x] get keyboard input 
  - [x] get mouse input
- [x] memory intrinsics
   - [x] @memmov
   - [x] @memcpy
   - [x] @memset
   - [x] @memcmp

03/22/26
- [x] flesh out meta/
  - [x] lowers high-level intrinsics: 
    - [x] @gen
      A @gen is an annotation applied to a construct to enable compile-time metaprogramming.
      @gen can be applied to a type declaration to enable the production of a different type based on compile conditions.
      @gen can be applied to impl to access compile-time generation inside the impl block
    - [x] @for_fields(struct)
      Can only appear in an @gen block. iterates over fields of struct
    - [x] @field(s, f)
      access field f of struct s
    - [x] @if, @else_if, @else
    - [x] @assert
- [x] remove metapgramming stuff from sema and put it in its own module
- [x] optimize slab allocator
  - [x] implement slab cache
    - [x] keep up to MAX_CACHED_SLABS slabs cached (munmap the others)
    - [x] use madvise(DONTNEED) to drop physical pages but keep mapping
- [x] accept initializing an array from a slice []T{...}, the array owns the data 
- [x] allow implicit conversion of array to slice
      this enables:
      for (var it := array) {}
      for (var it := slice) {}
- [x] use Error in driver and loader code
- [x] add anonymous struct syntax:
    var anon := struct {
        x: i32,
        y: i32,
    }{
          x = 10,
          y = 13,
    };

03/11/26
  - [x] optimize memory allocator
    - [x] implement slab allocator 

03/10/26
- [x] implement memory allocator
  - [x] very simple std lib makes tracked allocations
  - [x] runtime handles actual allocation and implements very simple GPA

- [x] handle low-level intrinsics 
    - [x] @bitcast
    - [x] @as
    - [x] @size_of, @align_of, @offset_of
  
03/09/26
- [x] emit executable files
- [x] get fmt::println working

03/08/26
- [x] make helper for getting fresh type lowerer
- [x] add pub to globals
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
