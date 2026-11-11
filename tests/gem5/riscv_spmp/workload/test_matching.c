/* Address matching modes (OFF, TOR, NA4, NAPOT), priority, and split-store tests. */
#include "spmp_harness.h"

static void
test_empty_table_default_deny(unsigned count)
{
    clear_all_spmp_entries(count);
    expect_trap(probe_u_lw, (uintptr_t)&spmp_target, 0, PRV_U, 0,
                CAUSE_INST_PAGE, (uintptr_t)probe_u_lw,
                "empty active SPMP table denies U instruction fetch");
    expect_trap(probe_s_lw, (uintptr_t)&spmp_target, 0, PRV_S, 0,
                CAUSE_INST_PAGE, (uintptr_t)probe_s_lw,
                "empty active SPMP table denies S instruction fetch");
}

static void
test_tor_and_na4_boundaries(unsigned count)
{
    const uintptr_t target = (uintptr_t)&spmp_target;

    /* TOR entry 0 uses 0 as implicit lower bound. */
    clear_all_spmp_entries(count);
    write_entry(0, (target + 4) >> 2, SPMP_A_TOR | SPMP_U | SPMP_R | SPMP_X);
    fence_spmp();
    expect_ok(probe_u_lw, target, 0, PRV_U, 0,
              "TOR entry 0 covers [0, target+4)");
    expect_trap(probe_u_lw, target + 4, 0, PRV_U, 0,
                CAUSE_LOAD_PAGE, target + 4,
                "TOR entry 0 excludes upper bound");

    /* Non-zero TOR [target, target+8) boundary and straddling checks. */
    clear_all_spmp_entries(count);
    write_entry(0, target >> 2, SPMP_A_OFF);
    write_entry(1, (target + 8) >> 2, SPMP_A_TOR | SPMP_U | SPMP_R);
    write_entry(2, napot((uintptr_t)__ucode_start, 4096),
                SPMP_A_NAPOT | SPMP_U | SPMP_X);
    fence_spmp();

    expect_ok(probe_u_lw, target, 0, PRV_U, 0,
              "TOR lower bound permits 4-byte load");
    expect_ok(probe_u_lbu, target + 7, 0, PRV_U, 0,
              "TOR last byte (+7) is included");
    expect_trap(probe_u_lhu, target + 7, 0, PRV_U, 0,
                CAUSE_LOAD_PAGE, target + 7,
                "TOR partial overlap at +7 denies 2-byte load");
    expect_trap(probe_u_lhu, target + 8, 0, PRV_U, 0,
                CAUSE_LOAD_PAGE, target + 8,
                "TOR upper bound (+8) is excluded");

    /* Equal and reversed TOR bounds produce an empty range. */
    write_entry(1, target >> 2, SPMP_A_TOR | SPMP_U | SPMP_R);
    fence_spmp();
    expect_trap(probe_u_lbu, target, 0, PRV_U, 0,
                CAUSE_LOAD_PAGE, target,
                "TOR equal bounds [target, target) denies access");

    write_entry(0, (target + 8) >> 2, SPMP_A_OFF);
    write_entry(1, target >> 2, SPMP_A_TOR | SPMP_U | SPMP_R);
    fence_spmp();
    expect_trap(probe_u_lbu, target, 0, PRV_U, 0,
                CAUSE_LOAD_PAGE, target,
                "TOR reversed bounds denies access");

    /* NA4 exact 4-byte region [target, target+4) and straddling halfword. */
    set_test_rules(SPMP_R);
    write_entry(1, target >> 2, SPMP_A_NA4 | SPMP_U | SPMP_R);
    fence_spmp();
    expect_ok(probe_u_lw, target, 0, PRV_U, 0,
              "NA4 permits exact 4-byte load");
    expect_trap(probe_u_lhu, target + 3, 0, PRV_U, 0,
                CAUSE_LOAD_PAGE, target + 3,
                "NA4 partial overlap at +3 denies 2-byte load");
}

