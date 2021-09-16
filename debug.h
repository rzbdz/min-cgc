
#ifndef D_GREEN
#define D_GREEN(s) "\033[32;32m" #s "\033[0m"
#endif
#ifndef D_RED
#define D_RED(s) "\033[31;31m" #s "\033[0m"
#endif
#ifndef D_BLUE
#define D_BLUE(s) "\033[34;34m" #s "\033[0m"
#endif

#ifdef NDEBUG
// there are some problems using assert.h
#ifndef assert
#define assert(expr) ((void)0)
#endif
#define assert_eq(expr1, expr2) ((void)0)
#else
// there are some problems using glibc assert.h
// I decided to implemente my own.
#ifndef assert
#define assert(expr)                           \
  if (!(expr)) {                               \
    fprintf(stderr,                            \
            "[%s %d] %s: Assertion \"" #expr   \
            "\" failed."                       \
            "\n",                              \
            __FILE__, __LINE__, __FUNCTION__); \
    abort();                                   \
  }
#define assert_pass(x) \
  assert(x);           \
  LOG_DEBUG(#x " assert passed %lx", (size_t)(x));
#endif
#ifndef assert_eq
#define assert_eq(expr1, expr2)                                         \
  if (!((expr1) == (expr2))) {                                          \
    fprintf(stderr,                                                     \
            "[%s %d] %s: Eq Assertion failed: " #expr1 ": %lx, " #expr2 \
            ": %lx \n",                                                 \
            __FILE__, __LINE__, __FUNCTION__, (size_t)(expr1),          \
            (size_t)(expr2));                                           \
    abort();                                                            \
  }
#endif
#endif

#ifdef DEBUG_

// don't use stdio and printf/scanf for there are malloc calls in them.
// use stderr who has no buffer
#define LOG_DEBUG(fmt, args...) \
  fprintf(stderr, "[%s %d] " fmt "\n", __FILE__, __LINE__, ##args);

#define P_P(x) LOG_DEBUG(#x " %lx", (size_t)(x));
#define P_D(x) LOG_DEBUG(#x " %lu", (size_t)(x));
#else
#define LOG_DEBUG(fmt, ...)
#define P_P(x)
#define P_D(x)
#endif

#ifdef TEST_
#define LOG_TEST(fmt, args...) \
  fprintf(stderr, "[%s %d] " fmt "\n", __FILE__, __LINE__, ##args);
#else
#define LOG_TEST(fmt, ...)
#endif

#ifdef LOG_
#define LOG(fmt, args...) fprintf(stderr, "[LOG: CGC] " fmt "\n", ##args);
#else
#define LOG(fmt, ...)
#endif
