#pragma once

using namespace std;

#include <signal.h>
#include <atomic>
#include <iostream>
#include <sys/mman.h>

// You can assume this as your page size. On some OSs (e.g. macOS), 
// it may in fact be larger and you'll waste memory due to internal 
// fragmentation as a result, but that's okay for this exercise.
constexpr size_t pageSize = 4096;

class MMapObject {
    // The size of the allocated contiguous pages (i.e. the size passed to mmap)
    size_t m_mmapSize;

    // If the type is an arena, the size of each item in the arena. If a big alloc,
    // should be zero.
    size_t m_arenaSize;

    // Debug counter for asserting we freed all the pages we were supposed to.
    // Thread safe and you can ignore it. It's for tests and seeing how many
    // outstanding pages there are.
    static std::atomic<size_t> s_outstandingPages;
public:
    MMapObject(const MMapObject& other) = delete;
    MMapObject() = delete;

    /**
     * The number of contiguous bytes in this mmap allocation.
     */
    size_t mmapSize() {
        return m_mmapSize;
    }

    /**
     * If the type of this mmap overlay is an arena, this is the size of its items.
     * If a single allocation, this is zero.
     */
    size_t arenaSize() {
        return m_arenaSize;
    }

    /**
     * This function should call mmap to allocate a contiguous set of pages with
     * the passed size. If the caller is intending to use this region as an arena,
     * they should set arenaSize to the size of its items.
     * 
     * If this is a large allocation, the caller should set arenaSize to 0.
     */
    static MMapObject* alloc(size_t size, size_t arenaSize) 
    {
        void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
        if (ptr == MAP_FAILED)
        {
            return nullptr;
        }
        MMapObject* obj = (MMapObject*)ptr;
        obj->m_mmapSize = size;
        obj->m_arenaSize = arenaSize;
        s_outstandingPages++;
        return obj;
    }

    /**
     * This function should deallocate the passed pointer by calling munmap.
     * The passed pointer may not be at the start of the memory region, but will
     * be withing it, so you'll need to calculate the start of the MMapObject* ptr,
     * passing that as start of the region to unmap and ptr->mmapSize() as its length.
     * 
     * Recall that Arenas will never be larger than the OS page size and BigAllocs
     * always return a pointer to just after the MMapObject header, so you can
     * jump back to the nearest multiple of page size and that will be the MMapObject*.
     */
    static void dealloc(void* obj) {
        uintptr_t n = reinterpret_cast<uintptr_t>(obj); 
        uintptr_t remainder = n % pageSize;
        n = n - remainder;
        MMapObject* map = reinterpret_cast<MMapObject*>(n);
        int ret = munmap(map, map->mmapSize());

        size_t old = s_outstandingPages--;

        // If there previously 0 pages, then we goofed and tried to free more pages
        // than we allocated. This is a serious bug, so sigtrap and your debugger
        // can break on this line. If not debugging, you'll get a SIGTRAP message
        // and your program will exit.
        if (old == 0) {
            raise(SIGTRAP);
        }
    }

    /**
     * Returns the number of pages outstanding that have not been collected.
     * Don't touch this.
     */
    static size_t outstandingPages() {
        return s_outstandingPages.load();
    }
};

class BigAlloc : public MMapObject {
    // This inherits from MMapObject, so it also has the mmapSize and arenSize
    // members as well.

    char m_data[0];

public:
    BigAlloc(const BigAlloc& other) = delete;
    BigAlloc() = delete;

    /**
     * This method should allocate a single large contiguous block of memory using
     * MMapObject::alloc(). You then need to treat that pointer as a BigAlloc*
     * and return the address of the allocation *after* the header.
     * 
     * The returned address must be 64-bit aligned.
     */
    static void* alloc(size_t size) {
        size_t fullSize = size + sizeof(BigAlloc);
        void* ptr = MMapObject::alloc(fullSize, 0);
        BigAlloc* obj = (BigAlloc*)ptr;
        return &obj->m_data[0];
    }
};

// This is the data overlay for your Arena allocator.
// It inherits from MMapObject, and thus has a size_
class Arena : public MMapObject {
    // This inherits from MMapObject, so it also has the mmapSize and arenaSize
    // members as well.

    // You'll need some means of tracking the number of freed items in this arena
    // in a thread-safe manner. That should go here.
    std::atomic<size_t> freedItems;
    std::atomic<size_t> totalSpaceUsed;
    std::atomic<size_t> totalSpaceUsedNoHeader;

    // A pointer to the next free address in the arena.
    char* m_next;

    // This might look kind of weird as it's size is zero, but this serves as a surrogate
    // location to start of the arena's allocation slots. That is &this->m_data[0] is a pointer
    // to the first allocation slot, &this->m_data[arenaSize()] is a pointer to the second
    // and so forth.
    //
    // Note, if you put any data members in this class, you must put them *before* this.
    // Additionally, you need to ensure this address is 64-bit aligned, so you need appropriate
    // padding or to ensure the sizes of your previous members ensures this happens before this.
    //
    // If sizeof(Arena) % 8 == 0, you should be good.
    char m_data[0];

public:
    /**
     * Creates an arena with items of the given size. You should allocate with
     * MMapObject::alloc() and coerce the result into an Arena*.
     */
    static Arena* create(uint32_t itemSize) {
        void* ptr = MMapObject::alloc(pageSize, itemSize);
        Arena* obj = (Arena*)ptr;
        obj->m_next = &obj->m_data[0];
        obj->totalSpaceUsed = sizeof(Arena);
        if (sizeof(obj) % 8 != 0)
        {
            raise(SIGTRAP);
        }
        return obj;
    }

