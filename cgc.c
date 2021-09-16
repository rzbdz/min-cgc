/*
 * cgc.c
 * simple mark-and-sweep memory allocator
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <setjmp.h>
#include <stdint.h>
#include <time.h>
#include "cgc.h"
#include "debug.h"

/**
 * a chunk structure in x64 system,
 *    in x86 let 8 be 4.
 * 8bytes | SIZE | FLAG | Header
 * 8bytes | PAYLOAD (n) | next
 * 8bytes | PAYLOAD (p) | prev
 * ?bytes | PAYLOAD II  | rest of payload
 * 8bytes | SIZE | FLAG | Footer
 * uintptr_t is defined in C99.
 */
typedef uintptr_t field_t, *field_ptr;
typedef struct __attribute__((packed)) header_ {
  field_t field;
  struct header_* next;
  struct header_* prev;
} * HeaderPtr;

#define UINT(x) ((field_t)(x))

#define FD_SZ (sizeof(void*))
#define MIN_ALLOC (FD_SZ << 1)

#define ALLOC 0x1
#define MARK 0x2
#define ALL 0x3

#define IS(h, b) (*((field_ptr)(h)) & (b))

#define PURE_SZ(f) ((*(field_ptr)(f)) & (~ALL))
#define PAYLOAD(h) ((void*)(((field_ptr)h) + 1))

#define UNSET(f, b) ((*((field_ptr)(f))) &= ~(b))

#define CLEAR(f) UNSET(f, ALL)

#define THIS_FD(h) ((field_ptr)(UINT(h) + FD_SZ + PURE_SZ(h)))

#define NXT_HD(h) ((HeaderPtr)(UINT(THIS_FD(h)) + FD_SZ))

#define LST_HD(h) ((HeaderPtr)(UINT(LST_FD(h)) - PURE_SZ(LST_FD(h)) - FD_SZ))

#define LST_FD(h) ((field_ptr)(UINT(h) - FD_SZ))

inline static void SET_BLOCK_FL(HeaderPtr hd, field_t flag) {
  assert(!(flag & ~ALL));
  *(field_ptr)hd |= flag;
  *(field_ptr)hd |= flag;
}

inline static void UNSET_BLOCK_FL(HeaderPtr hd, field_t flag) {
  assert(!(flag & ~ALL));
  UNSET(hd, flag);
  UNSET(THIS_FD(hd), flag);
}

inline static void SET_BLOCK_SZ(HeaderPtr hd, field_t field) {
  *(field_ptr)hd = field;
  *(field_ptr)(UINT(hd) + FD_SZ + (field & (~ALL))) = field;
}

/**
 * structure tracking of all chunks separated by specific size.
 * store the first header of a sliced(like 4kb/8kb) range.
 * make sure all chunks with header (allocated or freed) be
 * reachable from the chunks array.
 * FIXME: optimize the memory overhead of data segment.
 */
typedef struct chunk_ {
  HeaderPtr first;
} Chunk;

#define CHUNK_INIT 0x100
#define CHUNK_SZ 0x1100
#define CHK_MAX 1023

#define FREE_MAX 16

#define PACK_BASE 8

#define PACK(ptr) ((UINT((ptr)) + PACK_BASE - 1) & ~(PACK_BASE - 1))

/**
 * calculate the index of segregated freelist array.
 */
#define SLOT_GRAIN ~(0x1f)
static inline size_t slot(field_t sz) {
  size_t index = 0;
  while ((index < FREE_MAX - 1) && (sz & SLOT_GRAIN)) {
    sz >>= 1;
    index++;
  }
  return index;
}

static HeaderPtr free_list[FREE_MAX];

static Chunk chunks[CHK_MAX];
static unsigned chunks_end = 0;

static field_t program_break = 0;
static field_t heap_start = 0;

static void* stack_start = NULL;
static void* stack_end = NULL;

/**
 * calculate the index of the chunks array.
 */
inline static int chunks_slot(HeaderPtr p) {
  unsigned i = (UINT(p) - heap_start) / UINT(0x2000);
  i = i >= CHK_MAX ? CHK_MAX - 1 : i;
  return i;
}
/**
 * remove a chunk from the array (if it has the lowest addr of a slot).
 * should be called when doing merging or slicing on a chunk.
 */
