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
#include <sys/mman.h>

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

/* The recompiler's code cache.
 *
 * This is the one thing a sandbox CAN give the dynarec, and it is given in
 * place rather than allocated. On Linux, DECLARE_CODE_CACHE puts the cache in
 * the BINARY - an 11 MB static array in .text - and prepare_jit_block only has
 * to make those pages writable and executable. That placement is not
 * incidental: rec_x64 reaches the SH4 context and the machine's memory with
 * 32-bit displacements, so the code has to live within 2 GB of the data, and
 * an ordinary mmap does not. Handing back a fresh mapping instead of the array
 * gets exactly as far as generating the main loop and then dies inside xbyak
 * with "offset is too big", which is the assembler saying the same thing.
 *
 * So: mprotect what we were handed, and return it. The sandbox implements
 * mprotect and maps PROT_EXEC to MB_PROT_RWX, and the array is already inside
 * the guest image, which the sandbox already owns - nothing new is mapped and
 * nothing lives outside the box.
 *
 * W^X is not attempted. jit_set_exec() exists so a host can flip a page
 * between writable and executable; there is nothing to defend against inside a
 * sandbox whose entire memory is already the guest's, and a flip would be a
 * syscall per block. The pages stay RWX and the toggles do nothing. */
bool prepare_jit_block(void *code_area, size_t size, void **code_area_rwx)
{
	uintptr_t page = (uintptr_t)PAGE_SIZE;
	uintptr_t start = (uintptr_t)code_area & ~(page - 1);
	size_t len = ((uintptr_t)code_area + size + page - 1 - start) & ~(page - 1);
	if (mprotect((void *)start, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
	{
		ERROR_LOG(DYNAREC, "chimera: cannot make the %zu byte code cache executable", size);
		return false;
	}
	*code_area_rwx = code_area;
	return true;
}

/* The dual-mapping form: one address to write through, another to execute
 * from. That needs the same pages mapped twice, which is exactly what a
 * sandbox does not do - so this one stays refused, and Flycast falls back to
 * the single RWX block above. */
bool prepare_jit_block(void *, size_t, void **, ptrdiff_t *) { return false; }

/* Nothing was allocated, so nothing is released. Leaving the pages RWX is
 * harmless: the cache is part of the image and outlives everything anyway. */
void release_jit_block(void *, size_t) {}
void release_jit_block(void *, void *, size_t) {}
void jit_set_exec(void *, size_t, bool) {}

/* x86 keeps its instruction cache coherent with its data cache in hardware, so
 * there is nothing to flush. (The ARM backends need this; the x64 one does
 * not, and this build has no other.) */
void flush_cache(void *, void *, void *, void *) {}

} // namespace virtmem
