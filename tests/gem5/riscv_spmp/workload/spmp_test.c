/* Privileged architectural SPMP probes for RV32 and RV64 full-system runs. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef SPMP_FEATURE
#define SPMP_FEATURE 1
#endif
#ifndef SPMP_COUNT
#define SPMP_COUNT 16
#endif
#ifndef TEST_XLEN
#error TEST_XLEN must be 32 or 64
#endif

#define CSR_SISELECT 0x150
#define CSR_SIREG 0x151
#define CSR_SIREG2 0x152
#define CSR_SIREG3 0x153
#define CSR_SATP 0x180
#define CSR_PMPCFG0 0x3a0
#define CSR_PMPADDR0 0x3b0
#define CAUSE_LOAD_ACCESS 5
#define CAUSE_ILLEGAL 2
#define CAUSE_INST_PAGE 12
#define CAUSE_LOAD_PAGE 13
#define CAUSE_STORE_PAGE 15
#define MSTATUS_MPP_MASK (3UL << 11)
#define MSTATUS_MPRV (1UL << 17)
#define MSTATUS_SUM (1UL << 18)
#define MSTATUS_MXR (1UL << 19)
#define SPMP_A_NAPOT (3UL << 3)
#define SPMP_LOCK (1UL << 7)
#define SPMP_U (1UL << 8)
#define SPMP_SHARED (1UL << 9)
#define ENTRY(i) (0x100 + (i))

extern void run_probe(uintptr_t pc, uintptr_t status, uintptr_t direct_m_mode);
extern void probe_s_load(void), probe_s_load_fault(void), probe_s_load_resume(void);
extern void probe_s_store(void), probe_s_store_fault(void), probe_s_store_resume(void);
extern void probe_s_lr(void), probe_s_lr_fault(void), probe_s_lr_resume(void);
extern void probe_s_sc(void), probe_s_sc_fault(void), probe_s_sc_resume(void);
extern void probe_s_amo(void), probe_s_amo_fault(void), probe_s_amo_resume(void);
extern void probe_u_load(void), probe_u_load_fault(void), probe_u_load_resume(void);
extern void probe_u_store(void), probe_u_store_fault(void), probe_u_store_resume(void);
extern void probe_u_lr(void), probe_u_lr_fault(void), probe_u_lr_resume(void);
extern void probe_u_sc(void), probe_u_sc_fault(void), probe_u_sc_resume(void);
extern void probe_u_amo(void), probe_u_amo_fault(void), probe_u_amo_resume(void);
extern void probe_m_load(void), probe_m_load_fault(void), probe_m_load_resume(void);
extern void probe_invalid_alias(void), probe_invalid_alias_fault(void);
extern void probe_u_csr(void), probe_u_csr_fault(void);
extern char __scode_start[], __ucode_start[], __spmp_data_start[];
extern char __spmp_locked_start[];
extern void probe_u_csr_resume(void), probe_invalid_alias_resume(void);
extern void probe_reserved_alias_resume(void);
#if TEST_XLEN == 64
static volatile uint64_t sv39_root[512]
    __attribute__((section(".pagetable"), aligned(4096)));
#endif
extern void probe_u_lrsc(void), probe_u_lrsc_fault(void), probe_u_lrsc_resume(void);
extern void probe_u_tor_cross(void), probe_u_tor_cross_fault(void), probe_u_tor_cross_resume(void);
extern void probe_u_tor_last(void), probe_u_tor_last_fault(void), probe_u_tor_last_resume(void);
extern void probe_u_tor_after(void), probe_u_tor_after_fault(void), probe_u_tor_after_resume(void);
extern void probe_u_na4_cross(void), probe_u_na4_cross_fault(void), probe_u_na4_cross_resume(void);
extern void probe_u_locked_load(void), probe_u_locked_load_fault(void), probe_u_locked_load_resume(void);
extern void probe_u_locked_store(void), probe_u_locked_store_fault(void), probe_u_locked_store_resume(void);
extern void probe_u_exec_page(void), probe_u_exec_page_resume(void);
extern void probe_u_target_load(void), probe_u_target_load_fault(void), probe_u_target_load_resume(void);
extern void probe_reserved_alias(void), probe_reserved_alias_fault(void);
extern void probe_u_split_store(void), probe_u_split_store_fault(void);

volatile uint32_t spmp_target __attribute__((section(".spmp_data.target"))) = 0x51a7c0de;
volatile uint32_t spmp_sc_status __attribute__((section(".spmp_data.status"))) = 0xffffffff;
volatile uint32_t spmp_locked_target __attribute__((section(".spmp_locked_data"))) = 0x7150cafe;
volatile uint8_t spmp_split[8]
    __attribute__((section(".spmp_split"), aligned(1)));
volatile uintptr_t spmp_expected_tval_alt;
volatile uintptr_t spmp_active;
volatile uintptr_t spmp_case_id;
volatile uintptr_t spmp_expected_cause;
volatile uintptr_t spmp_expected_tval;
volatile uintptr_t spmp_expected_pc;
volatile uintptr_t spmp_seen;
volatile uintptr_t spmp_failure;
volatile uintptr_t spmp_observed[4];

/* Select a register using the numeric Sscsrind CSR encoding. */
static inline void
write_siselect(uintptr_t value)
{
    __asm__ volatile("csrw 0x150, %0" :: "r"(value) : "memory");
}

/* Read the retained 12-bit indirect selector. */
static inline uintptr_t
read_siselect(void)
{
    uintptr_t value;
    __asm__ volatile("csrr %0, 0x150" : "=r"(value));
    return value;
}

/* Write the selected SPMP address register. */
static inline void
write_sireg(uintptr_t value)
{
    __asm__ volatile("csrw 0x151, %0" :: "r"(value) : "memory");
}

