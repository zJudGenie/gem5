/* RISC-V SPMP boot entry, generic privilege trampoline, and M-mode trap recovery. */
#if __riscv_xlen == 64
#define REG_S sd
#define REG_L ld
#define XLENBYTES 8
#else
#define REG_S sw
#define REG_L lw
#define XLENBYTES 4
#endif

    .section .text.boot,"ax",@progbits
    .globl _start
_start:
    la      sp, _stack_top
    csrw    medeleg, zero
    csrw    mideleg, zero
    csrw    mie, zero
    la      t0, m_trap
    csrw    mtvec, t0
    la      t0, __bss_start
    la      t1, __bss_end
0:
    bgeu    t0, t1, 1f
    sb      zero, 0(t0)
    addi    t0, t0, 1
    j       0b
1:
    tail    spmp_test_main

/*
 * void run_probe(uintptr_t fn, uintptr_t status, uintptr_t direct_m,
 *                uintptr_t arg0, uintptr_t arg1);
 * Saves the M-mode frame and SP in spmp_saved_sp, forwards (arg0, arg1) in
 * (a0, a1), and enters fn either directly in M-mode (direct_m != 0) or via mret.
 */
    .section .text.mprobe,"ax",@progbits
    .align  2
    .globl run_probe
run_probe:
    addi    sp, sp, -112
    REG_S   ra, 0(sp)
    REG_S   s0, 8(sp)
    REG_S   s1, 16(sp)
    REG_S   s2, 24(sp)
    REG_S   s3, 32(sp)
    REG_S   s4, 40(sp)
    REG_S   s5, 48(sp)
    REG_S   s6, 56(sp)
    REG_S   s7, 64(sp)
    REG_S   s8, 72(sp)
    REG_S   s9, 80(sp)
    REG_S   s10, 88(sp)
    REG_S   s11, 96(sp)
    csrr    t0, mstatus
    REG_S   t0, 104(sp)
    la      t0, spmp_saved_sp
    REG_S   sp, 0(t0)

    mv      t0, a0
    mv      t1, a1
    mv      t2, a2
    mv      a0, a3
    mv      a1, a4

    beqz    t2, 1f
    csrw    mstatus, t1
    jr      t0
1:
    csrw    mepc, t0
    csrw    mstatus, t1
    mret

    .globl probe_restore
probe_restore:
    la      t0, spmp_saved_sp
    REG_L   sp, 0(t0)
    REG_L   t0, 104(sp)
    csrw    mstatus, t0
    REG_L   ra, 0(sp)
    REG_L   s0, 8(sp)
    REG_L   s1, 16(sp)
    REG_L   s2, 24(sp)
    REG_L   s3, 32(sp)
    REG_L   s4, 40(sp)
    REG_L   s5, 48(sp)
    REG_L   s6, 56(sp)
    REG_L   s7, 64(sp)
    REG_L   s8, 72(sp)
    REG_L   s9, 80(sp)
    REG_L   s10, 88(sp)
    REG_L   s11, 96(sp)
    addi    sp, sp, 112
    ret

/* Trap handler records {mcause, mtval, mepc} and returns to probe_restore in M-mode. */
    .section .text.trap,"ax",@progbits
    .align  2
    .globl m_trap
m_trap:
    li      t4, (1 << 17)
    csrc    mstatus, t4
    csrr    t0, mcause
    csrr    t1, mtval
    csrr    t2, mepc

    la      t3, spmp_trap_cause
    REG_S   t0, 0(t3)
    la      t3, spmp_trap_tval
    REG_S   t1, 0(t3)
    la      t3, spmp_trap_epc
    REG_S   t2, 0(t3)

    la      t3, spmp_probe_active
    REG_L   t4, 0(t3)
    beqz    t4, unexpected_trap

    la      t3, spmp_trap_seen
    REG_L   t4, 0(t3)
    addi    t4, t4, 1
    REG_S   t4, 0(t3)

    la      t4, probe_restore
    csrw    mepc, t4
    li      t5, (3 << 11)
    csrs    mstatus, t5
    mret

unexpected_trap:
    li      a0, 0
#if __riscv_xlen == 32
    li      a1, 0
    li      a2, 1
    li      a3, 0
#else
    li      a1, 1
#endif
    .word   0x4400007b
1:
    j       1b

    .section .note.GNU-stack,"",@progbits
