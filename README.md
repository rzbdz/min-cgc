# min-cgc
A simple and fast conservative garbage collector using mark and sweep for C programing language. Provide interface of malloc/free and garbage collection without side effect.

## interfaces:
- `int cgc_init()` 
explicitly delegate memory management to min-cgc.

- `void* cgc_malloc(size_t size)`
similar to malloc() in unix.

- `void cgc_free(void *ptr)` 
similar to free() in unix.

- `void *realloc(void *ptr, size_t size)`

## algorithm:

- `cgc_malloc()` basically use ordinary explicit free lists to manage heap memory calling `sbrk()` (might be switched to `mmap()` in the future).

- the gc algorithm is simple mark-and-sweep (STW), if possible, could be implemented in incremental way.

## naming style:

- functions are named in snake case e.g. `cgc_init`.
- structures are named in pascal case e.g. `Header`.
- malloc are using snake upper case e.g. `SIZE`.

## git commit log prefix:

- feat
- fix
- docs
- style
- refactor
- test
