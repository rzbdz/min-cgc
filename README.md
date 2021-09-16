# min-cgc
A simple and NOT-fast **conservative** garbage collector (merely a **toy** for learning basic garbage collection concepts) using **mark-and-sweep** for C programing language. Provide interface of malloc/free and garbage collection without side effect.

The cgc_malloc, cgc_free, cgc_realloc package is tested using CSAPP3e [malloclab](http://csapp.cs.cmu.edu/3e/labs.html) mdriver to test. The gc package is tested using some novice codes in the `cgc.c` source file.

## algorithm:

- `cgc_malloc()` basically use ordinary **segregated explicit free lists** to manage heap memory allocated by calling `sbrk()` (might be switched to `mmap()` in the future).

- the gc algorithm is simple **mark-and-sweep** (STW), if possible, could be implemented in incremental way.


## interfaces:
- `int cgc_init()` 
explicitly delegate memory management to min-cgc.

- `int cgc(void)`
stop the world and do gc.

- `void* cgc_malloc(size_t size)`
similar to malloc() in unix.

- `void cgc_free(void *ptr)` 
similar to free() in unix.

- `void *realloc(void *ptr, size_t size)`

## performance
- The `cgc_malloc`, `cgc_free`, `cgc_realloc` test results using CSAPP3e mdriver in malloclab:
    |trace | valid | util  |   ops |     secs | Kops|
    |------|-------|-------|-------|----------|-----|
    | 0    |   yes |  98%  |  5694 | 0.000721 | 7902|
    | 1    |   yes |  94%  |  5848 | 0.000940 | 6221|
    | 2    |   yes |  96%  |  6648 | 0.001482 | 4486|
    | 3    |   yes |  95%  |  5380 | 0.001758 | 3060|
    | 4    |   yes |  94%  | 14400 | 0.000429 |33535|
    | 5    |   yes |  94%  |  4800 | 0.000927 | 5180|
    | 6    |   yes |  93%  |  4800 | 0.000799 | 6008|
    | 7    |   yes |  94%  | 12000 | 0.001891 | 6348|
    | 8    |   yes |  81%  | 24000 | 0.002075 |11567|
    | 9    |   yes |  99%  | 14401 | 0.000365 |39433|
    |10    |   yes |  99%  | 14401 | 0.000211 |68348|
    |Total |  pass |  94%  |112372 | 0.011597 | 9690|
- The garbage collection performance:. (none, it's very slow)
- test cgc calls with gc most the time and free in `test_many_objects`(@see src code): 
    |throughput|util|
    |----------|----|
    |1106 Kops|88.3%|
- test frequent `cgc` calls with `test_many_objects`(@see src code) keep 20,000 objects alive: 
    |throughput|util|
    |----------|----|
    |5 Kops|68.3%|

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