inline static void remove_chunks(HeaderPtr old) {
  // slicing by 4kb
  unsigned i = chunks_slot(old);
  chunks[i].first = (chunks[i].first == old) ? NULL : chunks[i].first;
}
/**
 * register a chunk if it should be.
 * should be called when adding new chunk from heap.
 */
inline static void add_chunks(HeaderPtr up) {
  // slicing by 4kb
  unsigned i = chunks_slot(up);
  HeaderPtr old = chunks[i].first;
  chunks[i].first =
      (UINT(old) >= heap_start && UINT(up) > UINT(old)) ? old : up;
  chunks_end = chunks_end > i ? chunks_end : i;
}

/**
 * search the heap to find which header manage a chunk
 * that the ptr points to.
 * using chunks array to speed up instead of searching the entire heap
 * sequentially.
 */
static HeaderPtr search_heap(void* ptr) {
  if (UINT(ptr) <= heap_start || UINT(ptr) > program_break) {
    return NULL;
  }
  unsigned i = chunks_slot(ptr);
  int j;
  void *cur = (void*)chunks[i].first, *end;
  if (!cur || ptr < cur) {
    return NULL;
  }
  // find a end for linear searching
  for (j = i + 1; j <= chunks_end && chunks[j].first == NULL; j++)
    ;
  end = (j <= chunks_end) ? (void*)(chunks[j].first) : (void*)(program_break);
  while (cur < end) {
    void* pl_start = PAYLOAD(cur);
    void* pl_end = THIS_FD(cur);
    if (ptr >= pl_start && ptr < pl_end) {
      break;
    }
    cur = NXT_HD(cur);
  }

  if (cur >= end || !IS(cur, ALLOC)) {
    return NULL;
  }
  return (HeaderPtr)cur;
}

/**
 * check the heap,
 * including chunks, freelist and the entire heap.
 * FIXME: the codes here are ugly, I will make them look better.
 */
static void heap_checker() {
  LOG("[heap checker] heap starts at: %p, program breaks at: %p, heap size in "
      "total: %p "
      "(%lu) bytes.",
      (void*)heap_start, (void*)program_break,
      (void*)(program_break - heap_start), (program_break - heap_start));
  LOG("[heap checker][chunks] checking chunks(only print 4 records at most)...")
  for (int i = 0; i < ((4 < chunks_end + 1) ? 4 : chunks_end + 1); i++) {
    LOG("[heap checker][chunks] chunk[%d] header: %p, end: %d", i,
        (void*)(chunks[i].first), chunks_end);
  }
  assert(UINT(chunks[0].first) ==
         ((heap_start == program_break) ? NULL : heap_start));
  LOG("[heap checker][chunks] chunks tests " D_GREEN(
      passed) ", all chunks are reacheable");

  LOG("[heap checker][freelist] examing the freelist structure...");
  for (int i = 0; i < FREE_MAX; i++) {
    HeaderPtr first;
    HeaderPtr cur = first = free_list[i];
    int count = 0;
    field_t size = 0;
    HeaderPtr last = cur;
    while (cur != NULL) {
      count++;
      size += PURE_SZ(cur);
      assert(!IS(cur, ALLOC));
      assert(!IS(cur, MARK));
      assert(!(IS(THIS_FD(cur), ALLOC)));
      assert(!(IS(THIS_FD(cur), MARK)));
      assert(THIS_FD(cur));
      assert(last <= cur);
      last = cur;
      if (cur->next == first) {
        break;
      }
      cur = cur->next;
    }
    if (free_list[i] != NULL)
      LOG("[heap checker][freelist][%d][%d, %d) has %d blocks,0x%lx(%lu)"
          "bytes, "
          "head: %p, head->next: %p",
          i, 2 << (i + 3), (2 << (i + 4)) - 1, count, UINT(size), size,
          free_list[i], free_list[i]->next);
  }
  LOG("[heap checker][freelist] all free blocks are correct, all "
      "tests " D_GREEN(passed) "");
  int count = 0;
  HeaderPtr cur = (HeaderPtr)heap_start;
  field_t freed = 0;
  field_t usage = 0;
  while (UINT(cur) <= program_break - 4 * FD_SZ) {
    count++;
    assert(PURE_SZ(cur) != 0);
    assert((PURE_SZ(cur) == PURE_SZ(THIS_FD(cur))));
    if (!IS(cur, ALLOC)) {
      freed += PURE_SZ(cur);
    } else {
      usage += PURE_SZ(cur);
    }
    LOG("[heap checker][heap] found 1 block : %p, "
        "0x%lx (%lu) bytes, allocated: %lu, marked: %lu",
        (void*)cur, PURE_SZ(cur), PURE_SZ(cur), IS(cur, ALLOC), IS(cur, MARK));
    cur = NXT_HD(cur);
  }
  LOG("[heap checker][heap] find %d blocks in total, 0x%lx(%lu) bytes "
      "are free, "
      "0x%lx(%lu) bytes are in use",
      count, UINT(freed), UINT(freed), UINT(usage), UINT(usage));
  LOG("[heap checker][heap] all blocks are correct, all tests " D_GREEN(
      passed) "");
}

