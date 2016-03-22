#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

extern char data[];  // defined by kernel.ld
pml4e_t *kpml4;  // for use in scheduler()
//struct segdesc gdt[NSEGS];

__thread struct cpu *cpu;
__thread struct proc *proc;

static void tss_set_rsp(uint *tss, uint n, uint64 rsp) {
  tss[n*2 + 1] = rsp;
  tss[n*2 + 2] = rsp >> 32;
}

// static void tss_set_ist(uint *tss, uint n, uint64 ist) {
//   tss[n*2 + 7] = ist;
//   tss[n*2 + 8] = ist >> 32;
// }

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  uint64 *gdt;
  uint *tss;
  uint64 addr;
  void *local;
  struct cpu *c;

  // create a page for cpu local storage
  local = kalloc();
  memset(local, 0, PGSIZE);

  gdt = (uint64*) local;
  tss = (uint*) (((char*) local) + 1024);
  tss[16] = 0x00680000; // IO Map Base = End of TSS

  // point FS smack in the middle of our local storage page
  wrmsr(FS_BAS, ((uint64) local) + (PGSIZE / 2));

  c = &cpus[cpunum()];
  c->local = local;

  cpu = c;
  proc = 0;

  addr = (uint64) tss;
  // // gdt[0] =         0x0000000000000000;
  // gdt[SEG_KCODE] = SEG_CODE64(STA_X|STA_R, 0);
  // gdt[SEG_UCODE] = SEG_CODE64(STA_X|STA_R, DPL_USER);
  // gdt[SEG_KDATA] = SEG(STA_W, 0, 0, 0);
  // // gdt[SEG_KCPU]  = 0x0000000000000000;  // unused
  // gdt[SEG_UDATA] = SEG(STA_W, 0, 0, DPL_USER);
  // gdt[SEG_TSS+0] = SEG_TSS64(addr, 0x0067, DPL_USER)
  // gdt[SEG_TSS+1] = (addr >> 32);
  gdt[0] =         0x0000000000000000;
  gdt[SEG_KCODE] = 0x0020980000000000;  // Code, DPL=0, R/X
  gdt[SEG_UCODE] = 0x0020F80000000000;  // Code, DPL=3, R/X
  gdt[SEG_KDATA] = 0x0000920000000000;  // Data, DPL=0, W
  gdt[SEG_KCPU]  = 0x0000000000000000;  // unused
  gdt[SEG_UDATA] = 0x0000F20000000000;  // Data, DPL=3, W
  gdt[SEG_TSS+0] = (0x0067) | ((addr & 0xFFFFFF) << 16) |
                   (0x00E9LL << 40) | (((addr >> 24) & 0xFF) << 56);
  gdt[SEG_TSS+1] = (addr >> 32);

  lgdt((void*) gdt, 8 * sizeof(uint64));

  ltr(SEG_TSS << 3);
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpml(pml4e_t *pml4, const void *va, int alloc)
{
  pml4e_t *pml4e;
  pdpte_t *pdpte;
  pde_t *pde;
  pte_t *pgtab;

  pml4e = &pml4[PML4X(va)];
  if(*pml4e & PTE_P) {
    pdpte = (pdpte_t*)p2v(PTE_ADDR(*pml4e));
  } else {
    if(!alloc || (pdpte = (pdpte_t*)kalloc()) == 0)
      return 0;
    memset(pdpte, 0, PGSIZE);
    *pml4e = v2p(pdpte) | PTE_P | PTE_W | PTE_U;
  }

  pdpte = &pdpte[PDPTX(va)];
  if(*pdpte & PTE_P) {
    pde = (pde_t*)p2v(PTE_ADDR(*pdpte));
  } else {
    if(!alloc || (pde = (pde_t*)kalloc()) == 0)
      return 0;
    memset(pde, 0, PGSIZE);
    *pdpte = v2p(pde) | PTE_P | PTE_W | PTE_U;
  }

  pde = &pde[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)p2v(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = v2p(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pml4e_t *pml4, void *va, uint64 size, uint64 pa, int perm)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uint64)va);
  last = (char*)PGROUNDDOWN(((uint64)va) + size - 1);
  for(;;){
    if((pte = walkpml(pml4, a, 1)) == 0)
      {
        cprintf("mappages: walkpml failed!\n");
        return -1;
      }
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.

#define IOFLAGS PTE_PS | PTE_P | PTE_W | PTE_PWT | PTE_PCD
static struct kmap {
  void *virt;
  uint64 phys_start;
  uint64 phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W },   // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0 },       // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W },   // kern data+memory
 { (void*)DEVBASE,  DEVSPACE,      DEVTOP,    IOFLAGS }, // more devices
};

