/* CSR programming, WARL canonicalization, lock rules, and checkpoint/switch lifecycle tests. */
#include "spmp_harness.h"

static void
test_status_bits(void)
{
    uintptr_t original, value;
    __asm__ volatile("csrr %0, sstatus" : "=r"(original));
    value = original | MSTATUS_SUM | MSTATUS_MXR;
    __asm__ volatile("csrw sstatus, %0" :: "r"(value) : "memory");
    __asm__ volatile("csrr %0, sstatus" : "=r"(value));
    check((value & (MSTATUS_SUM | MSTATUS_MXR)) == (MSTATUS_SUM | MSTATUS_MXR),
          "sstatus SUM and MXR can be set");

    value = original & ~(MSTATUS_SUM | MSTATUS_MXR);
    __asm__ volatile("csrw sstatus, %0" :: "r"(value) : "memory");
    __asm__ volatile("csrr %0, sstatus" : "=r"(value));
    check((value & (MSTATUS_SUM | MSTATUS_MXR)) == 0,
          "sstatus SUM and MXR can be cleared");
    __asm__ volatile("csrw sstatus, %0" :: "r"(original) : "memory");
}

static void
test_disabled_extension_csrs(const struct spmp_hw *hw)
{
    if (!hw->has_spmpen) {
        expect_trap(probe_m_csrr, CSR_SPMPEN, 0, PRV_M, 0,
                    CAUSE_ILLEGAL, 0, "disabled Sspmpen rejects spmpen in M-mode");
#if __riscv_xlen == 32
        expect_trap(probe_m_csrr, CSR_SPMPENH, 0, PRV_M, 0,
                    CAUSE_ILLEGAL, 0, "disabled Sspmpen rejects spmpenh on RV32");
#endif
    }
    if (!hw->has_delegation) {
        expect_trap(probe_m_csrr, CSR_MPMPDELEG, 0, PRV_M, 0,
                    CAUSE_ILLEGAL, 0, "disabled Smpmpdeleg rejects mpmpdeleg in M-mode");
        expect_trap(probe_m_csrr, CSR_MISELECT, 0, PRV_M, 0,
                    CAUSE_ILLEGAL, 0, "disabled Smpmpdeleg rejects miselect in M-mode");
    }
}