int cgc(void);

/*the mdriver which i use to test the malloc/free/realloc
 *functions provided by cmu csapp lab use heap memory
 *to detect payload collision,
 *I simply tried add a root structure for it.
 *but it still doesn't work for the mmap based allocator.
 *keep it for user define gc roots.*/
struct gc_root {
  void* start;
  void* end;
};

struct gc_root base_root;

/*
 * cgc_init - initialize the cgc package.
 * returns 1 if there are errors
 */
int cgc_init(void) {
#if INTPTR_MAX == INT32_MAX
// These macros are basically written to return 8bytes aligned payload
// pointer for cgc_malloc() caller in x86 machine (need C99).
#define PACK_INIT(p) (PACK(UINT(p) + sizeof(field_t)) - sizeof(field_t))
#define IS_PACKED(p) ((UINT(p) + sizeof(field_t)) & (PACK_BASE - 1))
#else
#define PACK_INIT(p) PACK(p)
#define IS_PACKED(p) (UINT(p) & (PACK_BASE - 1))
#endif

  chunks_end = 0;
  stack_start = NULL;
  stack_end = NULL;

  field_t get_stack = 48;
  stack_start = (void*)&get_stack;

  heap_start = (field_t)sbrk(0);

  if (IS_PACKED(heap_start) &&
      ((UINT(sbrk(PACK_INIT(heap_start) - heap_start))) == -1)) {
    return -1;
  }

  program_break = heap_start = PACK_INIT(heap_start);

  chunks[CHK_MAX].first = NULL;

  // mdriver calls init in a program more than once
  for (int i = 0; i < FREE_MAX; i++)
    free_list[i] = NULL;

  return 0;
}

/**
 * grow a new chunk in the heap using sbrk.
 * however, for realloc who might want a small chunk to extend
 * its old chunk, grow doesn't specific the minimum chunk size
 * which should be done before calling it.
 */
static HeaderPtr grow(field_t size) {
  size += FD_SZ + FD_SZ;
  size = PACK(size);

  void* p;

  if ((p = sbrk(size)) == (void*)-1) {
    LOG("ERROR: OOM at request for 0x%lx(%lu)bytes, check the heap "
        "layout dump printed out below:",
        size, size);
    heap_checker();
    return NULL;
  }

  assert((UINT(p) == program_break));
  program_break += size;

  size -= FD_SZ + FD_SZ;
  SET_BLOCK_SZ(p, size);
  UNSET_BLOCK_FL(p, ALL);

  ((HeaderPtr)p)->next = ((HeaderPtr)p)->prev = (HeaderPtr)p;

  add_chunks(p);
  return (void*)p;
}

static void merge(HeaderPtr);

/**
 * as it's name.
 */
inline static void cut_from_list(HeaderPtr p, int s) {
  assert(!IS(p, ALLOC));
  assert(s >= 0 && s < FREE_MAX);

  if (p == free_list[s]) {
    // free_list[s] = NULL;// keep it to remind myself...
    int cond = free_list[s]->next == free_list[s];
    free_list[s] = cond ? NULL : free_list[s]->next;
  }
  p->prev->next = p->next;
  p->next->prev = p->prev;
  p->prev = p->next = p;
}
/*
 * cgc_malloc
 */
