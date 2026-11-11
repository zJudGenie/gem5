/* Sspmpen and Smpmpdeleg extension CSRs, aliasing, dynamic partitioning, and lock rules. */
#include "spmp_harness.h"

#if __riscv_xlen == 64
static volatile uint64_t sv39_root[512]
    __attribute__((section(".pagetable"), aligned(4096)));
#endif

static void
test_enable_csr_rmw(void)
{
    const uintptr_t initial = read_spmpen();
    uintptr_t extra = (uintptr_t)1 << 31;
#if __riscv_xlen == 64
    extra |= (uintptr_t)1 << 63;
#else
    uintptr_t high;
    __asm__ volatile("csrr %0, 0x193" : "=r"(high));
#endif
    __asm__ volatile("csrrs x0, 0x183, %0" :: "r"(extra) : "memory");
    check(read_spmpen() == (initial | extra), "spmpen CSRRS sets high bits");
    __asm__ volatile("csrrs x0, 0x183, x0" ::: "memory");
    check(read_spmpen() == (initial | extra), "spmpen CSRRS rs1=x0 suppresses write");
    __asm__ volatile("csrrc x0, 0x183, %0" :: "r"(extra) : "memory");
    check(read_spmpen() == initial, "spmpen CSRRC clears high bits");
    __asm__ volatile("csrrw x0, 0x183, %0" :: "r"(initial) : "memory");
    check(read_spmpen() == initial, "spmpen CSRRW rd=x0 writes mask");
#if __riscv_xlen == 32
    uintptr_t after;
    __asm__ volatile("csrr %0, 0x193" : "=r"(after));
    check(after == high, "RV32 spmpen RMW preserves spmpenh");
#endif
}

#if __riscv_xlen == 64
static void
test_shared_paging_delegation(void)
{
    const uintptr_t target = (uintptr_t)&spmp_target;
    const uintptr_t old_sel = read_miselect();
    write_miselect(ENTRY(0));
    const uintptr_t old_addr = read_mireg();
    const uintptr_t old_cfg = read_mireg2();

    for (unsigned i = 0; i < 512; ++i)
        sv39_root[i] = 0;
    sv39_root[2] = (0x80000000UL >> 2) | 0xcf;
    __asm__ volatile("fence rw, rw; sfence.vma zero, zero" ::: "memory");
    __asm__ volatile("csrw 0x180, %0"
                     :: "r"((8UL << 60) | ((uintptr_t)sv39_root >> 12)) : "memory");
    __asm__ volatile("sfence.vma zero, zero" ::: "memory");

    write_mireg(PHYS_ADDR_MASK);
    write_mireg2(SPMP_A_NAPOT | SPMP_X);
    __asm__ volatile("csrw 0x316, zero" ::: "memory");
    fence_spmp();
    expect_mprv_ok(probe_m_lw, target, 0, PRV_S, MSTATUS_MPRV,
                   "Sv39 load succeeds when all PMP slots are delegated to SPMP");

    __asm__ volatile("csrw 0x316, %0" :: "r"((uintptr_t)1) : "memory");
    fence_spmp();
    expect_mprv_trap(probe_m_lw, target, 0, PRV_S, MSTATUS_MPRV,
                     CAUSE_LOAD_ACCESS, target,
                     "Reclaimed PMP[0] slot denies Sv39 page-table walk");

    __asm__ volatile("csrw 0x316, zero" ::: "memory");
    fence_spmp();
    expect_mprv_ok(probe_m_lw, target, 0, PRV_S, MSTATUS_MPRV,
                   "Re-delegating slot 0 restores Sv39 page-table access");

    __asm__ volatile("csrw 0x180, zero; sfence.vma zero, zero" ::: "memory");
    write_mireg(old_addr);
    write_mireg2(old_cfg);
    write_miselect(old_sel);
    fence_spmp();
}
#endif

