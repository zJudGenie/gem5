/* Shared trap harness, runtime hardware discovery, and section-pinned probe functions. */
#include "spmp_harness.h"

volatile uint32_t spmp_target __attribute__((section(".spmp_data.target"))) = 0x51a7c0de;
volatile uint32_t spmp_sc_status __attribute__((section(".spmp_data.status"))) = 0xffffffff;
volatile uint32_t spmp_locked_target __attribute__((section(".spmp_locked_data"))) = 0x7150cafe;
volatile uint8_t spmp_split[8] __attribute__((section(".spmp_split"), aligned(1)));

volatile uintptr_t spmp_saved_sp;
volatile uintptr_t spmp_probe_active;
volatile uintptr_t spmp_trap_seen;
volatile uintptr_t spmp_trap_cause;
volatile uintptr_t spmp_trap_tval;
volatile uintptr_t spmp_trap_epc;
volatile uintptr_t spmp_case_id;

void
semihost_write0(const char *message)
{
    register uintptr_t a0 __asm__("a0") = 4;
    register uintptr_t a1 __asm__("a1") = (uintptr_t)message;
    __asm__ volatile(
        ".balign 16\n\t"
        ".option push\n\t"
        ".option norvc\n\t"
        "slli x0, x0, 0x1f\n\t"
        "ebreak\n\t"
        "srai x0, x0, 7\n\t"
        ".option pop"
        : "+r"(a0) : "r"(a1) : "memory");
}

void
semihost_hex(const char *label, uintptr_t value)
{
    static const char digits[] = "0123456789abcdef";
    char output[2 + 2 * sizeof(uintptr_t) + 1];
    output[0] = '0';
    output[1] = 'x';
    for (unsigned i = 0; i < 2 * sizeof(uintptr_t); ++i) {
        unsigned shift = (2 * sizeof(uintptr_t) - i - 1) * 4;
        output[i + 2] = digits[(value >> shift) & 0xf];
    }
    output[sizeof(output) - 1] = 0;
    semihost_write0(label);
    semihost_write0(output);
    semihost_write0("\n");
}

void
m5_exit(void)
{
    register uintptr_t a0 __asm__("a0") = 0;
    register uintptr_t a1 __asm__("a1") = 0;
    __asm__ volatile(".word 0x4200007b" : "+r"(a0), "+r"(a1) :: "memory");
}

void
m5_fail(void)
{
    register uintptr_t a0 __asm__("a0") = 0;
#if __riscv_xlen == 32
    register uintptr_t a1 __asm__("a1") = 0;
    register uintptr_t a2 __asm__("a2") = 1;
    register uintptr_t a3 __asm__("a3") = 0;
    __asm__ volatile(".word 0x4400007b"
                     : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3) :: "memory");
#else
    register uintptr_t a1 __asm__("a1") = 1;
    __asm__ volatile(".word 0x4400007b" : "+r"(a0), "+r"(a1) :: "memory");
#endif
    for (;;) {}
}

int
check(int condition, const char *name)
{
    if (!condition) {
        semihost_write0("SPMP_WORKLOAD_FAIL: ");
        semihost_write0(name);
        semihost_write0("\n");
        semihost_hex("case=", spmp_case_id);
        semihost_hex("cause=", spmp_trap_cause);
        semihost_hex("tval=", spmp_trap_tval);
        semihost_hex("epc=", spmp_trap_epc);
        m5_fail();
    }
    return 1;
}

void
spmp_finish_phase1_and_exit(void)
{
    semihost_hex("SPMP_CASES_BEFORE_PHASE=", spmp_case_id);
    semihost_write0("SPMP_PHASE_READY\n");
    m5_exit();
}

void
spmp_pass_and_exit(void)
{
    semihost_write0("SPMP_WORKLOAD_PASS\n");
    m5_exit();
    for (;;) {}
}

