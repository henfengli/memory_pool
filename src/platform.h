#ifndef MEMPOOL_PLATFORM_H
#define MEMPOOL_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

namespace mp {
namespace platform {

// Allocate a chunk of memory aligned to `alignment` bytes.
// Returns nullptr on failure. The returned memory is zero-initialized.
void* chunk_alloc(size_t size, size_t alignment);

// Free a chunk previously allocated with chunk_alloc.
void  chunk_free(void* ptr, size_t size);

// Decommit (release physical pages) for a range within a chunk.
// The virtual address range is preserved but physical memory is returned to OS.
void  mem_decommit(void* ptr, size_t size);

// Recommit (make usable again) a previously decommitted range.
void  mem_recommit(void* ptr, size_t size);

// Get current thread's unique ID.
uint64_t get_thread_id();

// Callback type for thread exit notification.
using thread_exit_fn = void(*)(void* arg);

// Register a callback to be invoked when the current thread exits.
// `arg` is passed to the callback.
void register_thread_exit_callback(thread_exit_fn fn, void* arg);

} // namespace platform
} // namespace mp

#endif /* MEMPOOL_PLATFORM_H */