static void
test_shared_delegation_and_enable(void)
{
    const uintptr_t target = (uintptr_t)&spmp_target;
    const uintptr_t data_addr = napot((uintptr_t)__spmp_data_start, 4096);
    const uintptr_t code_addr = napot((uintptr_t)__ucode_start, 4096);
    const uintptr_t s_code_addr = napot((uintptr_t)__scode_start, 4096);
    uintptr_t value;

    /* 1. mpmpdeleg clamping and D=0 hidden-entry behavior. */
    __asm__ volatile("csrw 0x316, %0" :: "r"(~(uintptr_t)0) : "memory");
    __asm__ volatile("csrr %0, 0x316" : "=r"(value));
    check(value == 64, "mpmpdeleg clamps pmpnum to 64 and clears WPRI bits");

    write_miselect(ENTRY(0));
    write_mireg(0x111);
    write_mireg2(0xff);
    check(read_mireg() == 0 && read_mireg2() == 0,
          "M aliases ignore writes when D=0 (pmpnum=64)");

    write_siselect(ENTRY(0));
    write_sireg(0x333);
    write_sireg2(0xff);
    check(read_sireg() == 0 && read_sireg2() == 0,
          "S aliases ignore writes when D=0 (pmpnum=64)");

    /* 2. Delegate all 64 entries (pmpnum=0) and check reserved mireg3..6 / invalid selector. */
    __asm__ volatile("csrw 0x316, %0" :: "r"((uintptr_t)0x80) : "memory");
    __asm__ volatile("csrr %0, 0x316" : "=r"(value));
    check(value == 0, "mpmpdeleg ignores WPRI bit 7");

    const unsigned reserved_csrs[] = {0x353, 0x355, 0x356, 0x357};
    for (unsigned i = 0; i < 4; ++i) {
        expect_trap(probe_m_csrr, reserved_csrs[i], 0, PRV_M, 0,
                    CAUSE_ILLEGAL, 0, "reserved mireg3..6 fault in M-mode");
    }
    write_miselect(0x140);
    expect_trap(probe_m_csrr, CSR_MIREG, 0, PRV_M, 0,
                CAUSE_ILLEGAL, 0, "unsupported miselect=0x140 faults on mireg");

    /* 3. Program entries via M bank, verify S bank aliasing and PMP isolation. */
    write_miselect(ENTRY(0));
    write_mireg(data_addr);
    write_mireg2(SPMP_A_NAPOT | SPMP_U | SPMP_R | SPMP_W);
    __asm__ volatile("csrw 0x3b0, %0" :: "r"((uintptr_t)0x9999) : "memory");
    __asm__ volatile("csrw 0x3a0, %0" :: "r"((uintptr_t)0x1f) : "memory");
    __asm__ volatile("csrr %0, 0x3b0" : "=r"(value));
    check(value == 0 && read_mireg() == data_addr,
          "PMPADDR0/PMPCFG0 writes are ignored when pmpnum=0");

    write_miselect(ENTRY(1));
    write_mireg(code_addr);
    write_mireg2(SPMP_A_NAPOT | SPMP_U | SPMP_X);
    write_miselect(ENTRY(2));
    write_mireg(s_code_addr);
    write_mireg2(SPMP_A_NAPOT | SPMP_R | SPMP_W | SPMP_X);
    write_miselect(ENTRY(32));
    write_mireg(data_addr);
    write_mireg2(0);

    write_siselect(ENTRY(0));
    check(read_sireg() == data_addr &&
          read_sireg2() == (SPMP_A_NAPOT | SPMP_U | SPMP_R | SPMP_W),
          "S indirect bank aliases M-programmed SPMP entry 0");
    check(read_miselect() == ENTRY(32), "M and S selectors remain independent");

    /* 4. Enable mask CSRs (spmpen / spmpenh) and memory access transitions. */
#if __riscv_xlen == 32
    write_spmpen(7);
    __asm__ volatile("csrw 0x193, %0" :: "r"((uintptr_t)1) : "memory");
    __asm__ volatile("csrr %0, 0x193" : "=r"(value));
    check(read_spmpen() == 7 && value == 1, "RV32 spmpen and spmpenh readback");
#else
    const uintptr_t enable_mask = ((uintptr_t)1 << 32) | 7;
    write_spmpen(enable_mask);
    check(read_spmpen() == enable_mask, "RV64 spmpen bits 0..2 and 32 readback");
    expect_trap(probe_m_csrr, CSR_SPMPENH, 0, PRV_M, 0,
                CAUSE_ILLEGAL, 0, "RV64 spmpenh is illegal instruction");
#endif
    test_enable_csr_rmw();
    fence_spmp();

    expect_trap(probe_s_csrr_miselect, 0, 0, PRV_S, 0,
                CAUSE_ILLEGAL, 0, "S-mode cannot access miselect");
    expect_trap(probe_u_csrr_spmpen, 0, 0, PRV_U, 0,
                CAUSE_ILLEGAL, 0, "U-mode cannot access spmpen");

    spmp_target = 0x51a7c0de;
    expect_ok(probe_u_lw, target, 0, PRV_U, 0,
              "enabled SPMP[0] permits U load");

    /* Disable bit 0 -> U load denied -> re-enable bit 0 -> U load allowed. */
#if __riscv_xlen == 32
    write_spmpen(6);
#else
    write_spmpen(((uintptr_t)1 << 32) | 6);
#endif
    fence_spmp();
    expect_trap(probe_u_lw, target, 0, PRV_U, 0,
                CAUSE_LOAD_PAGE, target, "clearing spmpen bit 0 denies U load");

#if __riscv_xlen == 32
    write_spmpen(7);
#else
    write_spmpen(((uintptr_t)1 << 32) | 7);
#endif
    fence_spmp();
    expect_ok(probe_u_lw, target, 0, PRV_U, 0,
              "restoring spmpen bit 0 re-enables U load");

#if __riscv_xlen == 64
    test_shared_paging_delegation();
#endif

    /* 5. Lock clearing rules: only M bank (mireg / mireg2) can modify locked entries. */
    write_miselect(ENTRY(0));
    write_mireg2(SPMP_A_NAPOT | SPMP_LOCK | SPMP_U | SPMP_R | SPMP_W);
    expect_ok(probe_s_clear_lock, ENTRY(0), 0, PRV_S, 0,
              "S-mode write to locked sireg2 does not trap");
    write_siselect(ENTRY(0));
    check((read_sireg2() & SPMP_LOCK) != 0, "S-mode cannot clear SPMP_LOCK");
    write_sireg(0);
    write_sireg2(0);
    check(read_sireg() == data_addr && (read_sireg2() & SPMP_LOCK) != 0,
          "M-mode via S bank cannot modify locked SPMP entry");

    /* Locked entry blocks enable-bit modification in spmpen. */
    expect_ok(probe_s_write_spmpen, 6, 0, PRV_S, 0,
              "S-mode can write spmpen");
    check((read_spmpen() & 1) == 1, "locked SPMP[0] preserves spmpen bit 0");

    /* M-bank write via mireg/mireg2 can update locked address and clear L with new config. */
    write_miselect(ENTRY(0));
    write_mireg(data_addr + 2);
    check(read_mireg() == data_addr + 2,
          "M-mode write through mireg updates locked SPMP address");
    write_mireg(data_addr);
    write_mireg2(SPMP_A_NAPOT | SPMP_U | SPMP_R);
    check(read_mireg2() == (SPMP_A_NAPOT | SPMP_U | SPMP_R),
          "M-mode write through mireg2 clears SPMP_LOCK and updates config");
    write_mireg2(SPMP_A_NAPOT | SPMP_U | SPMP_R | SPMP_W);

    /* 6. Dynamic partition resize & locked PMP prefix guard. */
    __asm__ volatile("csrw 0x316, %0" :: "r"((uintptr_t)62) : "memory");
    write_pmp_baseline();
    /* Lock PMP[1] and verify shrinking pmpnum below 2 is rejected. */
    __asm__ volatile("csrw 0x3a0, %0" :: "r"((uintptr_t)0x9f1f) : "memory");
    __asm__ volatile("csrw 0x316, %0" :: "r"((uintptr_t)1) : "memory");
    __asm__ volatile("csrr %0, 0x316" : "=r"(value));
    check(value == 62, "locked PMP entry rejects mpmpdeleg shrink across it");

    /* Verify spmpen bits remain bound to physical slots across mpmpdeleg changes. */
    write_miselect(ENTRY(0)); /* physical slot 62 */
    write_mireg2(0);
    write_miselect(ENTRY(1)); /* physical slot 63 */
    write_mireg2(0);
    write_spmpen(2); /* enable logical SPMP[1] = physical slot 63 */
    check(read_spmpen() == 2, "spmpen bit 1 set at pmpnum=62");
    __asm__ volatile("csrw 0x316, %0" :: "r"((uintptr_t)63) : "memory");
    check(read_spmpen() == 1,
          "physical slot 63 enable bit shifts to logical bit 0 at pmpnum=63");
    __asm__ volatile("csrw 0x316, %0" :: "r"((uintptr_t)62) : "memory");
    check(read_spmpen() == 2,
          "physical slot 63 enable bit shifts back to logical bit 1 at pmpnum=62");

    /* Prepare phase-2 state: pmpnum=62, SPMP[0]=data, SPMP[1]=ucode, spmpen=3. */
    write_miselect(ENTRY(0));
    write_mireg(data_addr);
    write_mireg2(SPMP_A_NAPOT | SPMP_U | SPMP_R | SPMP_W);
    write_miselect(ENTRY(1));
    write_mireg(code_addr);
    write_mireg2(SPMP_A_NAPOT | SPMP_U | SPMP_X);
    write_siselect(ENTRY(0));
    write_miselect(ENTRY(0));
    write_spmpen(3);
    fence_spmp();
    expect_ok(probe_u_lw, target, 0, PRV_U, 0, "phase-1 delegated SPMP permits U load");
}

