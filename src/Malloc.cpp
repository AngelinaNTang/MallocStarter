#include <Malloc.hpp>
#include <sys/mman.h>

thread_local ArenaStore a;

/**
 * Your special drop-in replacement for malloc(). Should behave the same way.
 */
void* myMalloc(size_t n) {
    auto ret = a.alloc(n);
    return ret;
}

/**
 * Your special drop-in replacement for free(). Should behave the same way.
 */
void myFree(void* addr) {
    a.free(addr);
}


std::atomic<size_t> MMapObject::s_outstandingPages = 0;