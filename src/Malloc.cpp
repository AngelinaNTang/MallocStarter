#include <Malloc.hpp>
#include <sys/mman.h>

/**
 * Your special drop-in replacement for malloc(). Should behave the same way.
 */
void* myMalloc(size_t n) {
    // auto ret = ArenaStore::alloc(n);
    // return ret;
}

/**
 * Your special drop-in replacement for free(). Should behave the same way.
 */
void myFree(void* addr) {
    // ArenaStore::free(addr);
}

std::atomic<size_t> MMapObject::s_outstandingPages = 0;