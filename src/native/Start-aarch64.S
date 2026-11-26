/* Built once with Bolt. Linux/glibc ARM64 entry; x0 initially holds rtld_fini. */
.text
.globl _start
.type _start,%function
_start:
    mov x29,#0
    mov x30,#0
    mov x5,x0
    ldr x1,[sp]
    add x2,sp,#8
    mov x6,sp
    mov x3,#0
    mov x4,#0
    adrp x0,bolt_program_entry
    add x0,x0,:lo12:bolt_program_entry
    bl __libc_start_main
    brk #0
.size _start,.-_start
.section .note.GNU-stack,"",%progbits
