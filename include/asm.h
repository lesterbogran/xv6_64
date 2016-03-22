//
// assembler macros to create x86 segments
// note: byte = 8bit, word = 16bit, long = 32bit, quad = 64bit

#define SEG_NULLASM                                             \
        .word 0, 0;                                             \
        .byte 0, 0, 0, 0

// The 0x90 means the segment is present and not system
// The 0xC0 means the limit is in 4096-byte units (G = 1, D = 1)
// and (for executable segments) 32-bit mode.
#define SEG_ASM(type,base,lim)                                  \
        .word (((lim) >> 12) & 0xffff), ((base) & 0xffff);      \
        .byte (((base) >> 16) & 0xff), (0x90 | (type)),         \
                (0xC0 | (((lim) >> 28) & 0xf)), (((base) >> 24) & 0xff)

// If L-bit is set, then D-bit must be 0
// So C0 changes to A0(D = 0, L = 1, G remains)
#define SEG_ASM_64(type,base,lim)                               \
        .word (((lim) >> 12) & 0xffff), ((base) & 0xffff);      \
        .byte (((base) >> 16) & 0xff), (0x90 | (type)),         \
                (0xA0 | (((lim) >> 28) & 0xf)), (((base) >> 24) & 0xff)

#define STA_X     0x8       // Executable segment
#define STA_E     0x4       // Expand down (non-executable segments)
#define STA_C     0x4       // Conforming code segment (executable only)
#define STA_W     0x2       // Writeable (non-executable segments)
#define STA_R     0x2       // Readable (executable segments)
#define STA_A     0x1       // Accessed

#define EFER      0xc0000080// ID of EFER
#define EFER_LME  0x100      // Long mode
