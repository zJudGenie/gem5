/* RISC-V SPMP workload entry, probe trampolines, and precise M-mode recovery. */
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

/* Preserve the M stack; probes never change SP. a2 selects direct M execution. */
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
    beqz    a2, 1f
    csrw    mstatus, a1
    jr      a0
1:
    csrw    mepc, a0
    csrw    mstatus, a1
    mret
    .globl probe_restore
probe_restore:
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

/* Trap handler validates the first exception, then always returns to M code. */
    .section .text.trap,"ax",@progbits
    .align  2
m_trap:
    li      t4, (1 << 17)
    csrc    mstatus, t4
    csrr    t0, mcause
    csrr    t1, mtval
    csrr    t2, mepc
    la      t3, spmp_observed
    REG_S   t0, 0(t3)
    REG_S   t1, XLENBYTES(t3)
    REG_S   t2, (2*XLENBYTES)(t3)
    la      t4, spmp_case_id
    REG_L   t5, 0(t4)
    REG_S   t5, (3*XLENBYTES)(t3)
    la      t4, spmp_active
    REG_L   t5, 0(t4)
    beqz    t5, unexpected_trap
    la      t4, spmp_seen
    REG_L   t5, 0(t4)
    bnez    t5, 1f
    la      t4, spmp_expected_cause
    REG_L   t5, 0(t4)
    bne     t0, t5, 2f
    la      t4, spmp_expected_tval
    REG_L   t5, 0(t4)
    beq     t1, t5, 4f
    la      t4, spmp_expected_tval_alt
    REG_L   t5, 0(t4)
    beqz    t5, 2f
    bne     t1, t5, 2f
4:
    la      t4, spmp_expected_pc
    REG_L   t5, 0(t4)
    bne     t2, t5, 2f
    li      t5, 1
    la      t4, spmp_seen
    REG_S   t5, 0(t4)
    j       3f
1:
2:
    li      t5, 1
    la      t4, spmp_failure
    REG_S   t5, 0(t4)
3:
    la      t4, probe_restore
    csrw    mepc, t4
    li      t5, (3 << 11)
    csrc    mstatus, t5
    csrs    mstatus, t5
    mret

unexpected_trap:
    /* There is no saved probe frame; retain the log and fail immediately. */
    la      t4, spmp_failure
    li      t5, 1
    REG_S   t5, 0(t4)
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

/* S probes execute outside the U-only code page. */
    .section .scode,"ax",@progbits
    .align  12
    .macro LOAD_PROBE name
    .balign 64
    .globl \name
\name:
    la t0, spmp_target
    .globl \name\()_fault
\name\()_fault:
    lw t1, 0(t0)
    .globl \name\()_resume
\name\()_resume:
    ecall
    .endm
    .macro STORE_PROBE name
    .balign 64
    .globl \name
\name:
    la t0, spmp_target
    li t1, 0x13579bdf
    .globl \name\()_fault
\name\()_fault:
    sw t1, 0(t0)
    .globl \name\()_resume
\name\()_resume:
    ecall
    .endm
    .macro LR_PROBE name
    .balign 64
    .globl \name
\name:
    la t0, spmp_target
    .globl \name\()_fault
\name\()_fault:
    lr.w t1, (t0)
    .globl \name\()_resume
\name\()_resume:
    ecall
    .endm
    .macro SC_PROBE name
    .balign 64
    .globl \name
\name:
    la t0, spmp_target
    li t1, 0x2468ace0
    .globl \name\()_fault
\name\()_fault:
    sc.w t2, t1, (t0)
    .globl \name\()_resume
\name\()_resume:
    ecall
    .endm
    .macro AMO_PROBE name
    .balign 64
    .globl \name
\name:
    la t0, spmp_target
    li t1, 1
    .globl \name\()_fault
\name\()_fault:
    amoadd.w t2, t1, (t0)
    .globl \name\()_resume
\name\()_resume:
    ecall
    .endm
    .macro LRSC_PROBE name
    .balign 64
    .globl \name
\name:
    la t0, spmp_target
    lr.w t1, (t0)
    li t2, 0x13579bdf
    .globl \name\()_fault
\name\()_fault:
    sc.w t3, t2, (t0)
    la t0, spmp_sc_status
    sw t3, 0(t0)
    .globl \name\()_resume