void* cgc_malloc(field_t req) {
  //   LOG("malloc : req %p", req);
  //   heap_checker();
  if (req <= 0) {
    return NULL;
  }
  req = PACK(req);
  int s;

  HeaderPtr search;
  int gc_done = 0;

  // TODO: rewrite the loop.
  // a ungly loop trying to do some 'goto' jobs.
  // it's a state machine.
  for (s = slot(req), search = free_list[s]; s <= FREE_MAX;) {
    if (s == FREE_MAX && !gc_done) {
      if (!cgc()) {
        LOG_DEBUG("gc didn't get any freed mem.");
        search = NULL;
      } else {
        LOG_DEBUG("WooHOOO! gc bring us spaces!");
        s = slot(req);
        search = free_list[s];
      }
      gc_done = 1;
      continue;
    } else if (s == FREE_MAX) {
      field_t sz = req;
      if ((program_break == heap_start) && sz < CHUNK_INIT) {
        sz = CHUNK_INIT;
      } else if (sz < CHUNK_SZ) {
        sz = CHUNK_SZ;
      }
      if ((search = grow(sz)) == NULL)
        return NULL;
      break;
    }
    if (search == NULL) {
      s++;
      search = (s == FREE_MAX) ? NULL : free_list[s];
      continue;
    }
    if (PURE_SZ(search) >= req) {
      cut_from_list(search, s);
      break;
    }
    search = search->next;
    if (search == free_list[s]) {
      s++;
      search = (s == FREE_MAX) ? NULL : free_list[s];
      continue;
    }
  }

  field_t fit_size = PURE_SZ(search);
  // here take a strategy when we do slicing.
  // if we alway return a block cut from the beginning of
  // a big chunk, it will be un-merge-able when we allocated
  // several SMALL chunks between two BIG chunks.
  // which is unacceptable when we try to allocate a
  // VERY BIG chunks next time.
  if (fit_size > req + FD_SZ * 4) {
    if (req <= 0x56) {
      SET_BLOCK_SZ(search, req | ALLOC);
      HeaderPtr rest = NXT_HD(search);
      SET_BLOCK_SZ(rest, (fit_size - req - FD_SZ * 2));
      add_chunks(rest);
      merge(rest);
    } else {
      SET_BLOCK_SZ(search, (fit_size - req - FD_SZ * 2));
      HeaderPtr rest = NXT_HD(search);
      SET_BLOCK_SZ(rest, req | ALLOC);
      add_chunks(search);
      merge(search);
      search = rest;
    }
  } else {
    SET_BLOCK_FL(search, ALLOC);
  }
  return PAYLOAD(search);
}

/**
 * insert a chunk to specific segregated freelist.
 * not dealing the flag jobs.
 */
static void insert(HeaderPtr ins, int s) {
  if (free_list[s] == NULL) {
    free_list[s] = ins->prev = ins->next = ins;
    assert(free_list[s] == ins->prev);
    assert(free_list[s] == ins->next);
    assert(free_list[s] == ins);
    return;
  }

  HeaderPtr prec = free_list[s];

  while ((prec >= ins || ins >= prec->next) && prec < prec->next) {
    prec = prec->next;
  }
  assert(prec != NULL);
  ins->next = prec->next;
  (ins->next)->prev = ins;

  prec->next = ins;
  prec->next->prev = prec;

  free_list[s] = (ins < free_list[s]) ? ins : free_list[s];
  assert(prec == ins->prev);
  assert(prec->next == ins);
}

/**
 * try to merge m with its previous and next chunk.
 * if not merge-able, we just insert it to a freelist.
 * not dealing the flag jobs.
 */