static void
execute_probe(void (*fn)(uintptr_t, uintptr_t), uintptr_t arg0, uintptr_t arg1,
              uintptr_t priv, uintptr_t status_flags, uintptr_t direct_m)
{
    ++spmp_case_id;
    spmp_trap_seen = 0;
    spmp_trap_cause = 0;
    spmp_trap_tval = 0;
    spmp_trap_epc = 0;

    uintptr_t status = read_mstatus();
    status &= ~(MSTATUS_MPP_MASK | MSTATUS_MPRV | MSTATUS_SUM | MSTATUS_MXR);
    status |= (priv << 11) | status_flags;

    spmp_probe_active = 1;
    run_probe((uintptr_t)fn, status, direct_m, arg0, arg1);
    spmp_probe_active = 0;
}

void
expect_ok(void (*fn)(uintptr_t, uintptr_t), uintptr_t arg0, uintptr_t arg1,
          uintptr_t priv, uintptr_t status_flags, const char *name)
{
    const uintptr_t expected_ecall =
        priv == PRV_U ? CAUSE_ECALL_U :
        priv == PRV_S ? CAUSE_ECALL_S : CAUSE_ECALL_M;
    execute_probe(fn, arg0, arg1, priv, status_flags, 0);
    check(spmp_trap_seen == 1 &&
          spmp_trap_cause == expected_ecall &&
          spmp_trap_tval == 0,
          name);
}

void
expect_trap(void (*fn)(uintptr_t, uintptr_t), uintptr_t arg0, uintptr_t arg1,
            uintptr_t priv, uintptr_t status_flags,
            uintptr_t expected_cause, uintptr_t expected_tval,
            const char *name)
{
    execute_probe(fn, arg0, arg1, priv, status_flags, 0);
    const uintptr_t fn_pc = (uintptr_t)fn;
    const bool pc_ok = (expected_cause == CAUSE_INST_PAGE) ?
        (spmp_trap_epc == fn_pc) :
        (spmp_trap_epc >= fn_pc && spmp_trap_epc < fn_pc + 256);
    const bool tval_ok = (expected_cause == CAUSE_ILLEGAL && expected_tval == 0) ?
        (spmp_trap_tval != 0) : (spmp_trap_tval == expected_tval);
    check(spmp_trap_seen == 1 &&
          spmp_trap_cause == expected_cause &&
          pc_ok && tval_ok,
          name);
}

void
expect_mprv_ok(void (*fn)(uintptr_t, uintptr_t), uintptr_t arg0, uintptr_t arg1,
               uintptr_t mpp, uintptr_t status_flags, const char *name)
{
    execute_probe(fn, arg0, arg1, mpp, status_flags, 1);
    check(spmp_trap_seen == 1 &&
          spmp_trap_cause == CAUSE_ECALL_M &&
          spmp_trap_tval == 0,
          name);
}

void
expect_mprv_trap(void (*fn)(uintptr_t, uintptr_t), uintptr_t arg0, uintptr_t arg1,
                 uintptr_t mpp, uintptr_t status_flags,
                 uintptr_t expected_cause, uintptr_t expected_tval,
                 const char *name)
{
    execute_probe(fn, arg0, arg1, mpp, status_flags, 1);
    const uintptr_t fn_pc = (uintptr_t)fn;
    check(spmp_trap_seen == 1 &&
          spmp_trap_cause == expected_cause &&
          spmp_trap_epc >= fn_pc && spmp_trap_epc < fn_pc + 256 &&
          spmp_trap_tval == expected_tval,
          name);
}

void
write_pmp_baseline(void)
{
    __asm__ volatile("csrw 0x3b0, %0" :: "r"(PHYS_ADDR_MASK) : "memory");
    __asm__ volatile("csrw 0x3a0, %0" :: "r"((uintptr_t)0x1f) : "memory");
}

void
pmp_deny_page(uintptr_t base)
{
    const uintptr_t denied = napot(base, 4096);
    __asm__ volatile("csrw 0x3b0, %0" :: "r"(denied) : "memory");
    __asm__ volatile("csrw 0x3b1, %0" :: "r"(PHYS_ADDR_MASK) : "memory");
    __asm__ volatile("csrw 0x3a0, %0" :: "r"((uintptr_t)0x1f18) : "memory");
}

void
clear_all_spmp_entries(unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        write_siselect(ENTRY(i));
        write_sireg2(0);
        write_sireg(0);
    }
    fence_spmp();
}

