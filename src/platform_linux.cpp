#ifndef _WIN32

#include "platform.h"
#include "internal.h"
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdlib>

namespace mp {
namespace platform {

void* chunk_alloc(size_t size, size_t alignment) {
    // Over-allocate to guarantee alignment, then unmap excess.
    size_t alloc_size = size + alignment;
    // MAP_POPULATE eagerly faults in physical pages so first-write does not
    // pay a per-page kernel round-trip later. This aligns Linux behavior with
    // Windows VirtualAlloc(MEM_COMMIT), which is what historical perf docs
    // were measured against. Without this flag, mempool's bump-pointer
    // allocator defers all first-writes to the free phase (since alloc never
    // touches block memory), and Linux's lazy commit charges the entire page
    // fault cost to mp_free — masking the algorithm's true speedup over glibc
    // by 30-40% at sizes >= 1024B. See benchmark numbers in
    // docs/v1.20.0_实测重跑.md.
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MEMPOOL_LINUX_PREFAULT
    flags |= MAP_POPULATE;
#endif
    void* raw = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                     flags, -1, 0);
    if (raw == MAP_FAILED) return nullptr;

    uintptr_t addr = (uintptr_t)raw;
    uintptr_t aligned = mp_align_up(addr, alignment);

    // Unmap prefix
    if (aligned > addr) {
        munmap(raw, aligned - addr);
    }
    // Unmap suffix
    size_t suffix = (addr + alloc_size) - (aligned + size);
    if (suffix > 0) {
        munmap((void*)(aligned + size), suffix);
    }

    return (void*)aligned;
}

void chunk_free(void* ptr, size_t size) {
    if (ptr) munmap(ptr, size);
}

void mem_decommit(void* ptr, size_t size) {
    madvise(ptr, size, MADV_DONTNEED);
}

void mem_recommit(void* ptr, size_t size) {
    // On Linux, MADV_DONTNEED pages are automatically recommitted on access.
    (void)ptr; (void)size;
}

// Thread-local key for exit callbacks
static pthread_key_t g_tls_key;
static pthread_once_t g_tls_once = PTHREAD_ONCE_INIT;

struct TlsCallbackNode {
    thread_exit_fn fn;
    void* arg;
    TlsCallbackNode* next;
};

static void tls_destructor(void* ptr) {
    auto* node = static_cast<TlsCallbackNode*>(ptr);
    while (node) {
        node->fn(node->arg);
        auto* next = node->next;
        free(node);
        node = next;
    }
}

static void init_tls_key() {
    pthread_key_create(&g_tls_key, tls_destructor);
}

void register_thread_exit_callback(thread_exit_fn fn, void* arg) {
    pthread_once(&g_tls_once, init_tls_key);

    auto* node = static_cast<TlsCallbackNode*>(malloc(sizeof(TlsCallbackNode)));
    node->fn = fn;
    node->arg = arg;
    node->next = static_cast<TlsCallbackNode*>(pthread_getspecific(g_tls_key));
    pthread_setspecific(g_tls_key, node);
}

} // namespace platform
} // namespace mp

#endif // !_WIN32