static void merge(HeaderPtr m) {
  HeaderPtr new_header = m;
  field_t new_size = PURE_SZ(m);

  field_t close_size;
  int close_slot;
  field_t close = UINT(NXT_HD(m));

  // merge the next block (if possible)
  if (close < program_break) {
    close_size = PURE_SZ(close);
    close_slot = slot(close_size);
    if (!IS(close, ALLOC)) {
      // just don't forget the update the header 'cache' array.
      remove_chunks((HeaderPtr)close);
      new_size += PURE_SZ(close) + FD_SZ * 2;
      cut_from_list((HeaderPtr)close, close_slot);
    }
  }

  // merge the last block (if possible)
  // if NXT(m) is merged, we can deal with LST|M|NXT case at the same time.
  close = UINT(LST_FD(m));
  if (close > heap_start && (close = UINT(LST_HD(m))) >= heap_start) {
    close_size = PURE_SZ(close);
    close_slot = slot(close_size);
    if (!IS(close, ALLOC)) {
      // just don't forget the update the header 'cache' array.
      remove_chunks(m);
      new_header = (HeaderPtr)close;
      new_size += close_size + FD_SZ * 2;
      cut_from_list((HeaderPtr)close, close_slot);
    }
  }

  SET_BLOCK_SZ(new_header, new_size);
  insert(new_header, slot(new_size));
}

/*
 * cgc_free
 * simply unset allocated flag and call merge.
 */
void cgc_free(void* ptr) {
  if (ptr == NULL || UINT(ptr) >= program_break || UINT(ptr) <= heap_start) {
    return;
  }
  HeaderPtr to_free = (HeaderPtr)((field_ptr)ptr - 1);
  UNSET_BLOCK_FL(to_free, ALL);

  merge(to_free);
}

/*
 * cgc_realloc
 * some strategies are performed here for contiguous 'realloc' which
 * is very difficult to bugfree (for me).
 */
void* cgc_realloc(void* src, size_t size) {
  HeaderPtr old = (HeaderPtr)((field_ptr)src - 1);

  size_t old_sz = (size_t)(PURE_SZ(old));

  // LOG("realloc : old: %p, req %p", old_sz, size);
  // heap_checker();

  size = (size + MIN_ALLOC - 1) & ~(MIN_ALLOC - 1);

  if (size <= old_sz && old_sz <= 0x30 + size) {
    return src;
  }

  HeaderPtr try = NXT_HD(old);
  HeaderPtr get = NULL;
  field_t fit_size = 0;

  // ask if we can grow directly from the old one.
  if (UINT(try) < program_break && !IS(try, ALLOC)) {
    get = try;
    fit_size = PURE_SZ(try) + (FD_SZ << 1);
    try = NXT_HD(try);
  }

  // ask if the grow is enough or we luckily reach the brk.
  if (UINT(try) == program_break && fit_size + old_sz < size) {
    HeaderPtr verify = grow(size - old_sz - fit_size);
    assert((try == verify));
    try = verify;
    fit_size += PURE_SZ(try) + (FD_SZ << 1);
  }
  // do some slicing.
  // similar to what is in cgc_malloc
  // but do slightly different from it.
  // TODO: write a function to cover these operations.
  if (fit_size + old_sz >= size) {
    if (get) {
      int s = slot(fit_size - (FD_SZ << 1));
      cut_from_list(get, s);
      remove_chunks(get);
    }
    if (fit_size + old_sz >= size + (FD_SZ << 2)) {
      SET_BLOCK_SZ(old, size | ALLOC);
      HeaderPtr rest = NXT_HD(old);
      add_chunks(rest);
      SET_BLOCK_SZ(rest, (fit_size + old_sz - size - (FD_SZ << 1)));
      merge(rest);
    } else {
      SET_BLOCK_SZ(old, (fit_size + old_sz) | ALLOC);
    }
    return src;
  }

  // sadly.
  long* dst = (long*)cgc_malloc(size);

  if (dst == NULL) {
    return NULL;
  }

  size_t i;

  // act like memcpy/memove here,
  // but our cgc_malloc always return aligned spaces.
  const long* s = src;
  for (i = 0; i < old_sz / sizeof(long); i++) {
    dst[i] = s[i];
  }
  cgc_free(src);
  return dst;
}

/**
 * query the current stack pointer
 * (but the correctness depends on how compiler manipulate the stack)
 */
