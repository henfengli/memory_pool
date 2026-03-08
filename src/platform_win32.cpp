#ifdef _WIN32

#include "platform.h"
#include "internal.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mutex>
#include <unordered_map>

namespace mp {
namespace platform {

// --- Aligned chunk allocation on Windows ---
static std::mutex g_alloc_map_mutex;
static std::unordered_map<void*, void*> g_alloc_map; // aligned -> raw

void* chunk_alloc(size_t size, size_t alignment) {
    size_t alloc_size = size + alignment;
    void* raw = VirtualAlloc(nullptr, alloc_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!raw) return nullptr;

    uintptr_t addr = (uintptr_t)raw;
    uintptr_t aligned = mp_align_up(addr, alignment);
    void* result = (void*)aligned;

    {
        std::lock_guard<std::mutex> lock(g_alloc_map_mutex);
        g_alloc_map[result] = raw;
    }

    return result;
}

void chunk_free(void* ptr, size_t size) {
    if (!ptr) return;
    void* raw = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_alloc_map_mutex);
        auto it = g_alloc_map.find(ptr);
        if (it != g_alloc_map.end()) {
            raw = it->second;
            g_alloc_map.erase(it);
        }
    }
    if (raw) {
        VirtualFree(raw, 0, MEM_RELEASE);
    }
    (void)size;
}

void mem_decommit(void* ptr, size_t size) {
    VirtualFree(ptr, size, MEM_DECOMMIT);
}

void mem_recommit(void* ptr, size_t size) {
    VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE);
}

uint64_t get_thread_id() {
    return (uint64_t)GetCurrentThreadId();
}

// --- Thread exit callback via TLS + DllMain-like mechanism ---
// We use a TLS slot to store per-thread callback list,
// and the DLL entry point to fire callbacks on thread detach.

struct CallbackNode {
    thread_exit_fn fn;
    void* arg;
    CallbackNode* next;
};

static DWORD g_cb_tls_index = TLS_OUT_OF_INDEXES;
static std::once_flag g_cb_tls_once;

static void init_cb_tls() {
    g_cb_tls_index = TlsAlloc();
}

// Fire all registered callbacks for the current thread.
// Called from DllMain on DLL_THREAD_DETACH.
void fire_thread_exit_callbacks() {
    if (g_cb_tls_index == TLS_OUT_OF_INDEXES) return;
    auto* node = static_cast<CallbackNode*>(TlsGetValue(g_cb_tls_index));
    TlsSetValue(g_cb_tls_index, nullptr);
    while (node) {
        node->fn(node->arg);
        CallbackNode* next = node->next;
        free(node);
        node = next;
    }
}

void register_thread_exit_callback(thread_exit_fn fn, void* arg) {
    std::call_once(g_cb_tls_once, init_cb_tls);

    auto* node = static_cast<CallbackNode*>(malloc(sizeof(CallbackNode)));
    node->fn = fn;
    node->arg = arg;
    node->next = static_cast<CallbackNode*>(TlsGetValue(g_cb_tls_index));
    TlsSetValue(g_cb_tls_index, node);
}

} // namespace platform
} // namespace mp

// DllMain: fire thread exit callbacks on DLL_THREAD_DETACH
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)hinstDLL; (void)lpvReserved;
    switch (fdwReason) {
    case DLL_THREAD_DETACH:
        mp::platform::fire_thread_exit_callbacks();
        break;
    case DLL_PROCESS_DETACH:
        mp::platform::fire_thread_exit_callbacks();
        break;
    }
    return TRUE;
}

#endif // _WIN32
