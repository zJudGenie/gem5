/* Shared CSR definitions, runtime hardware discovery, and trap assertions for SPMP tests. */
#ifndef SPMP_HARNESS_H
#define SPMP_HARNESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CSR_SISELECT  0x150
#define CSR_SIREG     0x151
#define CSR_SIREG2    0x152
#define CSR_SIREG3    0x153
#define CSR_SATP      0x180
#define CSR_SPMPEN    0x183
#define CSR_SPMPENH   0x193
#define CSR_MPMPDELEG 0x316
#define CSR_MISELECT  0x350
#define CSR_MIREG     0x351
#define CSR_MIREG2    0x352
#define CSR_PMPCFG0   0x3a0
#define CSR_PMPADDR0  0x3b0

#define CAUSE_ILLEGAL     2
#define CAUSE_LOAD_ACCESS 5
#define CAUSE_ECALL_U     8
#define CAUSE_ECALL_S     9
#define CAUSE_ECALL_M     11
#define CAUSE_INST_PAGE   12
#define CAUSE_LOAD_PAGE   13
#define CAUSE_STORE_PAGE  15

#define PRV_U 0UL
#define PRV_S 1UL
#define PRV_M 3UL

#define MSTATUS_MPP_MASK (3UL << 11)
#define MSTATUS_MPRV     (1UL << 17)
#define MSTATUS_SUM      (1UL << 18)
#define MSTATUS_MXR      (1UL << 19)

#define SPMP_R       (1UL << 0)
#define SPMP_W       (1UL << 1)
#define SPMP_X       (1UL << 2)
#define SPMP_A_OFF   (0UL << 3)
#define SPMP_A_TOR   (1UL << 3)
#define SPMP_A_NA4   (2UL << 3)
#define SPMP_A_NAPOT (3UL << 3)
#define SPMP_LOCK    (1UL << 7)
#define SPMP_U       (1UL << 8)
#define SPMP_SHARED  (1UL << 9)

#define ENTRY(i) (0x100UL + (uintptr_t)(i))

#if __riscv_xlen == 32
#define PHYS_ADDR_MASK 0xffffffffUL
#else
#define PHYS_ADDR_MASK ((1UL << 54) - 1)
#endif

struct spmp_hw {
    bool enabled;
    bool has_spmpen;
    bool has_delegation;
    unsigned count;
};

extern char __scode_start[], __ucode_start[], __spmp_data_start[], __spmp_locked_start[];
extern volatile uint32_t spmp_target;
extern volatile uint32_t spmp_sc_status;
extern volatile uint32_t spmp_locked_target;
extern volatile uint8_t spmp_split[8];

extern volatile uintptr_t spmp_saved_sp;
extern volatile uintptr_t spmp_probe_active;
extern volatile uintptr_t spmp_trap_seen;
extern volatile uintptr_t spmp_trap_cause;
extern volatile uintptr_t spmp_trap_tval;
extern volatile uintptr_t spmp_trap_epc;
extern volatile uintptr_t spmp_case_id;

void run_probe(uintptr_t fn, uintptr_t status, uintptr_t direct_m,
               uintptr_t arg0, uintptr_t arg1);

/* Reusable probe functions placed in dedicated linker sections. */
void probe_u_lw(uintptr_t addr, uintptr_t unused);
void probe_u_lhu(uintptr_t addr, uintptr_t unused);
void probe_u_lbu(uintptr_t addr, uintptr_t unused);
void probe_u_sw(uintptr_t addr, uintptr_t val);
void probe_u_lr(uintptr_t addr, uintptr_t unused);
void probe_u_sc(uintptr_t addr, uintptr_t val);
void probe_u_lrsc(uintptr_t addr, uintptr_t val);
void probe_u_amoadd(uintptr_t addr, uintptr_t val);
void probe_u_csrr_siselect(uintptr_t unused0, uintptr_t unused1);
void probe_u_csrr_spmpen(uintptr_t unused0, uintptr_t unused1);

void probe_s_lw(uintptr_t addr, uintptr_t unused);
void probe_s_sw(uintptr_t addr, uintptr_t val);
void probe_s_write_spmpen(uintptr_t val, uintptr_t unused);
void probe_s_clear_lock(uintptr_t entry_sel, uintptr_t unused);
void probe_s_csrr_miselect(uintptr_t unused0, uintptr_t unused1);

void probe_u_exec_target(uintptr_t unused0, uintptr_t unused1);
void probe_u_exec_target_lw(uintptr_t addr, uintptr_t unused);

void probe_m_lw(uintptr_t addr, uintptr_t unused);
void probe_m_csrr(uintptr_t csr_num, uintptr_t unused);

