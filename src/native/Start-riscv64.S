/* Linux RV64GC, glibc lp64d process entry. The loader supplies rtld_fini in a0. */
.text
.globl _start
.type _start,@function
_start:
    .option push
    .option norelax
    lla gp,__global_pointer$
    .option pop
    mv s0,zero
    mv a5,a0
    ld a1,0(sp)
    addi a2,sp,8
    andi sp,sp,-16
    mv a6,sp
    mv a3,zero
    mv a4,zero
    lla a0,bolt_program_entry
    call __libc_start_main
    ebreak
.size _start,.-_start
.section .note.GNU-stack,"",@progbits