static void
verify_shared_phase2(void)
{
    const uintptr_t target = (uintptr_t)&spmp_target;
    uintptr_t pmpnum;
    __asm__ volatile("csrr %0, 0x316" : "=r"(pmpnum));
    check(pmpnum == 62, "phase-2 retains mpmpdeleg=62");
    check(read_siselect() == ENTRY(0) &&
          read_mireg() == napot((uintptr_t)__spmp_data_start, 4096) &&
          read_spmpen() == 3,
          "phase-2 retains selectors, delegated entry, and spmpen");
    expect_ok(probe_u_lw, target, 0, PRV_U, 0, "phase-2 delegated SPMP permits U load");
}

static void
test_standalone_spmpen(void)
{
    const uintptr_t target = (uintptr_t)&spmp_target;
    const uintptr_t data_addr = napot((uintptr_t)__spmp_data_start, 4096);

    write_entry(0, data_addr, SPMP_A_NAPOT | SPMP_U | SPMP_R | SPMP_W);
    write_entry(1, napot((uintptr_t)__ucode_start, 4096),
                SPMP_A_NAPOT | SPMP_U | SPMP_X);
    write_entry(2, napot((uintptr_t)__scode_start, 4096),
                SPMP_A_NAPOT | SPMP_R | SPMP_W | SPMP_X);
#if __riscv_xlen == 32
    write_spmpen(7);
    __asm__ volatile("csrw 0x193, %0" :: "r"((uintptr_t)1) : "memory");
#else
    write_spmpen(((uintptr_t)1 << 32) | 7);
#endif
    test_enable_csr_rmw();
    fence_spmp();

    expect_ok(probe_u_lw, target, 0, PRV_U, 0, "standalone spmpen permits U load");
    write_spmpen(6);
    fence_spmp();
    expect_trap(probe_u_lw, target, 0, PRV_U, 0,
                CAUSE_LOAD_PAGE, target, "standalone clearing spmpen bit 0 denies U load");
    write_spmpen(7);
    fence_spmp();
    expect_ok(probe_u_lw, target, 0, PRV_U, 0, "standalone restoring spmpen bit 0 permits U load");
}

