#ifndef _WIN32

#include "platform.h"
#include "internal.h"
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>

namespace mp {
namespace platform {

void* chunk_alloc(size_t size, size_t alignment) {
    // Over-allocate to guarantee alignment, then unmap excess.
    size_t alloc_size = size + alignment;
    void* raw = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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

uint64_t get_thread_id() {
    return (uint64_t)pthread_self();
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
