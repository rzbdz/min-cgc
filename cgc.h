#ifndef _MIN_CGC
#define _MIN_CGC

int cgc_init();

void* cgc_malloc(size_t size);

void cgc_free(void* ptr);

void* cgc_realloc(void* ptr, size_t size);

int cgc(void);

#endif