// Set up kernel part of a page table.
pml4e_t*
setupkvm(void)
{
  pml4e_t *pml4;
  struct kmap *k;

  if((pml4 = (pml4e_t*)kalloc()) == 0)
    return 0;
  memset(pml4, 0, PGSIZE);
  // if (p2v(PHYSTOP) > (void*)DEVSPACE)
    // panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pml4, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm) < 0)
    {
      cprintf("setupkvm: mappages failed!\n");
      return 0;
    }
  return pml4;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpml4 = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(v2p(kpml4));   // switch to the kernel page table
}

void
switchuvm(struct proc *p)
{
  void *pml4;
  uint *tss;
  pushcli();
  if(p->pml4 == 0)
    panic("switchuvm: no pml4");
  tss = (uint*) (((char*) cpu->local) + 1024);
  // set kstack to 0th entry for
  tss_set_rsp(tss, 0, (uint64)proc->kstack + KSTACKSIZE);
  pml4 = (void*) PTE_ADDR(p->pml4);
  lcr3(v2p(pml4));
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pml4e_t *pml4, char *init, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pml4, 0, PGSIZE, v2p(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pml4e_t *pml4, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint64) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpml(pml4, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, p2v(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
// changed to uint64 since vm size can be gigantic...
int
allocuvm(pml4e_t *pml4, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pml4, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    mappages(pml4, (char*)a, PGSIZE, v2p(mem), PTE_W|PTE_U);
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pml4e_t *pml4, uint64 oldsz, uint64 newsz)
{
  pte_t *pte;
  uint64 a, pa;

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpml(pml4, (char*)a, 0);
    if(!pte)
      a += (NPTENTRIES - 1) * PGSIZE;
    else if((*pte & PTE_P) != 0){
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      char *v = p2v(pa);
      kfree(v);
      *pte = 0;
    }
  }
  return newsz;
}

// Free ptpde and the pgdir it points to
void
freepgdir(pde_t *pgdir)
{
    uint i;
    for(i = 0; i < NPDENTRIES; i++){
      if(pgdir[i] & PTE_P){
        // free the pages used in page tables
        // pages pointed by page tables must deallocuvm'ed first
        char *v = p2v(PTE_ADDR(pgdir[i]));
        kfree(v);
      }
    }
    kfree((char*)pgdir);
}

// Free the pml4e and the pdpt it points to
void
freepdpt(pdpte_t *pdpt)
{
    uint i;
    for(i = 0; i < NPDPTENTRIES; i++){
      if(pdpt[i] & PTE_P)
        freepgdir( (pde_t*)p2v(PTE_ADDR(pdpt[i])) );
    }
    kfree((char*)pdpt);
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pml4e_t *pml4)
{
  uint i;

  if(pml4 == 0)
    panic("freevm: no pml4");
  deallocuvm(pml4, PHYSTOP, 0);
  for(i = 0; i < NPML4ENTRIES-1; i++){
    if(pml4[i] & PTE_P)
      freepdpt( (pdpte_t*)p2v(PTE_ADDR(pml4[i])) );
  }
  kfree((char*)pml4);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pml4e_t *pml4, char *uva)
{
  pte_t *pte;

  pte = walkpml(pml4, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pml4e_t*
copyuvm(pml4e_t *pml4, uint sz)
{
  pml4e_t *d;
  pte_t *pte;
  uint64 pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0) {
    cprintf("copyuvm: setupkvm failed!\n");
    return 0;
  }
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpml(pml4, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(!(*pte & PTE_P))
      panic("copyuvm: page not present");
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto bad;
    memmove(mem, (char*)p2v(pa), PGSIZE);
    if(mappages(d, (void*)i, PGSIZE, v2p(mem), flags) < 0)
      goto bad;
  }
  return d;

bad:
  freevm(d);
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pml4e_t *pml4, char *uva)
{
  pte_t *pte;

  pte = walkpml(pml4, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)p2v(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pml4e_t *pml4, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint64 n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pml4, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}
