/* Data-driven Figure 4 permission matrix, SUM/MXR, LR/SC/AMO, and MPRV tests. */
#include "spmp_harness.h"

static void
test_figure4_permission_matrix(void)
{
    const uintptr_t target = (uintptr_t)&spmp_target;

    /*
     * Figure 4 permission encodings for packed (X, W, R) in 0..7:
     * Note that reserved (W=1, R=0) canonicalizes to W=0 via WARL.
     */
    for (unsigned rwx = 0; rwx < 8; ++rwx) {
        const bool can_r = (rwx & SPMP_R) != 0;
        const bool can_w = can_r && ((rwx & SPMP_W) != 0);
        const bool can_x = (rwx & SPMP_X) != 0;

        /* 1. U-only rule (U=1, SHARED=0): U gets (R,W,X); S gets (R,W) only if SUM=1, never X. */
        set_test_rules(rwx);
        if (can_r) {
            expect_ok(probe_u_lw, target, 0, PRV_U, 0, "Figure 4 U-only: U load allowed");
            expect_trap(probe_s_lw, target, 0, PRV_S, 0,
                        CAUSE_LOAD_PAGE, target, "Figure 4 U-only: S load denied when SUM=0");
            expect_ok(probe_s_lw, target, 0, PRV_S, MSTATUS_SUM,
                      "Figure 4 U-only: S load allowed when SUM=1");
        } else {
            expect_trap(probe_u_lw, target, 0, PRV_U, 0,
                        CAUSE_LOAD_PAGE, target, "Figure 4 U-only: U load denied");
            expect_trap(probe_s_lw, target, 0, PRV_S, MSTATUS_SUM | MSTATUS_MXR,
                        CAUSE_LOAD_PAGE, target,
                        "Figure 4 U-only: S load denied even with SUM=1, MXR=1");
        }

        if (can_w) {
            expect_ok(probe_u_sw, target, 0x51a7c0de, PRV_U, 0,
                      "Figure 4 U-only: U store allowed");
            expect_trap(probe_s_sw, target, 0x51a7c0de, PRV_S, 0,
                        CAUSE_STORE_PAGE, target, "Figure 4 U-only: S store denied when SUM=0");
            expect_ok(probe_s_sw, target, 0x51a7c0de, PRV_S, MSTATUS_SUM,
                      "Figure 4 U-only: S store allowed when SUM=1");
        } else {
            expect_trap(probe_u_sw, target, 0x51a7c0de, PRV_U, 0,
                        CAUSE_STORE_PAGE, target, "Figure 4 U-only: U store denied");
        }

        /* Check U and S instruction fetch against U-only rule. */
        write_entry(1, napot((uintptr_t)__spmp_data_start, 4096),
                    SPMP_A_NAPOT | SPMP_U | SPMP_R);
        write_entry(0, napot((uintptr_t)__ucode_start, 4096),
                    SPMP_A_NAPOT | SPMP_U | (rwx & SPMP_X));
        fence_spmp();
        if (can_x) {
            expect_ok(probe_u_lw, target, 0, PRV_U, 0,
                      "Figure 4 U-only: U fetch allowed when X=1");
        } else {
            expect_trap(probe_u_lw, target, 0, PRV_U, 0,
                        CAUSE_INST_PAGE, (uintptr_t)probe_u_lw,
                        "Figure 4 U-only: U fetch denied when X=0");
        }

        write_entry(0, napot((uintptr_t)__scode_start, 4096),
                    SPMP_A_NAPOT | SPMP_U | (rwx & SPMP_X));
        fence_spmp();
        expect_trap(probe_s_lw, target, 0, PRV_S, MSTATUS_SUM,
                    CAUSE_INST_PAGE, (uintptr_t)probe_s_lw,
                    "Figure 4 U-only: S fetch always denied even with SUM=1");

        /* 2. S-only rule (U=0, SHARED=0): S gets (R,W,X); U always denied. */
        write_entry(0, napot((uintptr_t)__ucode_start, 4096),
                    SPMP_A_NAPOT | SPMP_U | SPMP_X);
        write_entry(1, napot((uintptr_t)__spmp_data_start, 4096),
                    SPMP_A_NAPOT | rwx);
        write_entry(2, PHYS_ADDR_MASK, SPMP_A_NAPOT | SPMP_R | SPMP_W | SPMP_X);
        fence_spmp();
        expect_trap(probe_u_lw, target, 0, PRV_U, 0,
                    CAUSE_LOAD_PAGE, target, "Figure 4 S-only: U load always denied");
        if (can_r) {
            expect_ok(probe_s_lw, target, 0, PRV_S, 0, "Figure 4 S-only: S load allowed");
        } else {
            expect_trap(probe_s_lw, target, 0, PRV_S, MSTATUS_MXR,
                        CAUSE_LOAD_PAGE, target, "Figure 4 S-only: S load denied");
        }

        /* 3. Shared rule (U=1, SHARED=1): special cases RW (U read-only) and RWX (U exec-only). */
        const bool shared_rw = can_r && can_w && !can_x;
        const bool shared_rwx = can_r && can_w && can_x;
        const bool u_shared_r = shared_rwx ? false : can_r;
        const bool u_shared_w = (shared_rw || shared_rwx) ? false : can_w;

        set_test_rules(SPMP_SHARED | rwx);
        if (u_shared_r) {
            expect_ok(probe_u_lw, target, 0, PRV_U, 0, "Figure 4 Shared: U load allowed");
        } else {
            expect_trap(probe_u_lw, target, 0, PRV_U, 0,
                        CAUSE_LOAD_PAGE, target, "Figure 4 Shared: U load denied");
        }
        if (u_shared_w) {
            expect_ok(probe_u_sw, target, 0x51a7c0de, PRV_U, 0,
                      "Figure 4 Shared: U store allowed");
        } else {
            expect_trap(probe_u_sw, target, 0x51a7c0de, PRV_U, 0,
                        CAUSE_STORE_PAGE, target, "Figure 4 Shared: U store denied");
        }
        if (can_w) {
            expect_ok(probe_s_sw, target, 0x51a7c0de, PRV_S, 0,
                      "Figure 4 Shared: S store allowed with SUM=0");
        }
    }

    /* Shared RWX execute-only for U: U executes target page but cannot read from it. */
    set_test_rules(SPMP_SHARED | SPMP_R | SPMP_W | SPMP_X);
    expect_ok(probe_u_exec_target, 0, 0, PRV_U, 0,
              "Shared RWX permits U execution in target page");
    expect_trap(probe_u_exec_target_lw, target, 0, PRV_U, 0,
                CAUSE_LOAD_PAGE, target,
                "Shared RWX denies U data load from executing page");
}

