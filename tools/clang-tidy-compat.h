/*
 * Fixup header for running clang-tidy against picolibc-based ESP-IDF projects.
 *
 * Force-included via -include before any source file so that the include guard
 * in sys/cdefs.h prevents later re-definitions.
 *
 * Picolibc defines __noreturn as [[noreturn]] in C++11 mode, but Clang rejects
 * that attribute when it appears after a function's parameter list (e.g.
 *   void pthread_exit(void *) [[noreturn]];
 * ). Override it to the equivalent __attribute__ form that both GCC and Clang
 * accept in any position.
 */

#ifdef __clang__

#include <sys/cdefs.h>

#undef __noreturn
#define __noreturn __attribute__((__noreturn__))

#endif