    /**
     * Allocates an item in the arena and returns its address. Returns null if you
     * have already exceeded the bounds of the arena.
     */
    void* alloc() {
        if (this->full() != true)
        {
            char* temp = this->m_next;
            totalSpaceUsed += arenaSize();
            totalSpaceUsedNoHeader += arenaSize();

            this->m_next = &this->m_data[totalSpaceUsed.load()];
            if (temp == nullptr)
            {
                raise(SIGTRAP);
            }
            return temp;
        }
        return nullptr;
    }

    /**
     * Marks one of the items in the arena as freed. Returns true if this arena
     * has no more allocation slots and everything is free'd.
     */
    bool free() {
        freedItems++;
        if (freedItems.load() == (totalSpaceUsedNoHeader.load() / arenaSize()) && full())
        {
            return true;
        }
        return false;
    }

    /**
     * Whether or not this arena can hold more items.
     */
    bool full() {
        if (totalSpaceUsed.load() + arenaSize() > pageSize)
        {
            return true;
        }
        return false;
    }

    /**
     * Returns a pointer to the next free item in the arena.
     */
    char* next() {
        return m_next;
    }
};
class ArenaStore {
    /**
     * A set of arenas with the following sizes:
     * 0: 8 bytes
     * 1: 16 bytes
     * 2: 32 bytes
     * ...
     * 8: 1024 bytes
     */
    Arena* m_arenas[9]; // Default initializer for pointer is nullptr

public:
    /**
     * Allocates `bytes` bytes of data. If the data is too large to fit in an arena,
     * it will be allocated using BigAlloc.
     */
    void* alloc(size_t bytes) {
        if (bytes <= 8)
        {
            Arena* arena = m_arenas[0];
            Arena* temp;
            if (arena == nullptr)
            {
                Arena* newArena = Arena::create(8);
                temp = (Arena*)newArena->alloc();
            }
            else
            {
                temp = (Arena*)arena->alloc();
            }
            m_arenas[0] = arena;
            return temp;
        }
        else if (bytes <= 16)
        {
            Arena* arena = m_arenas[1];
            Arena* temp;
            if (arena == nullptr)
            {
                Arena* newArena = Arena::create(16);
                m_arenas[1] = newArena;
                temp = (Arena*)newArena->alloc();
            }
            else
            {
                temp = (Arena*)arena->alloc();
            }
            m_arenas[1] = arena;
            return temp;
        }
        else if (bytes <= 32)
        {
            Arena* arena = m_arenas[2];
            Arena* temp;
            if (arena == nullptr)
            {
                Arena* newArena = Arena::create(32);
                m_arenas[2] = newArena;
                temp = (Arena*)newArena->alloc();
            }
            else
            {
                temp = (Arena*)arena->alloc();
            }
            m_arenas[2] = arena;
            return temp;
        }
        else if (bytes <= 64)
        {
            Arena* arena = m_arenas[3];
            Arena* temp;
            if (arena == nullptr)
            {
                Arena* newArena = Arena::create(64);
                m_arenas[3] = newArena;
                temp = (Arena*)newArena->alloc();
            }
            else
            {
                temp = (Arena*)arena->alloc();
            }
            m_arenas[3] = arena;
            return temp;
        }
        else if (bytes <= 128)
        {
            Arena* arena = m_arenas[4];
            Arena* temp;
            if (arena == nullptr)
            {
                Arena* newArena = Arena::create(128);
                m_arenas[4] = newArena;
                temp = (Arena*)newArena->alloc();
            }
            else
            {
                temp = (Arena*)arena->alloc();
            }
            m_arenas[4] = arena;
            return temp;
        }
        else if (bytes <= 256)
        {
            Arena* arena = m_arenas[5];
            Arena* temp;
            if (arena == nullptr)
            {
                Arena* newArena = Arena::create(256);
                m_arenas[5] = newArena;
                temp = (Arena*)newArena->alloc();
            }
            else
            {
                temp = (Arena*)arena->alloc();
            }
            m_arenas[5] = arena;
            return temp;
        }
        else if (bytes <= 512)
        {
            Arena* arena = m_arenas[6];
            Arena* temp;
            if (arena == nullptr)
            {
                Arena* newArena = Arena::create(512);
                m_arenas[6] = newArena;
                temp = (Arena*)newArena->alloc();
            }
            else
            {
                temp = (Arena*)arena->alloc();
            }
            m_arenas[6] = arena;
            return temp;
        }
        else if (bytes <= 1024)
        {
            Arena* arena = m_arenas[7];
            Arena* temp;
            if (arena == nullptr)
            {
                Arena* newArena = Arena::create(1024);
                m_arenas[7] = newArena;
                temp = (Arena*)newArena->alloc();
            }
            else
            {
                temp = (Arena*)arena->alloc();
            }
            m_arenas[7] = arena;
            return temp;
        }
        else
        {
            return BigAlloc::alloc(bytes);
        }
    }

    /**
     * Determines the allocation type for the given pointer and calls
     * the appropriate free method.
     */
    void free(void* ptr) {
        uintptr_t n = reinterpret_cast<uintptr_t>(ptr); 
        uintptr_t remainder = n % pageSize;
        n = n - remainder;
        MMapObject* map = reinterpret_cast<MMapObject*>(n);
        if (map->arenaSize() == 0)
        {
            MMapObject::dealloc(ptr);
        }
        else
        {
            Arena* arena = (Arena*) map;
            bool freed = arena->free();
            MMapObject::dealloc(ptr);
        }
    }
};

void* myMalloc(size_t n);
void myFree(void* ptr);