static void stack_checkpoint(void) {
  unsigned long dummy;
  dummy = 48;
  stack_end = (void*)&dummy;
}
static void mark(void* ptr);

/**
 * FIXME: the assumption that every 'pointer like' variable is
 * aligned is not be proved. But using (void*)cur++ would be extremely slow.
 */
static void mark_range(void* start, void* end) {
  void* cur;
  LOG_DEBUG("start %p, end %p", start, end);
  start = PACK(start);
  for (cur = start; cur < end; cur = ((field_ptr)cur) + 1) {
    // LOG_DEBUG("mark source is %p, deref is %p", cur, *(void**)cur);
    mark(*(void**)cur);
  }
}

/**
 * mark a pointer by searching on the chunks array.
 * if not use chunks, it would be very slow to
 * find which header a pointer belongs to.
 */
static void mark(void* ptr) {
  HeaderPtr hd;
  if (!(hd = search_heap(ptr))) {
    // LOG("try mark but not in heap %p", ptr);
    return;
  }
  assert(IS(hd, ALLOC));
  if (IS(hd, MARK)) {
    // LOG_DEBUG("already marked: %p", (void*)hd);
    return;
  }
  SET_BLOCK_FL(hd, MARK);
  LOG_DEBUG("a block is marked: %p, by %p", (void*)hd, ptr);

  mark_range(PAYLOAD(hd), (void*)NXT_HD(hd));
}

/**
 * use setjmp package to get the current registers.
 * again, this is very conservative since jmp_buf contains more than regs.
 */
static void mark_register(void) {
  jmp_buf regs;
  size_t i;

  setjmp(regs);
  for (i = 0; i < sizeof(regs); i++) {
    mark(((void**)regs)[i]);
  }
}

/**
 * mark the stack (conservative) by using global variables.
 */
static void mark_stack(void) {
  stack_checkpoint();
  // P_P(stack_start)
  // P_P(stack_end);
  if (stack_start > stack_end) {
    mark_range(stack_end, stack_start);
  } else {
    mark_range(stack_start, stack_end);
  }
}

/**
 * scan every chunk from the chunks array(I just found
 * that this is meanningless) to the program_break.
 */
static void sweep_from_chunks(void) {
  unsigned i;
  HeaderPtr cur, end;
  unsigned j;
  for (i = 0; i < chunks_end; i++) {
    // find a end for linear searching
    for (j = i + 1; j < chunks_end && chunks[j].first == NULL; j++)
      ;
    end = chunks[j - 1].first ? chunks[j - 1].first : (HeaderPtr)program_break;
    for (cur = chunks[i].first; cur < end; cur = NXT_HD(cur)) {
      if (IS(cur, ALLOC)) {
        if (IS(cur, MARK)) {
          LOG_DEBUG("a block is unmarked: %p, in chunks[%d]", (void*)cur, i);
          UNSET_BLOCK_FL(cur, MARK);
        } else {
          cgc_free(PAYLOAD(cur));
        }
      }
    }
  }
}

/**
 * scan every chunk from the heap_start to the program_break.
 */
static int sweep_from_heap(void) {
  int flag = 0;
  HeaderPtr cur;
  for (cur = (HeaderPtr)heap_start; cur < (HeaderPtr)program_break;
       cur = NXT_HD(cur)) {
    if (IS(cur, ALLOC)) {
      if (IS(cur, MARK)) {
        UNSET_BLOCK_FL(cur, MARK);
      } else {
        flag = 1;
        cgc_free(PAYLOAD(cur));
      }
    }
  }
  return flag;
}

/**
 * some temporarily wrapper.
 * TODO: remove this.
 */
inline static int sweep(void) {
  return sweep_from_heap();
}

/**
 * do the gc, simply call mark then sweep.
 */
int cgc(void) {
  LOG_DEBUG("gc called!");
  if (heap_start == program_break)
    return 0;
  mark_range(base_root.start, base_root.end);
  mark_register();
  mark_stack();
  return sweep();
}

//=================================================================================================//
/*
 * test code, the code here are ugly.
 * TODO: rewrite it.
 */