static void
test_active_csr_programming(unsigned count)
{
    const uintptr_t address = napot((uintptr_t)__spmp_data_start, 4096);
    const uintptr_t cfg = SPMP_A_NAPOT | SPMP_U | SPMP_SHARED | SPMP_R;

    if (count >= 3) {
        set_test_rules(SPMP_R | SPMP_W);
        expect_trap(probe_u_csrr_siselect, 0, 0, PRV_U, 0,
                    CAUSE_ILLEGAL, 0, "U-mode access to siselect faults");
    }

    clear_all_spmp_entries(count);
    write_entry(0, address, cfg);
    check(read_siselect() == ENTRY(0), "CSR selector retains entry 0");
    check(read_sireg() == address, "CSR canonical address readback");
    check((read_sireg2() & 0x3ff) == cfg, "CSR U/SHARED config readback");

    write_siselect(ENTRY(1));
    check(read_sireg2() == 0, "CSR selector switches entry");
    write_siselect(0xfff);
    check(read_siselect() == 0xfff, "CSR selector maximum 12-bit WARL");
    write_siselect(0);
    check(read_siselect() == 0, "CSR selector minimum WARL");
    write_siselect(ENTRY(0));

    /* Suppressed and read-modify-write CSR forms. */
    __asm__ volatile("csrrs x0, 0x152, x0" ::: "memory");
    check(read_sireg2() == cfg, "CSRRS with rs1=x0 suppresses write");
    __asm__ volatile("csrrs x0, 0x152, %0" :: "r"((uintptr_t)SPMP_X) : "memory");
    check(read_sireg2() == (cfg | SPMP_X), "CSRRS updates full backend config");
    __asm__ volatile("csrrc x0, 0x152, %0" :: "r"((uintptr_t)SPMP_X) : "memory");
    check(read_sireg2() == cfg, "CSRRC clears bit in full backend config");
    __asm__ volatile("csrrw x0, 0x152, %0" :: "r"(cfg) : "memory");
    check(read_sireg2() == cfg, "CSRRW with rd=x0 writes canonical backend");

    /* Entry count boundaries and out-of-range selectors. */
    if (count == 64) {
        write_entry(63, address, SPMP_A_NAPOT | SPMP_U | SPMP_R);
        check(read_sireg() == address, "CSR entry 63 writable when count=64");
    } else {
        write_siselect(ENTRY(63));
        write_sireg(address);
        write_sireg2(cfg);
        check(read_sireg() == 0 && read_sireg2() == 0,
              "unimplemented entry 63 reads zero and ignores writes");

        write_siselect(ENTRY(count));
        write_sireg(address);
        write_sireg2(cfg);
        check(read_sireg() == 0 && read_sireg2() == 0,
              "configured count boundary reads zero and ignores writes");
    }

    write_siselect(0x140);
    expect_trap(probe_m_csrr, CSR_SIREG, 0, PRV_M, 0,
                CAUSE_ILLEGAL, 0, "unsupported siselect 0x140 faults on sireg");

    write_siselect(ENTRY(0));
    expect_trap(probe_m_csrr, CSR_SIREG3, 0, PRV_M, 0,
                CAUSE_ILLEGAL, 0, "reserved sireg3 faults as illegal instruction");

    /* WARL canonicalization of reserved encodings. */
    write_siselect(ENTRY(0));
    write_sireg2(SPMP_A_NAPOT | SPMP_SHARED | SPMP_R);
    check((read_sireg2() & SPMP_SHARED) == 0,
          "WARL clears SHARED when U is clear");
    write_sireg2(SPMP_A_NAPOT | SPMP_U | SPMP_W);
    check((read_sireg2() & (SPMP_R | SPMP_W)) == 0,
          "WARL canonicalizes reserved W=1,R=0 to W=0");

    /* Lock bit enforcement on high spare entry and TOR predecessor. */
    const unsigned lock_index = count - 1;
    write_entry(lock_index, address, SPMP_A_NAPOT | SPMP_U | SPMP_R | SPMP_W);
    write_sireg2(SPMP_A_NAPOT | SPMP_U | SPMP_R | SPMP_W | SPMP_LOCK);
    const uintptr_t locked_address = read_sireg();
    write_sireg(0);
    check(read_sireg() == locked_address, "locked entry ignores address write");
    write_sireg2(0);
    check((read_sireg2() & SPMP_LOCK) != 0, "locked entry ignores config write");

    if (count >= 16) {
        const uintptr_t pred_addr = (uintptr_t)__spmp_data_start >> 2;
        write_entry(13, pred_addr, 0);
        write_entry(14, ((uintptr_t)__spmp_data_start + 4096) >> 2,
                    SPMP_A_TOR | SPMP_LOCK | SPMP_R);
        write_siselect(ENTRY(13));
        write_sireg(pred_addr + 1);
        check(read_sireg() == pred_addr,
              "locked TOR successor protects predecessor address");
    }
}

static void
configure_lifecycle_state(const struct spmp_hw *hw)
{
    if (!hw->enabled || hw->count < 8)
        return;
    set_test_rules(SPMP_R | SPMP_W);
    write_siselect(ENTRY(2));
    write_sireg2(0);
    write_entry(3, (uintptr_t)__spmp_locked_start >> 2, 0);
    write_entry(4, ((uintptr_t)__spmp_locked_start + 4096) >> 2,
                SPMP_A_TOR | SPMP_LOCK | SPMP_U | SPMP_R);
    write_entry(5, napot(0x8000b000UL, 4096),
                SPMP_A_NAPOT | SPMP_LOCK | SPMP_U | SPMP_R);
    write_entry(6, PHYS_ADDR_MASK, SPMP_A_NAPOT | SPMP_R | SPMP_W | SPMP_X);
    write_siselect(ENTRY(2));
    fence_spmp();
}