/* Semihosting and simulation lifecycle helpers. */
void semihost_write0(const char *message);
void semihost_hex(const char *label, uintptr_t value);
void m5_exit(void);
void m5_fail(void);
int check(int condition, const char *name);
void spmp_finish_phase1_and_exit(void);
void spmp_pass_and_exit(void);

/* Trap assertion wrappers for S/U mret probes and M-mode MPRV probes. */
void expect_ok(void (*fn)(uintptr_t, uintptr_t), uintptr_t arg0, uintptr_t arg1,
               uintptr_t priv, uintptr_t status_flags, const char *name);
void expect_trap(void (*fn)(uintptr_t, uintptr_t), uintptr_t arg0, uintptr_t arg1,
                 uintptr_t priv, uintptr_t status_flags,
                 uintptr_t expected_cause, uintptr_t expected_tval,
                 const char *name);
void expect_mprv_ok(void (*fn)(uintptr_t, uintptr_t), uintptr_t arg0, uintptr_t arg1,
                    uintptr_t mpp, uintptr_t status_flags, const char *name);
void expect_mprv_trap(void (*fn)(uintptr_t, uintptr_t), uintptr_t arg0, uintptr_t arg1,
                      uintptr_t mpp, uintptr_t status_flags,
                      uintptr_t expected_cause, uintptr_t expected_tval,
                      const char *name);

/* Runtime hardware feature and entry-count discovery. */
void spmp_discover_hw(struct spmp_hw *hw);

/* Standard test rules: entry 0 = U code (X), entry 1 = U data (data_cfg), entry 2 = S all (RWX). */
void set_test_rules(uintptr_t data_cfg);
void clear_all_spmp_entries(unsigned count);
void write_pmp_baseline(void);
void pmp_deny_page(uintptr_t base);

static inline void
write_siselect(uintptr_t value)
{
    __asm__ volatile("csrw 0x150, %0" :: "r"(value) : "memory");
}

static inline uintptr_t
read_siselect(void)
{
    uintptr_t value;
    __asm__ volatile("csrr %0, 0x150" : "=r"(value));
    return value;
}

static inline void
write_sireg(uintptr_t value)
{
    __asm__ volatile("csrw 0x151, %0" :: "r"(value) : "memory");
}

static inline uintptr_t
read_sireg(void)
{
    uintptr_t value;
    __asm__ volatile("csrr %0, 0x151" : "=r"(value));
    return value;
}

static inline void
write_sireg2(uintptr_t value)
{
    __asm__ volatile("csrw 0x152, %0" :: "r"(value) : "memory");
}

static inline uintptr_t
read_sireg2(void)
{
    uintptr_t value;
    __asm__ volatile("csrr %0, 0x152" : "=r"(value));
    return value;
}

static inline void
write_miselect(uintptr_t value)
{
    __asm__ volatile("csrw 0x350, %0" :: "r"(value) : "memory");
}

static inline uintptr_t
read_miselect(void)
{
    uintptr_t value;
    __asm__ volatile("csrr %0, 0x350" : "=r"(value));
    return value;
}

static inline void
write_mireg(uintptr_t value)
{
    __asm__ volatile("csrw 0x351, %0" :: "r"(value) : "memory");
}

static inline uintptr_t
read_mireg(void)
{
    uintptr_t value;
    __asm__ volatile("csrr %0, 0x351" : "=r"(value));
    return value;
}

static inline void
write_mireg2(uintptr_t value)
{
    __asm__ volatile("csrw 0x352, %0" :: "r"(value) : "memory");
}

static inline uintptr_t
read_mireg2(void)
{
    uintptr_t value;
    __asm__ volatile("csrr %0, 0x352" : "=r"(value));
    return value;
}

static inline uintptr_t
read_spmpen(void)
{
    uintptr_t value;
    __asm__ volatile("csrr %0, 0x183" : "=r"(value));
    return value;
}

static inline void
write_spmpen(uintptr_t value)
{
    __asm__ volatile("csrw 0x183, %0" :: "r"(value) : "memory");
}

static inline uintptr_t
read_mstatus(void)
{
    uintptr_t value;
    __asm__ volatile("csrr %0, mstatus" : "=r"(value));
    return value;
}

static inline uintptr_t
napot(uintptr_t base, uintptr_t size)
{
    return (base >> 2) | ((size >> 3) - 1);
}

static inline void
write_entry(unsigned index, uintptr_t address, uintptr_t config)
{
    write_siselect(ENTRY(index));
    write_sireg(address);
    write_sireg2(config);
}

static inline void
fence_spmp(void)
{
    __asm__ volatile(".word 0x12000073" ::: "memory");
}

#endif /* SPMP_HARNESS_H */
