/* SFENCE.VMA reconfiguration, SPMP-vs-PMP fault priority, and Bare/Sv39 paging tests. */
#include "spmp_harness.h"

#if __riscv_xlen == 64
static volatile uint64_t sv39_root[512]
    __attribute__((section(".pagetable"), aligned(4096)));
#endif

static void
test_fences(void)
{
    const uintptr_t target = (uintptr_t)&spmp_target;

    set_test_rules(SPMP_R);
    expect_ok(probe_u_lw, target, 0, PRV_U, 0,
              "fence warm data and instruction caches");

    /* Data allow -> deny -> allow with SFENCE.VMA. */
    write_siselect(ENTRY(1));
    write_sireg2(SPMP_A_NAPOT | SPMP_U);
    fence_spmp();
    expect_trap(probe_u_lw, target, 0, PRV_U, 0,
                CAUSE_LOAD_PAGE, target, "fence data allow-to-deny");

    write_sireg2(SPMP_A_NAPOT | SPMP_U | SPMP_R);
    fence_spmp();
    expect_ok(probe_u_lw, target, 0, PRV_U, 0, "fence data deny-to-allow");

    /* Instruction fetch allow -> deny -> allow with SFENCE.VMA. */
    write_siselect(ENTRY(0));
    write_sireg2(SPMP_A_NAPOT | SPMP_U);
    fence_spmp();
    expect_trap(probe_u_lw, target, 0, PRV_U, 0,
                CAUSE_INST_PAGE, (uintptr_t)probe_u_lw,
                "fence fetch allow-to-deny");

    write_sireg2(SPMP_A_NAPOT | SPMP_U | SPMP_X);
    fence_spmp();
    expect_ok(probe_u_lw, target, 0, PRV_U, 0, "fence fetch deny-to-allow");
}

static void
test_fault_priority(void)
{
    const uintptr_t target = (uintptr_t)&spmp_target;

    /* When both SPMP and PMP deny an access, SPMP page fault takes priority. */
    set_test_rules(0);
    pmp_deny_page((uintptr_t)__spmp_data_start);
    expect_trap(probe_u_lw, target, 0, PRV_U, 0,
                CAUSE_LOAD_PAGE, target,
                "priority: SPMP load page fault (13) precedes PMP access fault (5)");
    expect_trap(probe_u_sw, target, 0x13579bdf, PRV_U, 0,
                CAUSE_STORE_PAGE, target,
                "priority: SPMP store page fault (15) precedes PMP access fault (7)");
    write_pmp_baseline();

    write_siselect(ENTRY(0));
    write_sireg2(SPMP_A_NAPOT | SPMP_U);
    fence_spmp();
    pmp_deny_page((uintptr_t)__ucode_start);
    expect_trap(probe_u_lw, target, 0, PRV_U, 0,
                CAUSE_INST_PAGE, (uintptr_t)probe_u_lw,
                "priority: SPMP fetch page fault (12) precedes PMP access fault (1)");
    write_pmp_baseline();
}

static void
test_paging(void)
{
    const uintptr_t target = (uintptr_t)&spmp_target;

#if __riscv_xlen == 32
    /* On RV32, SATP bit 31 (Sv32) is WARL 0 (Bare) and cannot disable SPMP. */
    set_test_rules(0);
    uintptr_t satp = 0x80000000UL;
    __asm__ volatile("csrw 0x180, %0" :: "r"(satp) : "memory");
    __asm__ volatile("csrr %0, 0x180" : "=r"(satp));
    check(satp == 0, "enabled RV32 SATP remains Bare (0)");
    expect_trap(probe_s_lw, target, 0, PRV_S, 0,
                CAUSE_LOAD_PAGE, target,
                "RV32 attempted Sv32 write keeps SPMP active in Bare mode");
#else
    /* On RV64, enabling Sv39 bypasses SPMP while keeping PMP and PTE checks active. */
    set_test_rules(0);
    for (unsigned i = 0; i < 512; ++i)
        sv39_root[i] = 0;
    /* S-only RWX 1 GiB identity leaf at root[2] (0x80000000..0xbfffffff). */
    sv39_root[2] = (0x80000000UL >> 2) | 0xcf;
    __asm__ volatile("fence rw, rw" ::: "memory");
    const uintptr_t satp = (8UL << 60) | ((uintptr_t)sv39_root >> 12);
    __asm__ volatile("csrw 0x180, %0" :: "r"(satp) : "memory");
    fence_spmp();

    for (unsigned i = 0; i < 2; ++i) {
        expect_ok(probe_s_lw, target, 0, PRV_S, 0,
                  "Sv39 TLB miss and hit bypass restrictive SPMP");
    }
    fence_spmp();
    expect_ok(probe_s_lw, target, 0, PRV_S, 0,
              "Sv39 table walk after SFENCE.VMA bypasses restrictive SPMP");

    pmp_deny_page((uintptr_t)__spmp_data_start);
    expect_trap(probe_s_lw, target, 0, PRV_S, 0,
                CAUSE_LOAD_ACCESS, target,
                "PMP remains active when Sv39 paging is enabled");
    write_pmp_baseline();

    sv39_root[2] = (0x80000000UL >> 2) | 0xcd; /* Reserved W=1, R=0 PTE */
    __asm__ volatile("fence rw, rw" ::: "memory");
    fence_spmp();
    expect_trap(probe_s_lw, target, 0, PRV_S, 0,
                CAUSE_INST_PAGE, (uintptr_t)probe_s_lw,
                "Invalid Sv39 PTE raises instruction page fault");

    __asm__ volatile("csrw 0x180, zero" ::: "memory");
    fence_spmp();
    expect_trap(probe_s_lw, target, 0, PRV_S, 0,
                CAUSE_LOAD_PAGE, target,
                "Returning SATP to Bare reactivates SPMP");
#endif
}

void
spmp_test_main(void)
{
    struct spmp_hw hw;
    write_pmp_baseline();
    spmp_discover_hw(&hw);
    check(hw.enabled && hw.count >= 3, "test_paging_fences requires >= 3 SPMP entries");

    test_fences();
    test_fault_priority();
    test_paging();

    spmp_finish_phase1_and_exit();
    spmp_pass_and_exit();
}
