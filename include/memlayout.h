// Memory layout
// reference of linux documentation:
// Virtual memory map with 4 level page tables:
//
// 0000000000000000 - 00007fffffffffff (=47 bits) user space, different per mm
// hole caused by [48:63] sign extension
// ffff800000000000 - ffff87ffffffffff (=43 bits) guard hole, reserved for hypervisor
// ffff880000000000 - ffffc7ffffffffff (=64 TB) direct mapping of all phys. memory
// ffffc80000000000 - ffffc8ffffffffff (=40 bits) hole
// ffffc90000000000 - ffffe8ffffffffff (=45 bits) vmalloc/ioremap space
// ffffe90000000000 - ffffe9ffffffffff (=40 bits) hole
// ffffea0000000000 - ffffeaffffffffff (=40 bits) virtual memory map (1TB)
// ... unused hole ...
// ffffec0000000000 - fffffc0000000000 (=44 bits) kasan shadow memory (16TB)
// ... unused hole ...
// ffffff0000000000 - ffffff7fffffffff (=39 bits) %esp fixup stacks
// ... unused hole ...
// ffffffff80000000 - ffffffffa0000000 (=512 MB)  kernel text mapping, from phys 0
// ffffffffa0000000 - ffffffffff5fffff (=1525 MB) module mapping space
// ffffffffff600000 - ffffffffffdfffff (=8 MB) vsyscalls
// ffffffffffe00000 - ffffffffffffffff (=2 MB) unused hole
//
// The direct mapping covers all memory in the system up to the highest
// memory address (this means in some cases it can also include PCI memory
// holes).
//
// vmalloc space is lazily synchronized into the different PML4 pages of
// the processes using the page fault handler, with init_level4_pgt as
// reference.
//
// Current X86-64 implementations only support 40 bits of address space,
// but we support up to 46 bits. This expands into MBZ space in the page tables.
//
// ->trampoline_pgd:
//
// We map EFI runtime services in the aforementioned PGD in the virtual
// range of 64Gb (arbitrarily set, can be raised if needed)
//
// 0xffffffef00000000 - 0xffffffff00000000
//
// -Andi Kleen, Jul 2004

#define EXTMEM   0x100000           // Start of extended memory
#define PHYSTOP  0xE000000          // Top physical memory
#define DEVSPACE 0xFE000000         // Other devices are at high addresses
#define USREND   0x7FFFFFFFFFFF

// Key addresses for address space layout (see kmap in vm.c for layout)
#define KERNBASE 0xFFFFFFFF80000000 // First kernel virtual address
#define DEVBASE  0xFFFFFFFFa0000000 // First device virtual address
#define KERNLINK (KERNBASE+EXTMEM)  // Address where kernel is linked
#define DEVTOP   (DEVSPACE + 16 * PGSIZE)

#ifndef __ASSEMBLER__

// changed uint to uint64 which is unsigned long
static inline uint64 v2p(void *a) { return ((uint64) (a)) - ((uint64)KERNBASE); }
static inline void *p2v(uint64 a) { return (void *) ((a) + ((uint64)KERNBASE)); }

#endif

#define V2P(a) (((uint64) (a)) - KERNBASE)
#define P2V(a) (((void *) (a)) + KERNBASE)
#define IO2V(a) (((void *) (a)) + DEVBASE - DEVSPACE)

#define V2P_WO(x) ((x) - KERNBASE)    // same as V2P, but without casts
#define P2V_WO(x) ((x) + KERNBASE)    // same as V2P, but without casts