static void
test_priority_and_adjacent_entries(unsigned count)
{
    const uintptr_t target = (uintptr_t)&spmp_target;

    /* Lowest-numbered matching entry wins even when it denies access. */
    clear_all_spmp_entries(count);
    write_entry(0, napot((uintptr_t)__ucode_start, 4096),
                SPMP_A_NAPOT | SPMP_U | SPMP_X);
    write_entry(1, target >> 2, SPMP_A_NA4 | SPMP_U); /* matching but no R/W/X */
    write_entry(2, napot((uintptr_t)__spmp_data_start, 4096),
                SPMP_A_NAPOT | SPMP_U | SPMP_R | SPMP_W);
    fence_spmp();
    expect_trap(probe_u_lw, target, 0, PRV_U, 0,
                CAUSE_LOAD_PAGE, target,
                "lowest-index matching entry takes priority and denies load");

    /* Adjacent NA4 entries [target, target+4) and [target+4, target+8) do not combine. */
    clear_all_spmp_entries(count);
    write_entry(0, napot((uintptr_t)__ucode_start, 4096),
                SPMP_A_NAPOT | SPMP_U | SPMP_X);
    write_entry(1, target >> 2, SPMP_A_NA4 | SPMP_U | SPMP_R);
    write_entry(2, (target + 4) >> 2, SPMP_A_NA4 | SPMP_U | SPMP_R);
    fence_spmp();
    expect_ok(probe_u_lw, target, 0, PRV_U, 0,
              "first adjacent NA4 entry permits its own 4-byte load");
    expect_ok(probe_u_lw, target + 4, 0, PRV_U, 0,
              "second adjacent NA4 entry permits its own 4-byte load");
    expect_trap(probe_u_lw, target + 2, 0, PRV_U, 0,
                CAUSE_LOAD_PAGE, target + 2,
                "spanning load across two adjacent NA4 entries is denied");
}

static void
test_split_store(unsigned count)
{
    if (count < 4)
        return;
    check((uintptr_t)spmp_split == 0x8000603cUL, "split buffer linker address");
    for (unsigned i = 0; i < 8; ++i)
        spmp_split[i] = 0xaa;

    clear_all_spmp_entries(count);
    write_entry(0, napot((uintptr_t)__ucode_start, 4096),
                SPMP_A_NAPOT | SPMP_U | SPMP_X);
    write_entry(1, 0x80006040UL >> 2, SPMP_A_NA4 | SPMP_U | SPMP_R);
    write_entry(2, napot((uintptr_t)__spmp_data_start, 4096),
                SPMP_A_NAPOT | SPMP_U | SPMP_R | SPMP_W);
    write_entry(3, PHYS_ADDR_MASK, SPMP_A_NAPOT | SPMP_R | SPMP_W | SPMP_X);
    fence_spmp();

    /* Store 4 bytes at 0x8000603e crossing the 64-byte line at 0x80006040. */
    ++spmp_case_id;
    spmp_trap_seen = 0;
    uintptr_t status = read_mstatus() &
        ~(MSTATUS_MPP_MASK | MSTATUS_MPRV | MSTATUS_SUM | MSTATUS_MXR);
    spmp_probe_active = 1;
    run_probe((uintptr_t)probe_u_sw, status, 0, 0x8000603eUL, 0x11223344UL);
    spmp_probe_active = 0;

    check(spmp_trap_seen == 1 && spmp_trap_cause == CAUSE_STORE_PAGE &&
          (spmp_trap_tval == 0x8000603eUL || spmp_trap_tval == 0x80006040UL),
          "split store raises precise store page fault");
    check(spmp_split[0] == 0xaa && spmp_split[1] == 0xaa &&
          spmp_split[4] == 0xaa && spmp_split[5] == 0xaa &&
          spmp_split[6] == 0xaa && spmp_split[7] == 0xaa,
          "split store never modifies denied bytes or guard bytes");
    const bool unchanged = spmp_split[2] == 0xaa && spmp_split[3] == 0xaa;
    const bool earlier_fragment = spmp_split[2] == 0x44 && spmp_split[3] == 0x33;
    check(unchanged || earlier_fragment, "split store first-fragment invariant");
}

void
spmp_test_main(void)
{
    struct spmp_hw hw;
    write_pmp_baseline();
    spmp_discover_hw(&hw);
    check(hw.enabled && hw.count >= 4, "test_matching requires >= 4 SPMP entries");

    test_empty_table_default_deny(hw.count);
    test_tor_and_na4_boundaries(hw.count);
    test_priority_and_adjacent_entries(hw.count);
    test_split_store(hw.count);

    spmp_finish_phase1_and_exit();
    spmp_pass_and_exit();
}