static void
test_delegation_only(void)
{
    const uintptr_t target = (uintptr_t)&spmp_target;
    uintptr_t pmpnum;
    __asm__ volatile("csrw 0x316, %0" :: "r"((uintptr_t)48) : "memory");
    __asm__ volatile("csrr %0, 0x316" : "=r"(pmpnum));
    check(pmpnum == 48, "delegation-only sets pmpnum=48 (16 SPMP entries)");

    set_test_rules(SPMP_R | SPMP_W);
    expect_ok(probe_u_lw, target, 0, PRV_U, 0, "delegation-only permits U load");
    set_test_rules(0);
    expect_trap(probe_u_lw, target, 0, PRV_U, 0,
                CAUSE_LOAD_PAGE, target, "delegation-only denies U load when R=0");
    set_test_rules(SPMP_R | SPMP_W);
}

void
spmp_test_main(void)
{
    struct spmp_hw hw;
    write_pmp_baseline();
    spmp_discover_hw(&hw);
    check(hw.enabled && (hw.has_spmpen || hw.has_delegation),
          "test_extensions requires Sspmpen or Smpmpdeleg enabled");

    if (hw.has_delegation && hw.has_spmpen) {
        test_shared_delegation_and_enable();
        spmp_finish_phase1_and_exit();
        verify_shared_phase2();
    } else if (hw.has_spmpen) {
        test_standalone_spmpen();
        spmp_finish_phase1_and_exit();
        expect_ok(probe_u_lw, (uintptr_t)&spmp_target, 0, PRV_U, 0,
                  "phase-2 standalone spmpen retains U load permission");
    } else {
        test_delegation_only();
        spmp_finish_phase1_and_exit();
        uintptr_t pmpnum;
        __asm__ volatile("csrr %0, 0x316" : "=r"(pmpnum));
        check(pmpnum == 48, "phase-2 delegation-only retains pmpnum=48");
        expect_ok(probe_u_lw, (uintptr_t)&spmp_target, 0, PRV_U, 0,
                  "phase-2 delegation-only retains U load permission");
    }
    spmp_pass_and_exit();
}