/* Read the selected canonical SPMP address. */
static inline uintptr_t
read_sireg(void)
{
    uintptr_t value;
    __asm__ volatile("csrr %0, 0x151" : "=r"(value));
    return value;
}

/* Write the selected complete SPMP configuration. */
static inline void
write_sireg2(uintptr_t value)
{
    __asm__ volatile("csrw 0x152, %0" :: "r"(value) : "memory");
}

/* Read all defined bits of the selected SPMP configuration. */
static inline uintptr_t
read_sireg2(void)
{
    uintptr_t value;
    __asm__ volatile("csrr %0, 0x152" : "=r"(value));
    return value;
}

/* Read machine status before constructing a probe privilege state. */
static inline uintptr_t
read_mstatus(void)
{
    uintptr_t value;
    __asm__ volatile("csrr %0, mstatus" : "=r"(value));
    return value;
}

/* Grant unlocked PMP access before isolating one SPMP condition. */
static inline void
write_pmp_baseline(void)
{
#if TEST_XLEN == 32
    uintptr_t all = 0xffffffffUL;
#else
    uintptr_t all = (1UL << 54) - 1;
#endif
    __asm__ volatile("csrw 0x3b0, %0" :: "r"(all) : "memory");
    __asm__ volatile("csrw 0x3a0, %0" :: "r"(0x1f) : "memory");
}

/* Encode a naturally aligned region of at least eight bytes. */
static uintptr_t
napot(uintptr_t base, uintptr_t size)
{
    return (base >> 2) | ((size >> 3) - 1);
}

/* Program one address and full configuration through the indirect window. */
static void
write_entry(unsigned index, uintptr_t address, uintptr_t config)
{
    write_siselect(ENTRY(index));
    write_sireg(address);
    write_sireg2(config);
}

/* Order prior SPMP writes before subsequent S/U accesses. */
static void
fence_spmp(void)
{
    __asm__ volatile(".word 0x12000073" ::: "memory");
}

/* Write a zero-terminated diagnostic through configured semihosting. */
static void
semihost_write0(const char *message)
{
    register uintptr_t a0 __asm__("a0") = 4;
    register uintptr_t a1 __asm__("a1") = (uintptr_t)message;
    __asm__ volatile(".balign 16\n\t.option push\n\t.option norvc\n\tslli x0, x0, 0x1f\n\tebreak\n\tsrai x0, x0, 7\n\t.option pop"
                     : "+r"(a0) : "r"(a1) : "memory");
}

/* Exit this simulation phase immediately using the m5 ABI. */
static void
m5_exit(void)
{
    register uintptr_t a0 __asm__("a0") = 0;
    register uintptr_t a1 __asm__("a1") = 0;
    // RegABI32 consumes a0:a1 for the 64-bit delay; RV64 only consumes a0.
    __asm__ volatile(".word 0x4200007b" : "+r"(a0), "+r"(a1) :: "memory");
}

