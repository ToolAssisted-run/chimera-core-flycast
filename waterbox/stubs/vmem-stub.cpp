/* No virtual memory, said plainly.
 *
 * Flycast would rather give the SH4 a REAL address space: reserve 512MB, map
 * the console's RAM into it several times over the way the hardware mirrors
 * it, and let the host MMU do the translation - which makes the recompiler's
 * memory accesses single instructions. Upstream's POSIX implementation of that
 * is shm plus mmap.
 *
 * A waterbox guest has neither. It has one heap, handed to it by the sandbox,
 * and every byte the machine touches must come from inside it: the first
 * attempt at this core mapped memory the sandbox did not know about and died
 * on the very first frame ("unhandled fault ... OUTSIDE every registered
 * block"), which is the sandbox doing exactly its job.
 *
 * So init() refuses. Flycast has a supported path for that - addrspace's
 * virtmemEnabled() goes false, RAM becomes an ordinary allocation, and every
 * access goes through software translation. It costs speed and buys the one
 * thing that matters more: a machine that lives entirely inside the box.
 *
 * The JIT entry points refuse too, and always will here: this core interprets.
 */
#include "types.h"
#include "oslib/virtmem.h"

namespace virtmem
{

bool init(void **vmem_base_addr, void **sh4rcb_addr, size_t ramSize)
{
	(void)vmem_base_addr; (void)sh4rcb_addr; (void)ramSize;
	return false;
}

void destroy() {}
void reset_mem(void *ptr, unsigned size_bytes) { (void)ptr; (void)size_bytes; }
void ondemand_page(void *address, unsigned size_bytes) { (void)address; (void)size_bytes; }
void create_mappings(const Mapping *vmem_maps, unsigned nummaps) { (void)vmem_maps; (void)nummaps; }

/* Page protection: the memory watcher uses these to catch writes to VRAM. With
 * one flat heap there is nothing to protect and nothing that could fault, so
 * they succeed by doing nothing - a watcher that never fires, rather than a
 * lock that silently is not one. */
bool region_lock(void *, std::size_t) { return true; }
bool region_unlock(void *, std::size_t) { return true; }
bool region_set_exec(void *, std::size_t) { return true; }

bool prepare_jit_block(void *, size_t, void **) { return false; }
bool prepare_jit_block(void *, size_t, void **, ptrdiff_t *) { return false; }
void release_jit_block(void *, size_t) {}
void release_jit_block(void *, void *, size_t) {}
void jit_set_exec(void *, size_t, bool) {}
void flush_cache(void *, void *, void *, void *) {}

} // namespace virtmem