void
set_test_rules(uintptr_t data_cfg)
{
    write_entry(0, napot((uintptr_t)__ucode_start, 4096),
                SPMP_A_NAPOT | SPMP_U | SPMP_X);
    write_entry(1, napot((uintptr_t)__spmp_data_start, 4096),
                SPMP_A_NAPOT | SPMP_U | data_cfg);
    write_entry(2, PHYS_ADDR_MASK, SPMP_A_NAPOT | SPMP_R | SPMP_W | SPMP_X);
    fence_spmp();
}

void
spmp_discover_hw(struct spmp_hw *hw)
{
    hw->enabled = false;
    hw->has_spmpen = false;
    hw->has_delegation = false;
    hw->count = 0;

    write_siselect(ENTRY(0));
    execute_probe(probe_m_csrr, CSR_SIREG, 0, PRV_M, 0, 0);
    if (spmp_trap_cause == CAUSE_ILLEGAL)
        return;
    hw->enabled = true;

    execute_probe(probe_m_csrr, CSR_SPMPEN, 0, PRV_M, 0, 0);
    hw->has_spmpen = (spmp_trap_cause == CAUSE_ECALL_M);

    execute_probe(probe_m_csrr, CSR_MPMPDELEG, 0, PRV_M, 0, 0);
    hw->has_delegation = (spmp_trap_cause == CAUSE_ECALL_M);

    unsigned writable = 0;
    for (unsigned i = 0; i < 64; ++i) {
        write_siselect(ENTRY(i));
        uintptr_t orig_addr = read_sireg();
        uintptr_t orig_cfg = read_sireg2();
        write_sireg(1);
        if (read_sireg() != 0) {
            writable = i + 1;
            write_sireg(orig_addr);
            write_sireg2(orig_cfg);
        }
    }
    hw->count = writable;
}

/* Section-pinned U-mode probes (.ucode). */
#define UCODE_PROBE __attribute__((section(".ucode"), aligned(64), noinline, noclone))
#define SCODE_PROBE __attribute__((section(".scode"), aligned(64), noinline, noclone))
#define MPROBE_ATTR __attribute__((section(".text.mprobe"), aligned(64), noinline, noclone))
#define TARGET_CODE __attribute__((section(".spmp_target_code"), aligned(16), noinline, noclone))

UCODE_PROBE void
probe_u_lw(uintptr_t addr, uintptr_t unused)
{
    (void)unused;
    (void)*(volatile uint32_t *)addr;
    __asm__ volatile("ecall");
}

UCODE_PROBE void
probe_u_lhu(uintptr_t addr, uintptr_t unused)
{
    (void)unused;
    (void)*(volatile uint16_t *)addr;
    __asm__ volatile("ecall");
}

UCODE_PROBE void
probe_u_lbu(uintptr_t addr, uintptr_t unused)
{
    (void)unused;
    (void)*(volatile uint8_t *)addr;
    __asm__ volatile("ecall");
}

UCODE_PROBE void
probe_u_sw(uintptr_t addr, uintptr_t val)
{
    *(volatile uint32_t *)addr = (uint32_t)val;
    __asm__ volatile("ecall");
}

UCODE_PROBE void
probe_u_lr(uintptr_t addr, uintptr_t unused)
{
    (void)unused;
    uint32_t tmp;
    __asm__ volatile("lr.w %0, (%1)\n\tecall" : "=r"(tmp) : "r"(addr) : "memory");
}

UCODE_PROBE void
probe_u_sc(uintptr_t addr, uintptr_t val)
{
    uint32_t status;
    __asm__ volatile("sc.w %0, %1, (%2)\n\tecall"
                     : "=r"(status) : "r"((uint32_t)val), "r"(addr) : "memory");
}

UCODE_PROBE void
probe_u_lrsc(uintptr_t addr, uintptr_t val)
{
    uint32_t tmp, status;
    __asm__ volatile(
        "lr.w %0, (%2)\n\t"
        "sc.w %1, %3, (%2)\n\t"
        : "=&r"(tmp), "=&r"(status)
        : "r"(addr), "r"((uint32_t)val)
        : "memory");
    spmp_sc_status = status;
    __asm__ volatile("ecall");
}

