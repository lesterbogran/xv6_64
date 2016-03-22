# Multiboot header, for multiboot boot loaders like GNU Grub.
# http://www.gnu.org/software/grub/manual/multiboot/multiboot.html
#
# Using GRUB 2, you can boot xv6 from a file stored in a
# Linux file system by copying kernel or kernelmemfs to /boot
# and then adding this menu entry:
#
# menuentry "xv6" {
# 	insmod ext2
# 	set root='(hd0,msdos1)'
# 	set kernel='/boot/kernel'
# 	echo "Loading ${kernel}..."
# 	multiboot ${kernel} ${kernel}
# 	boot
# }

#include "asm.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"

#define mboot_magic 0x1badb002
#define mboot_flags 0x00010000

# Multiboot header.  Data to direct multiboot loader.
.code32
.global mboot_header
.global mboot_entry

mboot_header:
  .long mboot_magic
  .long mboot_flags
  .long (-mboot_magic -mboot_flags)	# checksum
  .long mboot_load_addr			# header_addr
  .long mboot_load_addr
  .long mboot_load_end
  .long mboot_bss_end
  .long mboot_entry_addr

mboot_entry:

# zero 4 pages for our bootstrap page tables
  xor %eax, %eax
  mov $0x1000, %edi
  mov $0x5000, %ecx
  rep stosb

# P4ML[0] -> 0x2000 (PDPT-A)
  mov $(0x2000 | PTE_P | PTE_W), %eax
  mov %eax, 0x1000

# P4ML[511] -> 0x3000 (PDPT-B)
  mov $(0x3000 | PTE_P | PTE_W), %eax
  mov %eax, 0x1FF8

# PDPT-A[0] -> 0x4000 (PD)
  mov $(0x4000 | PTE_P | PTE_W), %eax
  mov %eax, 0x2000

# PDPT-B[510] -> 0x4000 (PD)
  mov $(0x4000 | PTE_P | PTE_W), %eax
  mov %eax, 0x3FF0

# PD[0..511] -> 0..1022MB
  mov $(0x0 | PTE_PS | PTE_P | PTE_W), %eax
  mov $0x4000, %ebx
  mov $512, %ecx
ptbl_loop:
  mov %eax, (%ebx)
  add $0x200000, %eax # add 2MB
  add $0x8, %ebx
  dec %ecx
  jnz ptbl_loop

# Clear ebx for initial processor boot.
# When secondary processors boot, they'll call through
# entry32mp (from entryother), but with a nonzero ebx.
# We'll reuse these bootstrap pagetables and GDT.
  xor %ebx, %ebx

.global entry32mp
entry32mp:
# CR3 -> 0x1000 (P4ML)
  mov      $0x1000, %eax
  mov      %eax, %cr3

  lgdt (gdtr64 - mboot_header + mboot_load_addr)

# Enable PAE - CR4.PAE=1
  mov      %cr4, %eax
  orl      $CR4_PAE, %eax
  mov      %eax, %cr4

# Enable long mode - EFER.LME=1
  mov      $EFER, %ecx
  rdmsr
  orl      $EFER_LME, %eax
  wrmsr

# Turn on paging.
  movl     %cr0, %eax
  orl      $(CR0_PG|CR0_WP), %eax
  movl     %eax, %cr0

# shift to 64bit segment
  ljmp     $8,$(entry64low - mboot_header + mboot_load_addr)


# Turn on page size extension for 4Mbyte pages
#  movl    %cr4, %eax
#  orl     $(CR4_PSE), %eax
#  movl    %eax, %cr4

# Set pml4
#  movl    $(V2P_WO(entrypml4)), %eax
#  movl    %eax, %cr3

.align 16
gdtr64:
  .word gdt64_end - gdt64_begin - 1;
  .quad gdt64_begin - mboot_header + mboot_load_addr

.align 16
gdt64_begin:
  SEG_NULLASM                                # null seg
  SEG_ASM_64 (STA_X|STA_R, 0x0, 0x0)         # code seg
  SEG_ASM    (STA_W,       0x0, 0x0)         # data seg
#    .long 0x00000000 # 0: null desc
#    .long 0x00000000
#    .long 0x00000000 # 1: Code, R/X, Nonconforming
#    .long 0x00209800
#    .long 0x00000000 # 2: Data, R/W, Expand Down
#    .long 0x00009000
gdt64_end:

.align 16
.code64
entry64low:
# jump to 64 bit segment in high addresses.
# The indirect call is needed because
# the assembler produces a PC-relative instruction
# for a direct jump.
  movq $entry64high, %rax
  jmp *%rax

.global _start
_start:
entry64high:

# ensure data segment registers are sane
  xor %rax, %rax
  mov %ax, %ss
  mov %ax, %ds
  mov %ax, %es
  mov %ax, %fs
  mov %ax, %gs

# check to see if we're booting a secondary core
  test %ebx, %ebx
  jnz entry64mp

# Set up the stack pointer.
  mov $(stack + KSTACKSIZE), %rsp

# enter main()
  jmp main

# we should never return here...
  jmp .

entry64mp:
# obtain kstack from data block before entryother
  mov $0x7000, %rax
  mov -16(%rax), %rsp
  jmp mpenter

.global wrmsr
wrmsr:
  mov %rdi, %rcx     # arg0 -> msrnum
  mov %rsi, %rax     # val.low -> eax
  shr $32, %rsi
  mov %rsi, %rdx     # val.high -> edx
  wrmsr
  retq

// declare a block named stack with size of KSTACKSIZE
.comm stack, KSTACKSIZE
