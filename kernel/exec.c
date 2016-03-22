#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "defs.h"
#include "x86.h"
#include "elf.h"

int
exec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  // char* gets longer in 64bit
  // and these are converted to char*, soo...
  uint64 argc, sz, sp, ustack[3+MAXARG+1];
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pml4e_t *pml4, *oldpml4;

  if((ip = namei(path)) == 0)
    return -1;
  ilock(ip);
  pml4 = 0;

  // Check ELF header
  if(readi(ip, (char*)&elf, 0, sizeof(elf)) < sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pml4 = setupkvm()) == 0)
    goto bad;

  // Load program into memory.
  sz = 0;
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, (char*)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if((sz = allocuvm(pml4, sz, ph.vaddr + ph.memsz)) == 0)
      goto bad;
    if(loaduvm(pml4, (char*)ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  ip = 0;

  // Allocate two pages at the next page boundary.
  // Make the first inaccessible.  Use the second as the user stack.
  sz = PGROUNDUP(sz);
  if((sz = allocuvm(pml4, sz, sz + 2*PGSIZE)) == 0)
    goto bad;
  clearpteu(pml4, (char*)(sz - 2*PGSIZE));
  sp = sz;

  // Push argument strings, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
      sp = (sp - (strlen(argv[argc]) + 1)) & ~(sizeof(uint64)-1);
    if(copyout(pml4, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[3+argc] = sp;
  }
  ustack[3+argc] = 0;

  ustack[0] = 0xffffffff;  // fake return PC
  ustack[1] = argc;
  ustack[2] = sp - (argc+1)*sizeof(uint64);  // argv pointer

  proc->tf->rdi = argc;
  proc->tf->rsi = sp - (argc+1)*sizeof(uint64);

  sp -= (3+argc+1) * sizeof(uint64);
  if(copyout(pml4, sp, ustack, (3+argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(proc->name, last, sizeof(proc->name));

  // Commit to the user image.
  oldpml4 = proc->pml4;
  proc->pml4 = pml4;
  proc->sz = sz;
  // register names...
  proc->tf->rip = elf.entry;  // main
  proc->tf->rsp = sp;
  switchuvm(proc);
  freevm(oldpml4);
  return 0;

 bad:
  if(pml4)
    freevm(pml4);
  if(ip)
    iunlockput(ip);
  return -1;
}