UCODE_PROBE void
probe_u_amoadd(uintptr_t addr, uintptr_t val)
{
    uint32_t old;
    __asm__ volatile("amoadd.w %0, %1, (%2)\n\tecall"
                     : "=r"(old) : "r"((uint32_t)val), "r"(addr) : "memory");
}

UCODE_PROBE void
probe_u_csrr_siselect(uintptr_t unused0, uintptr_t unused1)
{
    (void)unused0;
    (void)unused1;
    uintptr_t val;
    __asm__ volatile("csrr %0, 0x150\n\tecall" : "=r"(val));
}

UCODE_PROBE void
probe_u_csrr_spmpen(uintptr_t unused0, uintptr_t unused1)
{
    (void)unused0;
    (void)unused1;
    uintptr_t val;
    __asm__ volatile("csrr %0, 0x183\n\tecall" : "=r"(val));
}

/* Section-pinned S-mode probes (.scode). */
SCODE_PROBE void
probe_s_lw(uintptr_t addr, uintptr_t unused)
{
    (void)unused;
    (void)*(volatile uint32_t *)addr;
    __asm__ volatile("ecall");
}

SCODE_PROBE void
probe_s_sw(uintptr_t addr, uintptr_t val)
{
    *(volatile uint32_t *)addr = (uint32_t)val;
    __asm__ volatile("ecall");
}

SCODE_PROBE void
probe_s_write_spmpen(uintptr_t val, uintptr_t unused)
{
    (void)unused;
    __asm__ volatile("csrw 0x183, %0\n\tecall" :: "r"(val) : "memory");
}

SCODE_PROBE void
probe_s_clear_lock(uintptr_t entry_sel, uintptr_t unused)
{
    (void)unused;
    __asm__ volatile(
        "csrw 0x150, %0\n\t"
        "csrw 0x152, zero\n\t"
        "ecall"
        :: "r"(entry_sel) : "memory");
}

SCODE_PROBE void
probe_s_csrr_miselect(uintptr_t unused0, uintptr_t unused1)
{
    (void)unused0;
    (void)unused1;
    uintptr_t val;
    __asm__ volatile("csrr %0, 0x350\n\tecall" : "=r"(val));
}

/* Section-pinned target-page probes (.spmp_target_code at 0x80006000). */
TARGET_CODE void
probe_u_exec_target(uintptr_t unused0, uintptr_t unused1)
{
    (void)unused0;
    (void)unused1;
    __asm__ volatile("ecall");
}

TARGET_CODE void
probe_u_exec_target_lw(uintptr_t addr, uintptr_t unused)
{
    (void)unused;
    (void)*(volatile uint32_t *)addr;
    __asm__ volatile("ecall");
}

/* Section-pinned M-mode probes (.text.mprobe). */
MPROBE_ATTR void
probe_m_lw(uintptr_t addr, uintptr_t unused)
{
    (void)unused;
    (void)*(volatile uint32_t *)addr;
    __asm__ volatile("ecall");
}

MPROBE_ATTR void
probe_m_csrr(uintptr_t csr_num, uintptr_t unused)
{
    (void)unused;
    uintptr_t val = 0;
    switch (csr_num) {
    case 0x151: __asm__ volatile("csrr %0, 0x151" : "=r"(val)); break;
    case 0x153: __asm__ volatile("csrr %0, 0x153" : "=r"(val)); break;
    case 0x183: __asm__ volatile("csrr %0, 0x183" : "=r"(val)); break;
    case 0x193: __asm__ volatile("csrr %0, 0x193" : "=r"(val)); break;
    case 0x316: __asm__ volatile("csrr %0, 0x316" : "=r"(val)); break;
    case 0x350: __asm__ volatile("csrr %0, 0x350" : "=r"(val)); break;
    case 0x351: __asm__ volatile("csrr %0, 0x351" : "=r"(val)); break;
    case 0x353: __asm__ volatile("csrr %0, 0x353" : "=r"(val)); break;
    case 0x355: __asm__ volatile("csrr %0, 0x355" : "=r"(val)); break;
    case 0x356: __asm__ volatile("csrr %0, 0x356" : "=r"(val)); break;
    case 0x357: __asm__ volatile("csrr %0, 0x357" : "=r"(val)); break;
    default: break;
    }
    (void)val;
    __asm__ volatile("ecall");
}