static void
test_atomics_and_sentinels(void)
{
    const uintptr_t target = (uintptr_t)&spmp_target;

    /* Permitted U LR/SC and AMOADD.W. */
    set_test_rules(SPMP_R | SPMP_W);
    spmp_target = 0x51a7c0de;
    spmp_sc_status = 0xffffffff;
    expect_ok(probe_u_lrsc, target, 0x13579bdf, PRV_U, 0,
              "U LR/SC pair succeeds with RW permission");
    check(spmp_target == 0x13579bdf && spmp_sc_status == 0,
          "successful U SC updates target and status");

    spmp_target = 0x51a7c0de;
    expect_ok(probe_u_amoadd, target, 1, PRV_U, 0,
              "U AMOADD.W succeeds with RW permission");
    check(spmp_target == 0x51a7c0df, "successful U AMO increments target");

    /* Denied store, SC, and AMO preserve sentinel and report CAUSE_STORE_PAGE (15). */
    set_test_rules(SPMP_R);
    spmp_target = 0x51a7c0de;
    expect_trap(probe_u_sw, target, 0x13579bdf, PRV_U, 0,
                CAUSE_STORE_PAGE, target, "U store on read-only rule faults cause 15");
    check(spmp_target == 0x51a7c0de, "denied store preserves sentinel");

    expect_trap(probe_u_amoadd, target, 1, PRV_U, 0,
                CAUSE_STORE_PAGE, target, "U AMO on read-only rule faults cause 15");
    check(spmp_target == 0x51a7c0de, "denied AMO preserves sentinel");

    expect_trap(probe_u_sc, target, 0x2468ace0, PRV_U, 0,
                CAUSE_STORE_PAGE, target, "U SC on read-only rule faults cause 15");
    check(spmp_target == 0x51a7c0de, "denied SC preserves sentinel");

    /* Denied LR reports CAUSE_LOAD_PAGE (13). */
    set_test_rules(0);
    expect_trap(probe_u_lr, target, 0, PRV_U, 0,
                CAUSE_LOAD_PAGE, target, "U LR on no-access rule faults cause 13");
}

static void
test_mprv_privilege(void)
{
    const uintptr_t target = (uintptr_t)&spmp_target;

    set_test_rules(0);
    expect_mprv_trap(probe_m_lw, target, 0, PRV_U, MSTATUS_MPRV,
                     CAUSE_LOAD_PAGE, target,
                     "MPRV=1, MPP=U data load uses U SPMP permissions");
    expect_mprv_trap(probe_m_lw, target, 0, PRV_S, MSTATUS_MPRV | MSTATUS_SUM,
                     CAUSE_LOAD_PAGE, target,
                     "MPRV=1, MPP=S data load enforces SPMP");
    expect_mprv_ok(probe_m_lw, target, 0, PRV_M, MSTATUS_MPRV,
                   "MPRV=1, MPP=M data load bypasses SPMP");
    expect_mprv_ok(probe_m_lw, target, 0, PRV_U, 0,
                   "MPRV=0 ignores MPP=U for M data load");
    expect_mprv_ok(probe_m_lw, target, 0, PRV_S, 0,
                   "MPRV=0 ignores MPP=S for M data load");
    expect_ok(probe_m_lw, target, 0, PRV_M, 0,
              "Normal M-mode load bypasses SPMP");

    set_test_rules(SPMP_R);
    expect_mprv_trap(probe_m_lw, target, 0, PRV_S, MSTATUS_MPRV,
                     CAUSE_LOAD_PAGE, target,
                     "MPRV=1, MPP=S denies U-only read rule when SUM=0");
    expect_mprv_ok(probe_m_lw, target, 0, PRV_S, MSTATUS_MPRV | MSTATUS_SUM,
                   "MPRV=1, MPP=S allows U-only read rule when SUM=1");
    expect_mprv_ok(probe_m_lw, target, 0, PRV_U, MSTATUS_MPRV,
                   "MPRV=1, MPP=U allows U-only read rule");
}

void
spmp_test_main(void)
{
    struct spmp_hw hw;
    write_pmp_baseline();
    spmp_discover_hw(&hw);
    check(hw.enabled && hw.count >= 3, "test_permissions requires >= 3 SPMP entries");

    test_figure4_permission_matrix();
    test_atomics_and_sentinels();
    test_mprv_privilege();

    spmp_finish_phase1_and_exit();
    spmp_pass_and_exit();
}