//=================================================================================================//
#define TEST LOG("[testing] %s...", __FUNCTION__);
#define PASS LOG("[testing] %s " D_GREEN(passed), __FUNCTION__);
#define MAX(a, b) ((a) < (b)) ? (b) : (a)
static unsigned long test_calls_cnt = 0;
static field_t max_same_time = 0;
static void test_gc(void) {
  TEST void* p;
  p = cgc_malloc(100);
  assert(IS((((field_ptr)p) - 1), ALLOC));
  p = 0;
  cgc();
  test_calls_cnt += 2;
  max_same_time = MAX(max_same_time, 100);
  PASS
}

static void test_many_malloc(void) {
  TEST void* p;
  int i;
  for (i = 0; i < 2000; i++) {
    p = cgc_malloc(100);
  }
  assert(IS((((field_ptr)p) - 1), ALLOC));
  assert(stack_end != stack_start);
  test_calls_cnt += 2000;
  max_same_time = MAX(max_same_time, 100);
  PASS
}

static void test_integrity(void) {
  TEST char* long_string = cgc_malloc(4096);
  for (int i = 0; i < 4096; i++) {
    long_string[i] = i % 26 + 'a';
  }
  void* handle = long_string + 0x64;
  long_string = NULL;
  cgc();
  handle = (char*)handle - 0x64;
  for (int i = 0; i < 4096; i++) {
    char cmp = i % 26 + 'a';
    assert(((char*)handle)[i] == cmp);
  }
  test_calls_cnt += 2;
  max_same_time = MAX(max_same_time, 4096);
  PASS
}

typedef struct {
  int my_val;
  char name[40];
} obj;

static void test_simulate_user_call(void) {
  TEST LOG_DEBUG("size_t: %ld", sizeof(size_t));
  size_t sz = sizeof(obj);
  obj* obj_array[5];
  LOG_DEBUG("array stack location: %p", &obj_array)
  for (int i = 0; i < 5; i++) {
    LOG_DEBUG("allocate %d", i);
    obj_array[i] = (obj*)cgc_malloc(sz);
    heap_checker();
    LOG_DEBUG("array stack store heap ptr: %p", obj_array[i]);
    obj_array[i]->my_val = 9900 + i;
    char buf[40];
    for (int j = 0; j < 40; j++) {
      buf[j] = 'z' - j % 26;
    }
    for (int j = 0; j < 40; j++) {
      obj_array[i]->name[j] = buf[j];
    }
    heap_checker();
    P_P(obj_array[i]->my_val);
    LOG_DEBUG("assert obj[%d], located in %p, it's heap: %p", i, obj_array + i,
              &(obj_array[i]->my_val));
    assert_eq(obj_array[i]->my_val, 9900 + i);
    cgc();
    for (int j = 0; j < 40; j++) {
      assert_eq(obj_array[i]->name[j], buf[j]);
    }
    cgc();
    assert_eq(obj_array[i]->my_val, 9900 + i);
  }
  for (int i = 0; i < 5; i++) {
    obj_array[i] = NULL;
  }
  test_calls_cnt += 15;
  max_same_time = MAX(max_same_time, sz * 5);
  PASS
}
inline static void test_clear_regs(void) {
  jmp_buf rrs;
  // these codes here are
  // useful in some very special cases when I test the gc function
  // (where no register valueables need to be keep across a function call and
  // some useless pointers exist in regs) mostly useless.... and need
  register void* ax = 0;
  register void* bx = 0;
  register void* cx = 0;
  register void* cd = 0;
  register void* dx = 0;
  register void* ex = 0;
  register void* sp = 0;
  register void* bp = 0;
  register void* ra = 0;
  register void* tx = 0;
  register void* ta = 0;
  register void* spt = 0;
  register void* bpt = 0;
  register void* rat = 0;
  register void* txt = 0;
  register void* tat = 0;
  setjmp(rrs);
}
static void test_many_obj() {
  TEST
      // this will be very slow
      obj* ref_to;
  obj* myobj[20000];
  int t = 500;
  for (int i = 0; i < 20000; i++) {
    myobj[i] = (obj*)cgc_malloc(sizeof(obj));
    myobj[i]->my_val = 9901 + i;
    myobj[i]->name[20] = 'a' + i % 26;
    if (i == t) {
      ref_to = myobj[i];
      assert_eq(myobj[i]->my_val, 9901 + t);
      assert_eq(ref_to->my_val, 9901 + t);
    }
    // cgc(); please don't do this and free useless obj soon.
    assert_eq(myobj[i]->my_val, 9901 + i);
    assert_eq(myobj[i]->name[20], 'a' + i % 26);
  }
  cgc();
  assert_eq(ref_to->my_val, 9901 + t);
  test_calls_cnt += 40000;
  max_same_time = MAX(max_same_time, 20000 * sizeof(obj));
  PASS
}