\name\()_resume:
    ecall
    .endm
    .macro OFFSET_LOAD_PROBE name, offset
    .balign 64
    .globl \name
\name:
    la t0, spmp_target
    addi t0, t0, \offset
    .globl \name\()_fault
\name\()_fault:
    lhu t1, 0(t0)
    .globl \name\()_resume
\name\()_resume:
    ecall
    .endm
    .macro BYTE_LOAD_PROBE name, offset
    .balign 64
    .globl \name
\name:
    la t0, spmp_target
    addi t0, t0, \offset
    .globl \name\()_fault
\name\()_fault:
    lbu t1, 0(t0)
    .globl \name\()_resume
\name\()_resume:
    ecall
    .endm
LOAD_PROBE probe_s_load
STORE_PROBE probe_s_store
LR_PROBE probe_s_lr
SC_PROBE probe_s_sc
AMO_PROBE probe_s_amo

/* User code has its own executable page; targets occupy a separate page. */
    .section .ucode,"ax",@progbits
    .align  12
LOAD_PROBE probe_u_load
STORE_PROBE probe_u_store
LR_PROBE probe_u_lr
SC_PROBE probe_u_sc
AMO_PROBE probe_u_amo
LRSC_PROBE probe_u_lrsc
OFFSET_LOAD_PROBE probe_u_tor_cross, 7
BYTE_LOAD_PROBE probe_u_tor_last, 7
OFFSET_LOAD_PROBE probe_u_tor_after, 8
OFFSET_LOAD_PROBE probe_u_na4_cross, 3
    .balign 64
    .globl probe_u_locked_load
probe_u_locked_load:
    la t0, spmp_locked_target
    .globl probe_u_locked_load_fault
probe_u_locked_load_fault:
    lw t1, 0(t0)
    .globl probe_u_locked_load_resume
probe_u_locked_load_resume:
    ecall
    .balign 64
    .globl probe_u_locked_store
probe_u_locked_store:
    la t0, spmp_locked_target
    li t1, 0x2468ace0
    .globl probe_u_locked_store_fault
probe_u_locked_store_fault:
    sw t1, 0(t0)
    .globl probe_u_locked_store_resume
probe_u_locked_store_resume:
    ecall
/* U-mode attempts to read a supervisor CSR. */
    .balign 64
    .globl probe_u_csr
probe_u_csr:
    .globl probe_u_csr_fault
probe_u_csr_fault:
    csrr t0, 0x150
    .globl probe_u_csr_resume
probe_u_csr_resume:
    ecall

/* This store crosses the configured 64-byte cache-line boundary. */
    .balign 64
    .globl probe_u_split_store
probe_u_split_store:
    la t0, spmp_split
    addi t0, t0, 2
    li t1, 0x11223344
    .globl probe_u_split_store_fault
probe_u_split_store_fault:
    sw t1, 0(t0)
    ecall

/* Execute-only shared rules allow U fetch but deny a load from the same page. */
    .section .spmp_target_code,"ax",@progbits
    .globl probe_u_exec_page
probe_u_exec_page:
    .globl probe_u_exec_page_resume
probe_u_exec_page_resume:
    ecall
    .globl probe_u_target_load
probe_u_target_load:
    la t0, spmp_target
    .globl probe_u_target_load_fault
probe_u_target_load_fault:
    lw t1, 0(t0)
    .globl probe_u_target_load_resume
probe_u_target_load_resume:
    ecall

/* An M-mode fetch remains privileged while MPRV changes data privilege. */
    .section .text.mprobe,"ax",@progbits
LOAD_PROBE probe_m_load

    .globl probe_invalid_alias
probe_invalid_alias:
    .globl probe_invalid_alias_fault
probe_invalid_alias_fault:
    csrr t0, 0x151
    .globl probe_invalid_alias_resume
probe_invalid_alias_resume:
    ecall
    .globl probe_reserved_alias
probe_reserved_alias:
    .globl probe_reserved_alias_fault
probe_reserved_alias_fault:
    csrr t0, 0x153
    .globl probe_reserved_alias_resume
probe_reserved_alias_resume:
    ecall

    .section .note.GNU-stack,"",@progbits