/* Terminate with a nonzero m5 failure code and zero delay. */
static void
m5_fail(void)
{
    register uintptr_t a0 __asm__("a0") = 0;
#if TEST_XLEN == 32
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

/* Print an XLEN-wide hexadecimal trap field without libc. */
static void
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

/* Stop with the first mismatched case and print the saved architectural trap. */
static int
check(int condition, const char *name)
{
    if (!condition) {
        semihost_write0("SPMP_WORKLOAD_FAIL: ");
        semihost_write0(name);
        semihost_write0("\n");
        semihost_hex("case=", spmp_observed[3]);
        semihost_hex("cause=", spmp_observed[0]);
        semihost_hex("tval=", spmp_observed[1]);
        semihost_hex("pc=", spmp_observed[2]);
        semihost_hex("expected_cause=", spmp_expected_cause);
        semihost_hex("expected_tval=", spmp_expected_tval);
        semihost_hex("expected_pc=", spmp_expected_pc);
        m5_fail();
    }
    return 1;
}

/* Assign a case ID and clear the preceding probe trap record. */
static void
prepare_expectation(uintptr_t cause, uintptr_t pc, uintptr_t tval)
{
    ++spmp_case_id;
    spmp_expected_cause = cause;
    spmp_expected_pc = pc;
    spmp_expected_tval = tval;
    spmp_expected_tval_alt = 0;
    spmp_seen = 0;
    spmp_failure = 0;
    spmp_observed[0] = spmp_observed[1] = spmp_observed[2] = 0;
    spmp_observed[3] = spmp_case_id;
}

/* Enter one S/U probe and validate its architectural trap or successful ecall. */
static void
expect_probe(uintptr_t pc, uintptr_t mode, uintptr_t status_flags,
             uintptr_t cause, uintptr_t fault_pc, uintptr_t tval,
             const char *name)
{
    uintptr_t status = read_mstatus();
    status &= ~(MSTATUS_MPP_MASK | MSTATUS_MPRV | MSTATUS_SUM | MSTATUS_MXR);
    status |= (mode << 11) | status_flags;
    if (cause == 0) {
        cause = mode == 0 ? 8 : mode == 1 ? 9 : 11;
        fault_pc = pc == (uintptr_t)probe_s_load ?
            (uintptr_t)probe_s_load_resume :
            pc == (uintptr_t)probe_s_store ? (uintptr_t)probe_s_store_resume :
            pc == (uintptr_t)probe_s_lr ? (uintptr_t)probe_s_lr_resume :
            pc == (uintptr_t)probe_s_sc ? (uintptr_t)probe_s_sc_resume :
            pc == (uintptr_t)probe_s_amo ? (uintptr_t)probe_s_amo_resume :
            pc == (uintptr_t)probe_u_load ? (uintptr_t)probe_u_load_resume :
            pc == (uintptr_t)probe_u_store ? (uintptr_t)probe_u_store_resume :
            pc == (uintptr_t)probe_u_lr ? (uintptr_t)probe_u_lr_resume :
            pc == (uintptr_t)probe_u_sc ? (uintptr_t)probe_u_sc_resume :
            pc == (uintptr_t)probe_u_amo ? (uintptr_t)probe_u_amo_resume :
            pc == (uintptr_t)probe_u_lrsc ? (uintptr_t)probe_u_lrsc_resume :
            pc == (uintptr_t)probe_u_tor_last ? (uintptr_t)probe_u_tor_last_resume :
            pc == (uintptr_t)probe_u_locked_load ? (uintptr_t)probe_u_locked_load_resume :
            pc == (uintptr_t)probe_u_locked_store ? (uintptr_t)probe_u_locked_store_resume :
            pc == (uintptr_t)probe_u_exec_page ? (uintptr_t)probe_u_exec_page_resume :
            pc == (uintptr_t)probe_u_target_load ? (uintptr_t)probe_u_target_load_resume :
            pc == (uintptr_t)probe_m_load ? (uintptr_t)probe_m_load_resume :
            pc == (uintptr_t)probe_u_csr ? (uintptr_t)probe_u_csr_resume :
            pc == (uintptr_t)probe_invalid_alias ? (uintptr_t)probe_invalid_alias_resume :
            (uintptr_t)probe_reserved_alias_resume;
        tval = 0;
    }
    prepare_expectation(cause, fault_pc, tval);
    spmp_active = 1;
    run_probe(pc, status, 0);
    spmp_active = 0;
    check(spmp_seen == 1 && spmp_failure == 0, name);
}

/* Run an M-mode instruction with MPRV selecting its effective data mode. */
static void
expect_mprv_probe(uintptr_t pc, uintptr_t mpp, uintptr_t flags,
                  uintptr_t cause, uintptr_t fault_pc, uintptr_t tval,
                  const char *name)
{
    uintptr_t status = read_mstatus();
    status &= ~(MSTATUS_MPP_MASK | MSTATUS_MPRV | MSTATUS_SUM | MSTATUS_MXR);
    status |= (mpp << 11) | flags;
    prepare_expectation(cause ? cause : 11,
                        cause ? fault_pc : (uintptr_t)probe_m_load_resume,
                        tval);
    spmp_active = 1;
    run_probe(pc, status, 1);
    spmp_active = 0;
    check(spmp_seen == 1 && spmp_failure == 0, name);
}

/* Install separate U code/data entries before the S fallback. */
static void
set_test_rules(uintptr_t data_cfg)
{
#if TEST_XLEN == 32
    const uintptr_t physical_mask = 0xffffffffUL;
#else
    const uintptr_t physical_mask = (1UL << 54) - 1;
#endif
    write_entry(0, napot((uintptr_t)__ucode_start, 4096),
                SPMP_A_NAPOT | SPMP_U | 4);
    write_entry(1, napot((uintptr_t)__spmp_data_start, 4096),
                SPMP_A_NAPOT | SPMP_U | data_cfg);
    write_entry(2, physical_mask, SPMP_A_NAPOT | 7);
    fence_spmp();
}

/* Exercise canonical aliases, selectors, WARL, RMW and architectural locks. */
static void
test_csr_programming(void)
{
    const uintptr_t address = napot((uintptr_t)__spmp_data_start, 4096);
    uintptr_t cfg = SPMP_A_NAPOT | SPMP_U | SPMP_SHARED | 1;
    if (SPMP_COUNT >= 3) {
        set_test_rules(3);
        expect_probe((uintptr_t)probe_u_csr, 0, 0, CAUSE_ILLEGAL,
                     (uintptr_t)probe_u_csr_fault, 0x150022f3,
                     "U-mode access to supervisor selector faults");
    }
    for (unsigned i = 0; i < SPMP_COUNT; ++i) {
        write_siselect(ENTRY(i));
        write_sireg2(0);
    }
    write_entry(0, address, cfg);
    check(read_siselect() == ENTRY(0), "CSR selector entry 0");
    check(read_sireg() == address, "CSR canonical address readback");
    check((read_sireg2() & 0x3ff) == cfg, "CSR U/SHARED readback");

    write_siselect(ENTRY(1));
    check(read_sireg2() == 0, "CSR selector switch");
    write_siselect(0xfff);
    check(read_siselect() == 0xfff, "CSR selector maximum WARL");
    write_siselect(0);
    check(read_siselect() == 0, "CSR selector minimum WARL");
    write_siselect(ENTRY(0));

    __asm__ volatile("csrrs x0, 0x152, x0" ::: "memory");
    check(read_sireg2() == cfg, "CSR suppressed read-modify-write");
    __asm__ volatile("csrrs x0, 0x152, %0" :: "r"((uintptr_t)4) : "memory");
    check(read_sireg2() == (cfg | 4), "CSRRS uses full backend value");
    __asm__ volatile("csrrc x0, 0x152, %0" :: "r"((uintptr_t)4) : "memory");
    check(read_sireg2() == cfg, "CSRRC uses full backend value");
    __asm__ volatile("csrrw x0, 0x152, %0" :: "r"(cfg) : "memory");
    check(read_sireg2() == cfg, "CSRRW rd=x0 writes canonical backend");
#if SPMP_COUNT > 63
    write_entry(63, address, SPMP_A_NAPOT | SPMP_U | 1);
    check(read_sireg() == address, "CSR entry 63 address");
#else
    write_siselect(ENTRY(63));
    write_sireg(address);
    write_sireg2(cfg);
    check(read_sireg() == 0 && read_sireg2() == 0,
          "unimplemented CSR entry reads zero and ignores writes");
#endif
#if SPMP_COUNT < 64
    write_siselect(ENTRY(SPMP_COUNT));
    write_sireg(address);
    write_sireg2(cfg);
    check(read_sireg() == 0 && read_sireg2() == 0,
          "configured count boundary reads zero and ignores writes");
#endif
    write_siselect(0x140);
    expect_probe((uintptr_t)probe_invalid_alias, 3, 0, CAUSE_ILLEGAL,
                 (uintptr_t)probe_invalid_alias_fault, 0x151022f3,
                 "unsupported selection faults at alias instruction");
    write_siselect(ENTRY(0));
    write_sireg2(SPMP_A_NAPOT | SPMP_SHARED | 1);
    check((read_sireg2() & SPMP_SHARED) == 0,
          "WARL clears SHARED when U is clear");
    write_sireg2(SPMP_A_NAPOT | SPMP_U | 2);
    check((read_sireg2() & 3) == 0,
          "WARL canonicalizes reserved W without R");
    expect_probe((uintptr_t)probe_reserved_alias, 3, 0, CAUSE_ILLEGAL,
                 (uintptr_t)probe_reserved_alias_fault, 0x153022f3,
                 "reserved supervisor alias faults at instruction");

    /* Lock a spare high entry so later range/protection probes stay mutable. */
    const unsigned lock_index = SPMP_COUNT - 1;
    if (lock_index == 0 || lock_index > 4) {
        write_entry(lock_index, address, SPMP_A_NAPOT | SPMP_U | 3);
        write_sireg2(SPMP_A_NAPOT | SPMP_U | 3 | SPMP_LOCK);
    }
    const uintptr_t locked_address = read_sireg();
    write_sireg(0);
    check(read_sireg() == locked_address, "locked address write ignored");
    write_sireg2(0);
    check((read_sireg2() & SPMP_LOCK) != 0, "locked config write ignored");

#if SPMP_COUNT >= 16
    const uintptr_t predecessor = (uintptr_t)__spmp_data_start >> 2;
    write_entry(13, predecessor, 0);
    write_entry(14, ((uintptr_t)__spmp_data_start + 4096) >> 2,
                (1UL << 3) | SPMP_LOCK | 1);
    write_siselect(ENTRY(13));
    write_sireg(predecessor + 1);
    check(read_sireg() == predecessor,
          "locked TOR successor protects predecessor address");
#endif
}

/* Execute permitted and denied U/S operations, ranges and MPRV probes. */
static void
test_permissions(void)
{
#if SPMP_COUNT >= 3
    /* U-only RW data is denied to S without SUM and enforced with SUM. */
    set_test_rules(3);
    expect_probe((uintptr_t)probe_u_load, 0, 0, 0, 0, 0,
                 "U load permitted by U RW rule");
    expect_probe((uintptr_t)probe_u_store, 0, 0, 0, 0, 0,
                 "U store permitted by U RW rule");
    spmp_target = 0x51a7c0de;
    spmp_sc_status = 0xffffffff;
    expect_probe((uintptr_t)probe_u_lrsc, 0, 0, 0, 0, 0,
                 "U LR/SC pair succeeds with read/write permission");
    check(spmp_target == 0x13579bdf && spmp_sc_status == 0,
          "successful U SC updates its destination");
    spmp_target = 0x51a7c0de;
    expect_probe((uintptr_t)probe_u_amo, 0, 0, 0, 0, 0,
                 "U AMO succeeds with read/write permission");
    check(spmp_target == 0x51a7c0df, "successful U AMO updates sentinel");
    expect_probe((uintptr_t)probe_s_load, 1, 0, CAUSE_LOAD_PAGE,
                 (uintptr_t)probe_s_load_fault, (uintptr_t)&spmp_target,
                 "S load from U rule denied when SUM clear");
    expect_probe((uintptr_t)probe_s_load, 1, MSTATUS_SUM, 0, 0, 0,
                 "S load from U rule permitted with SUM");
    expect_probe((uintptr_t)probe_s_load, 1, MSTATUS_SUM | MSTATUS_MXR,
                 0, 0, 0, "MXR does not change SPMP read permission");
    set_test_rules(4);
    expect_probe((uintptr_t)probe_s_load, 1, MSTATUS_SUM | MSTATUS_MXR,
                 CAUSE_LOAD_PAGE, (uintptr_t)probe_s_load_fault,
                 (uintptr_t)&spmp_target,
                 "MXR does not make SPMP execute-only data readable");

    set_test_rules(SPMP_SHARED | 3);
    expect_probe((uintptr_t)probe_u_load, 0, 0, 0, 0, 0,
                 "shared RW gives U read-only exception");
    expect_probe((uintptr_t)probe_u_store, 0, 0, CAUSE_STORE_PAGE,
                 (uintptr_t)probe_u_store_fault, (uintptr_t)&spmp_target,
                 "shared RW denies U store");
    expect_probe((uintptr_t)probe_s_store, 1, 0, 0, 0, 0,
                 "shared RW ignores SUM for S store");
    set_test_rules(SPMP_SHARED | 7);
    expect_probe((uintptr_t)probe_u_exec_page, 0, 0, 0, 0, 0,
                 "shared RWX permits U instruction execution");
    expect_probe((uintptr_t)probe_u_target_load, 0, 0, CAUSE_LOAD_PAGE,
                 (uintptr_t)probe_u_target_load_fault, (uintptr_t)&spmp_target,
                 "shared RWX denies U data reads as execute-only");
    expect_probe((uintptr_t)probe_u_load, 0, 0, CAUSE_LOAD_PAGE,
                 (uintptr_t)probe_u_load_fault, (uintptr_t)&spmp_target,
                 "shared RWX gives U execute-only exception");
    expect_probe((uintptr_t)probe_u_store, 0, 0, CAUSE_STORE_PAGE,
                 (uintptr_t)probe_u_store_fault, (uintptr_t)&spmp_target,
                 "shared RWX denies U data store");

    /* The target sentinel proves denied single stores and AMOs had no effect. */
    set_test_rules(1);
    spmp_target = 0x51a7c0de;
    fence_spmp();
    expect_probe((uintptr_t)probe_u_store, 0, 0, CAUSE_STORE_PAGE,
                 (uintptr_t)probe_u_store_fault, (uintptr_t)&spmp_target,
                 "U store denied with cause 15");
    check(spmp_target == 0x51a7c0de, "denied store preserves sentinel");
    expect_probe((uintptr_t)probe_u_amo, 0, 0, CAUSE_STORE_PAGE,
                 (uintptr_t)probe_u_amo_fault, (uintptr_t)&spmp_target,
                 "U AMO denied with cause 15");
    check(spmp_target == 0x51a7c0de, "denied AMO preserves sentinel");
    expect_probe((uintptr_t)probe_u_sc, 0, 0, CAUSE_STORE_PAGE,
                 (uintptr_t)probe_u_sc_fault, (uintptr_t)&spmp_target,
                 "U SC denied with cause 15");
    set_test_rules(0);
    expect_probe((uintptr_t)probe_u_lr, 0, 0, CAUSE_LOAD_PAGE,
                 (uintptr_t)probe_u_lr_fault, (uintptr_t)&spmp_target,
                 "U LR denied with cause 13");

    /* Execute denial is observed at the target instruction address. */
    write_entry(0, napot((uintptr_t)__ucode_start, 4096),
                SPMP_A_NAPOT | SPMP_U);
    fence_spmp();
    expect_probe((uintptr_t)probe_u_load, 0, 0, CAUSE_INST_PAGE,
                 (uintptr_t)probe_u_load, (uintptr_t)probe_u_load,
                 "U instruction fetch denied with cause 12");

    /* TOR and NA4 check lower/upper bytes and reject a straddling access. */
    for (unsigned i = 0; i < SPMP_COUNT; ++i) {
        write_siselect(ENTRY(i));
        write_sireg2(0);
    }
    write_entry(0, (uintptr_t)&spmp_target >> 2, 0);
    write_entry(1, ((uintptr_t)&spmp_target + 8) >> 2,
                (1UL << 3) | SPMP_U | 1);
    write_entry(2, napot((uintptr_t)__ucode_start, 4096),
                SPMP_A_NAPOT | SPMP_U | 4);
    fence_spmp();
    expect_probe((uintptr_t)probe_u_load, 0, 0, 0, 0, 0,
                 "TOR lower bound permits a complete load");
    expect_probe((uintptr_t)probe_u_tor_last, 0, 0, 0, 0, 0,
                 "TOR last byte is included");
    expect_probe((uintptr_t)probe_u_tor_cross, 0, 0, CAUSE_LOAD_PAGE,
                 (uintptr_t)probe_u_tor_cross_fault,
                 (uintptr_t)&spmp_target + 7,
                 "TOR partial overlap denies whole access");
    expect_probe((uintptr_t)probe_u_tor_after, 0, 0, CAUSE_LOAD_PAGE,
                 (uintptr_t)probe_u_tor_after_fault,
                 (uintptr_t)&spmp_target + 8,
                 "TOR upper bound is excluded");
    set_test_rules(1);
    write_entry(1, (uintptr_t)&spmp_target >> 2,
                (2UL << 3) | SPMP_U | 1);
    fence_spmp();
    expect_probe((uintptr_t)probe_u_load, 0, 0, 0, 0, 0,
                 "NA4 grants exact four-byte region");
    expect_probe((uintptr_t)probe_u_na4_cross, 0, 0, CAUSE_LOAD_PAGE,
                 (uintptr_t)probe_u_na4_cross_fault,
                 (uintptr_t)&spmp_target + 3,
                 "NA4 partial overlap denies whole access");

    /* Check all R/W/X values using named permission bits. */
    for (unsigned rwx = 0; rwx < 8; ++rwx) {
        const bool read_ok = (rwx & 1) != 0;
        const bool write_ok = (rwx & 1) && (rwx & 2);
        set_test_rules(rwx);
        expect_probe((uintptr_t)probe_u_load, 0, 0,
                     read_ok ? 0 : CAUSE_LOAD_PAGE,
                     read_ok ? 0 : (uintptr_t)probe_u_load_fault,
                     read_ok ? 0 : (uintptr_t)&spmp_target,
                     "U read follows canonical R permission");
        expect_probe((uintptr_t)probe_u_store, 0, 0,
                     write_ok ? 0 : CAUSE_STORE_PAGE,
                     write_ok ? 0 : (uintptr_t)probe_u_store_fault,
                     write_ok ? 0 : (uintptr_t)&spmp_target,
                     "U write follows canonical W permission");
        write_entry(1, napot((uintptr_t)__spmp_data_start, 4096),
                    SPMP_A_NAPOT | SPMP_U | 1);
        write_entry(0, napot((uintptr_t)__ucode_start, 4096),
                    SPMP_A_NAPOT | SPMP_U | (rwx & 4));
        fence_spmp();
        expect_probe((uintptr_t)probe_u_load, 0, 0,
                     (rwx & 4) ? 0 : CAUSE_INST_PAGE,
                     (rwx & 4) ? 0 : (uintptr_t)probe_u_load,
                     (rwx & 4) ? 0 : (uintptr_t)probe_u_load,
                     "U instruction fetch follows X permission");
        write_entry(0, napot((uintptr_t)__scode_start, 4096),
                    SPMP_A_NAPOT | SPMP_U | (rwx & 4));
        fence_spmp();
        expect_probe((uintptr_t)probe_s_load, 1, MSTATUS_SUM,
                     CAUSE_INST_PAGE, (uintptr_t)probe_s_load,
                     (uintptr_t)probe_s_load,
                     "U-only rule always denies S instruction fetch");
        set_test_rules(rwx);
    }

    /* MPRV changes data privilege; instruction fetch stays M and succeeds. */
    set_test_rules(0);
    expect_mprv_probe((uintptr_t)probe_m_load, 0, MSTATUS_MPRV,
                 CAUSE_LOAD_PAGE, (uintptr_t)probe_m_load_fault,
                 (uintptr_t)&spmp_target,
                 "MPRV MPP=U data load uses U SPMP permissions");
    expect_mprv_probe((uintptr_t)probe_m_load, 1, MSTATUS_MPRV | MSTATUS_SUM,
                 CAUSE_LOAD_PAGE, (uintptr_t)probe_m_load_fault,
                 (uintptr_t)&spmp_target,
                 "MPRV MPP=S data load enforces SPMP");
    expect_mprv_probe((uintptr_t)probe_m_load, 3, MSTATUS_MPRV,
                 0, 0, 0, "MPRV MPP=M data load bypasses SPMP");
    expect_mprv_probe((uintptr_t)probe_m_load, 0, 0,
                 0, 0, 0, "MPRV off ignores MPP=U for M data access");
    expect_mprv_probe((uintptr_t)probe_m_load, 1, 0,
                 0, 0, 0, "MPRV off ignores MPP=S for M data access");
    expect_probe((uintptr_t)probe_m_load, 3, 0, 0, 0, 0,
                 "M data load bypasses SPMP");
    set_test_rules(1);
    expect_mprv_probe((uintptr_t)probe_m_load, 1, MSTATUS_MPRV,
                 CAUSE_LOAD_PAGE, (uintptr_t)probe_m_load_fault,
                 (uintptr_t)&spmp_target,
                 "MPRV effective S denies a U read-only rule with SUM clear");
    expect_mprv_probe((uintptr_t)probe_m_load, 1, MSTATUS_MPRV | MSTATUS_SUM,
                 0, 0, 0, "MPRV effective S permits U read-only rule with SUM set");
    expect_mprv_probe((uintptr_t)probe_m_load, 0, MSTATUS_MPRV,
                 0, 0, 0, "MPRV effective U permits U read-only rule");
#else
    semihost_write0("SPMP_CASE_SKIPPED: permission probes require at least 3 entries\n");
#endif
}

/* Distinguish a denied whole store from permitted earlier split fragments. */
static void
test_split_store(void)
{
#if SPMP_FEATURE && SPMP_COUNT >= 4
    check((uintptr_t)spmp_split == 0x8000603cUL, "split buffer linker address");
    for (unsigned i = 0; i < 8; ++i)
        spmp_split[i] = 0xaa;
    set_test_rules(3);
    write_entry(1, 0x80006040UL >> 2, (2UL << 3) | SPMP_U | 1);
    write_entry(2, napot((uintptr_t)__spmp_data_start, 4096),
                SPMP_A_NAPOT | SPMP_U | 3);
#if TEST_XLEN == 64
    const uintptr_t physical_mask = (1UL << 54) - 1;
#else
    const uintptr_t physical_mask = 0xffffffffUL;
#endif
    write_entry(3, physical_mask, SPMP_A_NAPOT | 7);
    fence_spmp();
    prepare_expectation(CAUSE_STORE_PAGE, (uintptr_t)probe_u_split_store_fault,
                        0x8000603eUL);
    spmp_expected_tval_alt = 0x80006040UL;
    uintptr_t status = read_mstatus() &
        ~(MSTATUS_MPP_MASK | MSTATUS_MPRV | MSTATUS_SUM | MSTATUS_MXR);
    spmp_active = 1;
    run_probe((uintptr_t)probe_u_split_store, status, 0);
    spmp_active = 0;
    check(spmp_seen == 1 && spmp_failure == 0, "split store precise trap");
    check(spmp_split[0] == 0xaa && spmp_split[1] == 0xaa &&
          spmp_split[4] == 0xaa && spmp_split[5] == 0xaa &&
          spmp_split[6] == 0xaa && spmp_split[7] == 0xaa,
          "split store never modifies denied bytes or guard bytes");
    const bool unchanged = spmp_split[2] == 0xaa && spmp_split[3] == 0xaa;
    const bool earlier_fragment = spmp_split[2] == 0x44 && spmp_split[3] == 0x33;
    check(unchanged || earlier_fragment, "split store allowed first-fragment effects");
    if (spmp_observed[1] == 0x8000603eUL) {
        check(unchanged, "denied whole request has no store effect");
        semihost_write0("SPMP_SPLIT_WHOLE_REQUEST_DENIED\n");
    } else {
        semihost_write0("SPMP_SPLIT_SUBREQUEST_DENIED\n");
    }
#endif
}

/* Check live data/fetch permissions after explicit full memory-management fences. */
static void
test_fences(void)
{
#if SPMP_FEATURE && SPMP_COUNT >= 3
    set_test_rules(1);
    expect_probe((uintptr_t)probe_u_load, 0, 0, 0, 0, 0,
                 "fence warm data and instruction caches");
    write_siselect(ENTRY(1));
    write_sireg2(SPMP_A_NAPOT | SPMP_U);
    fence_spmp();
    expect_probe((uintptr_t)probe_u_load, 0, 0, CAUSE_LOAD_PAGE,
                 (uintptr_t)probe_u_load_fault, (uintptr_t)&spmp_target,
                 "fence data allow-to-deny");
    write_sireg2(SPMP_A_NAPOT | SPMP_U | 1);
    fence_spmp();
    expect_probe((uintptr_t)probe_u_load, 0, 0, 0, 0, 0,
                 "fence data deny-to-allow");
    write_siselect(ENTRY(0));
    write_sireg2(SPMP_A_NAPOT | SPMP_U);
    fence_spmp();
    expect_probe((uintptr_t)probe_u_load, 0, 0, CAUSE_INST_PAGE,
                 (uintptr_t)probe_u_load, (uintptr_t)probe_u_load,
                 "fence fetch allow-to-deny");
    write_sireg2(SPMP_A_NAPOT | SPMP_U | 4);
    fence_spmp();
    expect_probe((uintptr_t)probe_u_load, 0, 0, 0, 0, 0,
                 "fence fetch deny-to-allow");
#endif
}

/* Deny one whole page in PMP while leaving code and page-table traffic valid. */
static void
pmp_deny_page(uintptr_t base)
{
    const uintptr_t denied = napot(base, 4096);
#if TEST_XLEN == 32
    const uintptr_t all = 0xffffffffUL;
#else
    const uintptr_t all = (1UL << 54) - 1;
#endif
    __asm__ volatile("csrw 0x3b0, %0" :: "r"(denied) : "memory");
    __asm__ volatile("csrw 0x3b1, %0" :: "r"(all) : "memory");
    __asm__ volatile("csrw 0x3a0, %0" :: "r"((uintptr_t)0x1f18) : "memory");
}

/* Observe SPMP-before-PMP faults on real aligned CPU requests of each type. */
static void
test_fault_priority(void)
{
#if SPMP_FEATURE && SPMP_COUNT >= 3
    set_test_rules(0);
    pmp_deny_page((uintptr_t)__spmp_data_start);
    expect_probe((uintptr_t)probe_u_load, 0, 0, CAUSE_LOAD_PAGE,
                 (uintptr_t)probe_u_load_fault, (uintptr_t)&spmp_target,
                 "priority SPMP load page fault precedes PMP access fault");
    expect_probe((uintptr_t)probe_u_store, 0, 0, CAUSE_STORE_PAGE,
                 (uintptr_t)probe_u_store_fault, (uintptr_t)&spmp_target,
                 "priority SPMP store page fault precedes PMP access fault");
    write_pmp_baseline();
    write_siselect(ENTRY(0));
    write_sireg2(SPMP_A_NAPOT | SPMP_U);
    fence_spmp();
    pmp_deny_page((uintptr_t)__ucode_start);
    expect_probe((uintptr_t)probe_u_load, 0, 0, CAUSE_INST_PAGE,
                 (uintptr_t)probe_u_load, (uintptr_t)probe_u_load,
                 "priority SPMP fetch page fault precedes PMP access fault");
    write_pmp_baseline();
#endif
}

/* Keep SUM and MXR writable through their architectural supervisor alias. */
static void
test_status_bits(void)
{
    uintptr_t original, value;
    __asm__ volatile("csrr %0, sstatus" : "=r"(original));
    value = original | MSTATUS_SUM | MSTATUS_MXR;
    __asm__ volatile("csrw sstatus, %0" :: "r"(value) : "memory");
    __asm__ volatile("csrr %0, sstatus" : "=r"(value));
    check((value & (MSTATUS_SUM | MSTATUS_MXR)) == (MSTATUS_SUM | MSTATUS_MXR),
          "status SUM and MXR can be set on this XLEN");
    value = original & ~(MSTATUS_SUM | MSTATUS_MXR);
    __asm__ volatile("csrw sstatus, %0" :: "r"(value) : "memory");
    __asm__ volatile("csrr %0, sstatus" : "=r"(value));
    check(!(value & (MSTATUS_SUM | MSTATUS_MXR)),
          "status SUM and MXR can be cleared on this XLEN");
    __asm__ volatile("csrw sstatus, %0" :: "r"(original) : "memory");
}

/* Walk a real Sv39 superpage, retain PMP/page faults, then return to Bare. */
static void
test_paging(void)
{
#if TEST_XLEN == 64 && SPMP_FEATURE && SPMP_COUNT >= 3
    set_test_rules(0);
    for (unsigned i = 0; i < 512; ++i)
        sv39_root[i] = 0;
    // S-only RWX leaf, A/D already set, identity-mapped 0x80000000..0xbfffffff.
    sv39_root[2] = (0x80000000UL >> 2) | 0xcf;
    __asm__ volatile("fence rw, rw" ::: "memory");
    const uintptr_t satp = (8UL << 60) | ((uintptr_t)sv39_root >> 12);
    __asm__ volatile("csrw 0x180, %0" :: "r"(satp) : "memory");
    fence_spmp();
    for (unsigned i = 0; i < 2; ++i)
        expect_probe((uintptr_t)probe_s_load, 1, 0, 0, 0, 0,
                     "paging Sv39 miss/hit ignores restrictive SPMP");
    fence_spmp();
    expect_probe((uintptr_t)probe_s_load, 1, 0, 0, 0, 0,
                 "paging Sv39 walk after fence ignores restrictive SPMP");
    // Code and PTE reads still pass PMP; only the data page is denied.
    pmp_deny_page((uintptr_t)__spmp_data_start);
    expect_probe((uintptr_t)probe_s_load, 1, 0, CAUSE_LOAD_ACCESS,
                 (uintptr_t)probe_s_load_fault, (uintptr_t)&spmp_target,
                 "paging PMP remains active with restrictive SPMP");
    write_pmp_baseline();
    sv39_root[2] = (0x80000000UL >> 2) | 0xcd; // Reserved W=1,R=0 leaf.
    __asm__ volatile("fence rw, rw" ::: "memory");
    fence_spmp();
    expect_probe((uintptr_t)probe_s_load, 1, 0, CAUSE_INST_PAGE,
                 (uintptr_t)probe_s_load, (uintptr_t)probe_s_load,
                 "paging invalid PTE still raises instruction page fault");
    __asm__ volatile("csrw 0x180, zero" ::: "memory");
    fence_spmp();
    expect_probe((uintptr_t)probe_s_load, 1, 0, CAUSE_LOAD_PAGE,
                 (uintptr_t)probe_s_load_fault, (uintptr_t)&spmp_target,
                 "paging returning to Bare reactivates SPMP");
#endif
}

/* Configure two locks and a TOR predecessor before the first simulator exit. */
static void
configure_checkpoint_state(void)
{
#if SPMP_FEATURE && SPMP_COUNT >= 8
    set_test_rules(3);
    write_siselect(ENTRY(2));
    write_sireg2(0);
    write_entry(3, (uintptr_t)__spmp_locked_start >> 2, 0);
    write_entry(4, ((uintptr_t)__spmp_locked_start + 4096) >> 2,
                (1UL << 3) | SPMP_LOCK | SPMP_U | 1);
    write_entry(5, napot(0x8000b000UL, 4096),
                SPMP_A_NAPOT | SPMP_LOCK | SPMP_U | 1);
#if TEST_XLEN == 64
    const uintptr_t physical_mask = (1UL << 54) - 1;
#else
    const uintptr_t physical_mask = 0xffffffffUL;
#endif
    write_entry(6, physical_mask, SPMP_A_NAPOT | 7);
    write_siselect(ENTRY(2));
    fence_spmp();
#endif
}

/* Observe transferred state before writes, then prove retained permissions. */
static void
verify_checkpoint_state(void)
{
#if SPMP_FEATURE && SPMP_COUNT >= 8
    check(read_siselect() == ENTRY(2), "takeover/checkpoint selector retained");
    write_siselect(ENTRY(3));
    const uintptr_t predecessor = (uintptr_t)__spmp_locked_start >> 2;
    check(read_sireg() == predecessor && read_sireg2() == 0,
          "takeover/checkpoint TOR predecessor retained");
    write_sireg(predecessor + 1);
    check(read_sireg() == predecessor,
          "takeover/checkpoint locked TOR predecessor ignores writes");
    write_siselect(ENTRY(4));
    check(read_sireg() == (((uintptr_t)__spmp_locked_start + 4096) >> 2) &&
          read_sireg2() == ((1UL << 3) | SPMP_LOCK | SPMP_U | 1),
          "takeover/checkpoint locked TOR entry retained");
    write_siselect(ENTRY(5));
    check(read_sireg() == napot(0x8000b000UL, 4096) &&
          read_sireg2() == (SPMP_A_NAPOT | SPMP_LOCK | SPMP_U | 1),
          "takeover/checkpoint second locked entry retained");
    write_sireg2(0);
    check(read_sireg2() == (SPMP_A_NAPOT | SPMP_LOCK | SPMP_U | 1),
          "takeover/checkpoint config lock still enforced");
    /* Drain may execute past m5_exit; establish PMP after the state probes. */
    write_pmp_baseline();
    expect_probe((uintptr_t)probe_u_locked_load, 0, 0, 0, 0, 0,
                 "takeover/checkpoint retains locked TOR read permission");
    expect_probe((uintptr_t)probe_u_locked_store, 0, 0, CAUSE_STORE_PAGE,
                 (uintptr_t)probe_u_locked_store_fault,
                 (uintptr_t)&spmp_locked_target,
                 "takeover/checkpoint retains locked TOR store denial");
    check(spmp_locked_target == 0x7150cafe,
          "takeover/checkpoint denied store preserves locked sentinel");
#endif
}

/* Run ordered cases and stop at two bounded simulator phases. */
void
spmp_test_main(void)
{
    write_pmp_baseline();
    test_status_bits();
#if SPMP_FEATURE && TEST_XLEN == 32
    uintptr_t satp = 0x80000000UL;
    __asm__ volatile("csrw 0x180, %0" :: "r"(satp) : "memory");
    __asm__ volatile("csrr %0, 0x180" : "=r"(satp));
    check(satp == 0, "enabled RV32 SATP remains Bare without Sv32 support");
#endif
#if SPMP_FEATURE && SPMP_COUNT > 0
    expect_probe((uintptr_t)probe_u_load, 0, 0, CAUSE_INST_PAGE,
                 (uintptr_t)probe_u_load, (uintptr_t)probe_u_load,
                 "Bare empty active table denies instruction fetch");
    test_permissions();
    test_split_store();
    test_fences();
    test_fault_priority();
    test_paging();
    test_csr_programming();
#else
    /* Disabled aliases are illegal; enabled zero-count aliases are zero/ignored. */
    write_siselect(ENTRY(0));
#if SPMP_FEATURE
    check(read_sireg() == 0 && read_sireg2() == 0,
          "zero-entry SPMP aliases are zero");
    write_sireg(1);
    write_sireg2(1);
    check(read_sireg() == 0 && read_sireg2() == 0,
          "zero-entry SPMP aliases ignore writes");
#else
    expect_probe((uintptr_t)probe_invalid_alias, 3, 0, CAUSE_ILLEGAL,
                 (uintptr_t)probe_invalid_alias_fault, 0x151022f3,
                 "disabled feature aliases are illegal instructions");
#endif
    expect_probe((uintptr_t)probe_u_load, 0, 0, 0, 0, 0,
                 "inactive SPMP preserves U Bare loads and fetches");
    expect_probe((uintptr_t)probe_u_store, 0, 0, 0, 0, 0,
                 "inactive SPMP preserves U Bare stores");
#endif
    configure_checkpoint_state();
    semihost_hex("SPMP_CASES_BEFORE_PHASE=", spmp_case_id);
    semihost_write0("SPMP_PHASE_READY\n");
    m5_exit();

    verify_checkpoint_state();
    semihost_write0("SPMP_WORKLOAD_PASS\n");
    m5_exit();
    for (;;) {}
}