static void
verify_lifecycle_state(const struct spmp_hw *hw)
{
    if (!hw->enabled || hw->count < 8)
        return;
    check(read_siselect() == ENTRY(2), "lifecycle retains siselect across exit/restore");

    write_siselect(ENTRY(3));
    const uintptr_t pred = (uintptr_t)__spmp_locked_start >> 2;
    check(read_sireg() == pred && read_sireg2() == 0,
          "lifecycle retains TOR predecessor state");
    write_sireg(pred + 1);
    check(read_sireg() == pred,
          "lifecycle locked TOR predecessor continues to ignore writes");

    write_siselect(ENTRY(4));
    check(read_sireg() == (((uintptr_t)__spmp_locked_start + 4096) >> 2) &&
          read_sireg2() == (SPMP_A_TOR | SPMP_LOCK | SPMP_U | SPMP_R),
          "lifecycle retains locked TOR entry");

    write_siselect(ENTRY(5));
    check(read_sireg() == napot(0x8000b000UL, 4096) &&
          read_sireg2() == (SPMP_A_NAPOT | SPMP_LOCK | SPMP_U | SPMP_R),
          "lifecycle retains second locked NAPOT entry");
    write_sireg2(0);
    check(read_sireg2() == (SPMP_A_NAPOT | SPMP_LOCK | SPMP_U | SPMP_R),
          "lifecycle config lock remains enforced");

    write_pmp_baseline();
    expect_ok(probe_u_lw, (uintptr_t)&spmp_locked_target, 0, PRV_U, 0,
              "lifecycle retains locked TOR read permission");
    expect_trap(probe_u_sw, (uintptr_t)&spmp_locked_target, 0x2468ace0, PRV_U, 0,
                CAUSE_STORE_PAGE, (uintptr_t)&spmp_locked_target,
                "lifecycle retains locked TOR store denial");
    check(spmp_locked_target == 0x7150cafe,
          "lifecycle denied store preserves locked sentinel");
}

void
spmp_test_main(void)
{
    struct spmp_hw hw;
    write_pmp_baseline();
    test_status_bits();
    spmp_discover_hw(&hw);
    test_disabled_extension_csrs(&hw);

    if (!hw.enabled) {
        write_siselect(ENTRY(0));
        expect_trap(probe_m_csrr, CSR_SIREG, 0, PRV_M, 0,
                    CAUSE_ILLEGAL, 0, "disabled Sspmp faults on sireg access");
        expect_ok(probe_u_lw, (uintptr_t)&spmp_target, 0, PRV_U, 0,
                  "disabled Sspmp permits U Bare load and fetch");
        expect_ok(probe_u_sw, (uintptr_t)&spmp_target, 0x51a7c0de, PRV_U, 0,
                  "disabled Sspmp permits U Bare store");
    } else if (hw.count == 0) {
        write_siselect(ENTRY(0));
        check(read_sireg() == 0 && read_sireg2() == 0,
              "zero-entry Sspmp aliases read zero");
        write_sireg(1);
        write_sireg2(1);
        check(read_sireg() == 0 && read_sireg2() == 0,
              "zero-entry Sspmp aliases ignore writes");
        expect_ok(probe_u_lw, (uintptr_t)&spmp_target, 0, PRV_U, 0,
                  "zero-entry Sspmp permits U Bare load and fetch");
        expect_ok(probe_u_sw, (uintptr_t)&spmp_target, 0x51a7c0de, PRV_U, 0,
                  "zero-entry Sspmp permits U Bare store");
    } else {
        test_active_csr_programming(hw.count);
        configure_lifecycle_state(&hw);
    }

    spmp_finish_phase1_and_exit();
    verify_lifecycle_state(&hw);
    spmp_pass_and_exit();
}