static void test_many_obj_with_free() {
  TEST
      // this will be faster than last one;
      obj* ref_to;
  obj* myobj[20000];
  int t = 500;
  for (int i = 0; i < 20000; i++) {
    myobj[i] = (obj*)cgc_malloc(sizeof(obj));
    myobj[i]->my_val = 9901 + i;
    myobj[i]->name[20] = 'a' + i % 26;
    if (i == t) {
      ref_to = myobj[i];
      assert_eq(myobj[i]->my_val, 9901 + t);
      assert_eq(ref_to->my_val, 9901 + t);
    }
    assert_eq(myobj[i]->my_val, 9901 + i);
    assert_eq(myobj[i]->name[20], 'a' + i % 26);
    cgc_free(myobj[i]);
  }
  test_calls_cnt += 40000;
  max_same_time = MAX(max_same_time, sizeof(obj));
  PASS
}
typedef struct linkedlist_ {
  int val;
  struct linkedlist_* next;
} * LinkedList;

static void test_cyclic_ref() {
  TEST for (int i = 0; i < 20000; i++) {
    LinkedList l0 = (LinkedList)cgc_malloc(sizeof(struct linkedlist_));
    LinkedList l1 = (LinkedList)cgc_malloc(sizeof(struct linkedlist_));
    LinkedList l2 = (LinkedList)cgc_malloc(sizeof(struct linkedlist_));
    LinkedList l3 = (LinkedList)cgc_malloc(sizeof(struct linkedlist_));
    l0->val = 0;
    l1->val = 1;
    l2->val = 2;
    l3->val = 3;
    l0->next = l1;
    l1->next = l2;
    l2->next = l3;
    l3->next = l0;
    l1 = l2 = l3 = 0;
    cgc();
    for (int i = 0; i < 4; i++) {
      assert_eq(l0->val, i);
      l0 = l0->next;
    }
    if (!i) {
      heap_checker();
    }
    l0 = NULL;
    cgc();
    if (!i) {
      heap_checker();
    }
  }
  max_same_time = MAX(max_same_time, sizeof(struct linkedlist_));
  test_calls_cnt += 20000 * 6;
  PASS
}
static void test(void) {
  cgc_init();
  heap_checker();
  test_gc();
  test_integrity();
  test_many_malloc();
  test_simulate_user_call();
  test_many_obj_with_free();
  test_cyclic_ref();
  clock_t o = clock();
  double kops1 = ((double)test_calls_cnt / (double)1000.0) /
                 ((double)o / ((double)CLOCKS_PER_SEC));
  double util1 =
      ((double)max_same_time) * 100.0 / ((double)(program_break - heap_start));
  test_many_obj();
  LOG("[testing] call gc, no free is used in the test");
  cgc();
  heap_checker();
  test_clear_regs();
  LOG("[testing] clear regs and call gc, no free is used in the test");
  cgc();
  heap_checker();
  LOG("[testing] " D_GREEN(all tests passed));
  double kops2 =
      ((double)20.0) / ((double)(clock() - o) / ((double)CLOCKS_PER_SEC));
  double util2 =
      ((double)max_same_time) * 100.0 / ((double)(program_break - heap_start));
  LOG("[testing] test with gc most the time and free in test_many_objects, "
      "throughput : %.0lf Kops, util: "
      "%.1lf%%.",
      kops1, util1);
  LOG("[testing] test with test_many_objects keep 20000 objs alive, "
      "throughput : %.0lf Kops, util: %.1lf%%.",
      kops2, util2);
}

int main(void) {
  test();
  return 0;
}
