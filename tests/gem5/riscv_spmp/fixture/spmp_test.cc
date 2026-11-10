#include "spmp_test.hh"

#include <memory>

#include "arch/riscv/faults.hh"
#include "arch/riscv/memflags.hh"
#include "arch/riscv/mmu.hh"
#include "arch/riscv/pagetable.hh"
#include "arch/riscv/regs/misc.hh"
#include "base/bitfield.hh"
#include "base/logging.hh"
#include "cpu/base.hh"
#include "cpu/null_static_inst.hh"
#include "cpu/thread_context.hh"
#include "params/RiscvSPMPTest.hh"
#include "sim/full_system.hh"

namespace gem5
{
namespace RiscvISA
{

namespace
{

constexpr Addr TestBase = 0x80001000;
constexpr Addr TestVaddr = 0x40001000;
constexpr uint64_t CfgR = 1;
constexpr uint64_t CfgW = 2;
constexpr uint64_t CfgX = 4;
constexpr uint64_t CfgTOR = 1 << 3;
constexpr uint64_t CfgNA4 = 2 << 3;
constexpr uint64_t CfgNAPOT = 3 << 3;
constexpr uint64_t CfgL = 1 << 7;
constexpr uint64_t CfgU = 1 << 8;
constexpr uint64_t CfgShared = 1 << 9;

/** Stop the fixture on the first failed named expectation. */
void
expect(bool condition, const char *caseId)
{
    fatal_if(!condition, "RiscvSPMPTest failed case %s", caseId);
}

/** Build a virtual CPU request and optionally assign its translated address. */
RequestPtr
request(BaseCPU *cpu, ThreadContext *tc, Addr vaddr, Addr paddr,
        unsigned size, BaseMMU::Mode mode, Request::Flags flags = 0,
        RequestorID requestor = Request::invldRequestorId)
{
    const RequestorID id = requestor == Request::invldRequestorId ?
        (mode == BaseMMU::Execute ? cpu->instRequestorId() :
                                   cpu->dataRequestorId()) : requestor;
    auto req = std::make_shared<Request>(vaddr, size, flags, id, 0,
                                         tc->contextId());
    if (paddr != MaxAddr)
        req->setPaddr(paddr);
    return req;
}

/** Assert a page-fault cause and tval while restoring trap side effects. */
void
expectFault(Fault fault, ExceptionCode cause, Addr tval,
            ThreadContext *tc, const char *caseId)
{
    expect(fault != NoFault, caseId);
    const auto pc = std::unique_ptr<PCStateBase>(tc->pcState().clone());
    const RegVal status = tc->readMiscRegNoEffect(MISCREG_STATUS);
    const RegVal prv = tc->readMiscRegNoEffect(MISCREG_PRV);
    const RegVal medeleg = tc->readMiscRegNoEffect(MISCREG_MEDELEG);
    const RegVal mtvec = tc->readMiscRegNoEffect(MISCREG_MTVEC);
    const RegVal mepc = tc->readMiscRegNoEffect(MISCREG_MEPC);
    const RegVal mcause = tc->readMiscRegNoEffect(MISCREG_MCAUSE);
    const RegVal mtval = tc->readMiscRegNoEffect(MISCREG_MTVAL);

    tc->setMiscRegNoEffect(MISCREG_MEDELEG, 0);
    fault->invoke(tc, nullStaticInstPtr);
    expect((tc->readMiscRegNoEffect(MISCREG_MCAUSE) & 0xfff) ==
           static_cast<RegVal>(cause), caseId);
    expect(tc->readMiscRegNoEffect(MISCREG_MEPC) == pc->instAddr(), caseId);
    expect(tc->readMiscRegNoEffect(MISCREG_MTVAL) == tval, caseId);

    tc->setMiscRegNoEffect(MISCREG_STATUS, status);
    tc->setMiscRegNoEffect(MISCREG_PRV, prv);
    tc->setMiscRegNoEffect(MISCREG_MEDELEG, medeleg);
    tc->setMiscRegNoEffect(MISCREG_MTVEC, mtvec);
    tc->setMiscRegNoEffect(MISCREG_MEPC, mepc);
    tc->setMiscRegNoEffect(MISCREG_MCAUSE, mcause);
    tc->setMiscRegNoEffect(MISCREG_MTVAL, mtval);
    tc->pcState(*pc);
}

/** Clear active entries while retaining each configured address for later probes. */
void
clearEntries(SPMP *spmp)
{
    spmp->spmpReset();
    for (uint32_t i = 0; i < spmp->getNumEntries(); ++i)
        spmp->spmpUpdateCfg(i, 0);
}

/** Match one direct backend access at a physical address. */
Fault
check(SPMP *spmp, ThreadContext *tc, Addr paddr, unsigned size,
      BaseMMU::Mode mode, PrivilegeMode privilege, Addr vaddr = TestVaddr,
      Request::Flags flags = 0)
{
    auto req = request(tc->getCpuPtr(), tc, vaddr, paddr, size, mode, flags);
    return spmp->spmpCheck(req, mode, privilege, tc, vaddr);
}

/** Configure one simple NAPOT entry for a power-of-two range. */
void
napot(SPMP *spmp, uint32_t index, Addr base, Addr size, uint64_t permissions)
{
    const Addr raw = (base >> 2) | ((size >> 3) - 1);
    expect(spmp->spmpUpdateAddr(index, raw), "range.napot.address");
    expect(spmp->spmpUpdateCfg(index, permissions | CfgNAPOT),
           "range.napot.config");
}

/** Compare named Figure 4 outcomes using explicit per-encoding access masks. */
void
permissionTable(SPMP *spmp, ThreadContext *tc)
{
    constexpr uint8_t Read = 1 << 0;
    constexpr uint8_t Write = 1 << 1;
    constexpr uint8_t Execute = 1 << 2;
    struct Row {
        uint64_t packed;
        uint8_t ordinary;
        uint8_t sharedUser;
    };
    // The third column records the special shared-U encodings directly:
    // RW is read-only and RWX is execute-only. The other legal rows enforce
    // their ordinary permission bits.
    const Row rows[] = {
        {0, 0, 0},
        {CfgX, Execute, Execute},
        {CfgR, Read, Read},
        {CfgR | CfgX, Read | Execute, Read | Execute},
        {CfgR | CfgW, Read | Write, Read},
        {CfgR | CfgW | CfgX, Read | Write | Execute, Execute},
    };
    for (const Row &row : rows) {
        expect(spmp->spmpUpdateAddr(0, TestBase >> 2),
               "permission.address");
        const uint64_t uCfg = row.packed | CfgNA4 | CfgU;
        expect(spmp->spmpUpdateCfg(0, uCfg), "permission.config");
        for (PrivilegeMode privilege : {PRV_U, PRV_S}) {
            for (bool sum : {false, true}) {
                STATUS status = tc->readMiscRegNoEffect(MISCREG_STATUS);
                status.sum = sum;
                tc->setMiscRegNoEffect(MISCREG_STATUS, status);
                const BaseMMU::Mode modes[] = {
                    BaseMMU::Read, BaseMMU::Write, BaseMMU::Execute};
                for (BaseMMU::Mode mode : modes) {
                    const uint8_t access = 1 << mode;
                    bool expected = (row.ordinary & access) != 0;
                    if (privilege == PRV_S)
                        expected = sum && mode != BaseMMU::Execute && expected;
                    expect((check(spmp, tc, TestBase, 1, mode, privilege) ==
                            NoFault) == expected,
                           "permission.u-rule-figure4");
                }
            }
        }

        const uint64_t sCfg = row.packed | CfgNA4;
        expect(spmp->spmpUpdateCfg(0, sCfg), "permission.s-only-config");
        for (BaseMMU::Mode mode : {BaseMMU::Read, BaseMMU::Write,
                                   BaseMMU::Execute}) {
            const bool allowed = (row.ordinary & (1 << mode)) != 0;
            expect((check(spmp, tc, TestBase, 1, mode, PRV_S) == NoFault) ==
                   allowed, "permission.s-only-figure4");
            expect(check(spmp, tc, TestBase, 1, mode, PRV_U) != NoFault,
                   "permission.s-only-denies-u");
        }

        const uint64_t sharedCfg = row.packed | CfgNA4 | CfgU | CfgShared;
        expect(spmp->spmpUpdateCfg(0, sharedCfg), "permission.shared.config");
        for (PrivilegeMode privilege : {PRV_U, PRV_S}) {
            for (bool sum : {false, true}) {
                STATUS status = tc->readMiscRegNoEffect(MISCREG_STATUS);
                status.sum = sum;
                tc->setMiscRegNoEffect(MISCREG_STATUS, status);
                for (BaseMMU::Mode mode : {BaseMMU::Read, BaseMMU::Write,
                                           BaseMMU::Execute}) {
                    const uint8_t expectedMask = privilege == PRV_U ?
                        row.sharedUser : row.ordinary;
                    expect((check(spmp, tc, TestBase, 1, mode, privilege) ==
                            NoFault) ==
                           ((expectedMask & (1 << mode)) != 0),
                           "permission.shared-figure4-sum-ignored");
                }
            }
        }
    }

    const uint64_t rwOnly = CfgNA4 | CfgU | CfgShared | CfgR | CfgW;
    expect(spmp->spmpUpdateCfg(0, rwOnly), "permission.shared-rw-config");
    expect(check(spmp, tc, TestBase, 4, BaseMMU::Read, PRV_U) == NoFault &&
           check(spmp, tc, TestBase, 4, BaseMMU::Write, PRV_U) != NoFault,
           "permission.shared-rw-u-read-only");
    auto sharedAmo = request(tc->getCpuPtr(), tc, TestVaddr, TestBase, 4,
        BaseMMU::Write, Request::ATOMIC_RETURN_OP);
    expectFault(spmp->spmpCheck(sharedAmo, BaseMMU::Write, PRV_U, tc),
                ExceptionCode::STORE_PAGE, TestVaddr, tc,
                "permission.shared-rw-amo-denied");
    const uint64_t rwx = rwOnly | CfgX;
    expect(spmp->spmpUpdateCfg(0, rwx), "permission.shared-rwx-config");
    expect(check(spmp, tc, TestBase, 4, BaseMMU::Execute, PRV_U) == NoFault &&
           check(spmp, tc, TestBase, 4, BaseMMU::Read, PRV_U) != NoFault,
           "permission.shared-rwx-u-execute-only");

    expect(spmp->spmpUpdateCfg(0, CfgNA4 | CfgX), "permission.x-only");
    auto amo = request(tc->getCpuPtr(), tc, TestVaddr, TestBase, 4,
                       BaseMMU::Write, Request::ATOMIC_RETURN_OP);
    expectFault(spmp->spmpCheck(amo, BaseMMU::Write, PRV_S, tc),
                ExceptionCode::STORE_PAGE, TestVaddr, tc,
                "permission.amo-needs-read-and-write-store-cause");

    STATUS status = tc->readMiscRegNoEffect(MISCREG_STATUS);
    status.mxr = 1;
    tc->setMiscRegNoEffect(MISCREG_STATUS, status);
    expect(check(spmp, tc, TestBase, 4, BaseMMU::Read, PRV_S) != NoFault,
           "permission.mxr-does-not-change-spmp-x-only");

    expect(spmp->spmpUpdateCfg(0, CfgNA4 | CfgR), "permission.lr-rule");
    auto lr = request(tc->getCpuPtr(), tc, TestVaddr, TestBase, 4,
        BaseMMU::Read, static_cast<Request::Flags>(XlateFlags::LR));
    const MemAccessInfo lrInfo = static_cast<TLB *>(
        tc->getMMUPtr()->dtb)->getMemAccessInfo(
            tc, BaseMMU::Read, lr->getArchFlags());
    expect(lrInfo.lr && check(spmp, tc, TestBase, 4,
           BaseMMU::Read, PRV_S) == NoFault, "permission.lr-is-load");
    auto sc = request(tc->getCpuPtr(), tc, TestVaddr, TestBase, 4,
                      BaseMMU::Write, Request::LLSC);
    expectFault(spmp->spmpCheck(sc, BaseMMU::Write, PRV_S, tc),
                ExceptionCode::STORE_PAGE, TestVaddr, tc,
                "permission.sc-is-store");
}

/** Exercise matching ranges, limits, and lowest-index priority with real state. */
void
rangeTable(SPMP *spmp, ThreadContext *tc, RiscvType type)
{
    clearEntries(spmp);
    expect(spmp->spmpUpdateAddr(0, TestBase >> 2), "range.entry0-tor-address");
    expect(spmp->spmpUpdateCfg(0, CfgTOR | CfgR), "range.entry0-tor");
    expect(check(spmp, tc, 0, 1, BaseMMU::Read, PRV_S) == NoFault,
           "range.tor-entry-zero-address-zero");
    expect(check(spmp, tc, 0, 1, BaseMMU::Write, PRV_S) != NoFault,
           "range.tor-entry-zero-denial");

    if (spmp->getNumEntries() > 1) {
        clearEntries(spmp);
        expect(spmp->spmpUpdateAddr(0, TestBase >> 2), "range.tor-low");
        expect(spmp->spmpUpdateAddr(1, (TestBase + 0x100) >> 2),
               "range.tor-high");
        expect(spmp->spmpUpdateCfg(1, CfgTOR | CfgR), "range.tor-config");
        expect(check(spmp, tc, TestBase, 1, BaseMMU::Read, PRV_S) == NoFault,
               "range.tor-first-byte");
        expect(check(spmp, tc, TestBase + 0xff, 1, BaseMMU::Read, PRV_S) ==
               NoFault, "range.tor-last-byte");
        expect(check(spmp, tc, TestBase + 0x100, 1, BaseMMU::Read, PRV_S) !=
               NoFault, "range.tor-end-exclusive");
        expect(spmp->spmpUpdateAddr(1, TestBase >> 2),
               "range.equal-tor-high");
        expect(check(spmp, tc, TestBase, 1, BaseMMU::Read, PRV_S) != NoFault,
               "range.empty-equal-tor");
        expect(spmp->spmpUpdateAddr(0, (TestBase + 0x100) >> 2),
               "range.reversed-tor-low");
        expect(spmp->spmpUpdateAddr(1, TestBase >> 2),
               "range.reversed-tor-high");
        expect(check(spmp, tc, TestBase + 0x80, 1, BaseMMU::Read, PRV_S) !=
               NoFault, "range.empty-reversed-tor");

        clearEntries(spmp);
        expect(spmp->spmpUpdateAddr(0, TestBase >> 2), "range.priority-low");
        expect(spmp->spmpUpdateCfg(0, CfgNAPOT), "range.priority-deny-first");
        napot(spmp, 1, TestBase, 16, CfgR | CfgW | CfgX);
        expect(check(spmp, tc, TestBase, 1, BaseMMU::Read, PRV_S) != NoFault,
               "range.first-overlap-wins");

        clearEntries(spmp);
        expect(spmp->spmpUpdateAddr(0, TestBase >> 2),
               "range.adjacent-first-address");
        expect(spmp->spmpUpdateCfg(0, CfgNA4 | CfgR),
               "range.adjacent-first-config");
        expect(spmp->spmpUpdateAddr(1, (TestBase + 4) >> 2),
               "range.adjacent-second-address");
        expect(spmp->spmpUpdateCfg(1, CfgNA4 | CfgR),
               "range.adjacent-second-config");
        expect(check(spmp, tc, TestBase, 8, BaseMMU::Read, PRV_S) != NoFault,
               "range-adjacent-entries-not-combined");
        expect(check(spmp, tc, TestBase - 1, 2, BaseMMU::Read, PRV_S) !=
               NoFault, "range-partial-overlap-denied");
        expect(check(spmp, tc, TestBase, 4, BaseMMU::Read, PRV_S) == NoFault,
               "range-na4-boundary");

        clearEntries(spmp);
        expect(spmp->spmpUpdateAddr(0, TestBase >> 2), "range.lock-predecessor");
        expect(spmp->spmpUpdateCfg(1, CfgTOR | CfgL | CfgR),
               "range.lock-tor-successor");
        expect(!spmp->spmpUpdateAddr(0, (TestBase + 4) >> 2),
               "range.locked-tor-predecessor");
    }

    clearEntries(spmp);
    const Addr addrMask = type == RV32 ? mask(32) : mask(54);
    const Addr physicalLimit = (addrMask + 1) << 2;
    expect(spmp->spmpUpdateAddr(0, addrMask), "range.max-napot-address");
    expect(spmp->spmpUpdateCfg(0, CfgNAPOT | CfgR | CfgW | CfgX),
           "range.max-napot-config");
    expect(check(spmp, tc, physicalLimit - 1, 1, BaseMMU::Read, PRV_S) ==
           NoFault, "range.max-napot-physical-last-byte");
    expect(check(spmp, tc, physicalLimit, 1, BaseMMU::Read, PRV_S) != NoFault,
           "range-physical-limit-exclusive");
    expect(check(spmp, tc, physicalLimit - 2, 8, BaseMMU::Read, PRV_S) !=
           NoFault, "range-cross-physical-limit");
    expect(check(spmp, tc, MaxAddr - 3, 8, BaseMMU::Read, PRV_S) != NoFault,
           "range-size-address-overflow");
    expect(check(spmp, tc, 0, 0, BaseMMU::Read, PRV_S) != NoFault,
           "range-zero-size-rejected");

    clearEntries(spmp);
    expect(check(spmp, tc, TestBase, 1, BaseMMU::Read, PRV_S) != NoFault,
           "range-no-match-denies");
    napot(spmp, 0, TestBase, 16, CfgR);
    expect(check(spmp, tc, TestBase, 16, BaseMMU::Read, PRV_S) == NoFault &&
           check(spmp, tc, TestBase + 15, 1, BaseMMU::Read, PRV_S) == NoFault,
           "range.napot-whole-range-and-last-byte");
    expect(check(spmp, tc, TestBase + 16, 1, BaseMMU::Read, PRV_S) != NoFault &&
           check(spmp, tc, TestBase + 15, 2, BaseMMU::Read, PRV_S) != NoFault,
           "range.napot-exclusive-end-and-crossing");
    clearEntries(spmp);
}

/** Confirm feature-off and zero-entry profiles leave Bare access unchecked. */
void
inactiveAccess(BaseCPU *cpu, ThreadContext *tc, MMU *mmu, ISA *isa, SPMP *spmp)
{
    if (isa->enableSspmp() && spmp->getNumEntries() != 0)
        return;
    mmu->getPMP()->pmpReset();
    expect(mmu->getPMP()->pmpUpdateAddr(0, MaxAddr),
           "activation.inactive-pmp-address");
    expect(mmu->getPMP()->pmpUpdateCfg(0, 0x0f),
           "activation.inactive-pmp-config");
    tc->setMiscRegNoEffect(MISCREG_SATP, 0);
    tc->setMiscRegNoEffect(MISCREG_PRV, PRV_S);
    auto req = request(cpu, tc, TestVaddr, MaxAddr, 4, BaseMMU::Read);
    expect(mmu->translateAtomic(req, tc, BaseMMU::Read) == NoFault,
           "activation.disabled-or-zero-preserves-bare-access");
}

/** Verify translation-site ordering, functional coverage, and timing completion. */
void
translationPaths(BaseCPU *cpu, ThreadContext *tc, MMU *mmu, SPMP *spmp)
{
    clearEntries(spmp);
    const Addr base = TestVaddr;
    mmu->getPMP()->pmpReset();
    expect(mmu->getPMP()->pmpUpdateAddr(0, MaxAddr),
           "translate.pmp-permissive-address");
    expect(mmu->getPMP()->pmpUpdateCfg(0, 0x0f),
           "translate.pmp-permissive-config");
    napot(spmp, 0, base, 16, CfgR | CfgW | CfgX);
    auto isa = static_cast<ISA *>(tc->getIsaPtr());
    tc->setMiscRegNoEffect(MISCREG_PRV, PRV_S);
    tc->setMiscRegNoEffect(MISCREG_SATP, 0);

    auto load = request(cpu, tc, base, MaxAddr, 4, BaseMMU::Read);
    Fault fault = mmu->translateAtomic(load, tc, BaseMMU::Read);
    expect(fault == NoFault && load->getPaddr() == base,
           "translate.bare-positive-load");

    auto denied = request(cpu, tc, TestVaddr + 0x100, MaxAddr, 4,
                          BaseMMU::Read);
    fault = mmu->translateAtomic(denied, tc, BaseMMU::Read);
    expectFault(fault, ExceptionCode::LOAD_PAGE, TestVaddr + 0x100, tc,
                "translate.bare-negative-load");

    auto store = request(cpu, tc, TestVaddr + 0x100, MaxAddr, 4,
                         BaseMMU::Write);
    fault = mmu->translateAtomic(store, tc, BaseMMU::Write);
    expectFault(fault, ExceptionCode::STORE_PAGE, TestVaddr + 0x100, tc,
                "translate.bare-negative-store");

    auto physical = request(cpu, tc, TestVaddr + 0x100, MaxAddr, 4,
        BaseMMU::Read, static_cast<Request::Flags>(Request::PHYSICAL));
    fault = mmu->translateAtomic(physical, tc, BaseMMU::Read);
    expectFault(fault, ExceptionCode::LOAD_PAGE, TestVaddr + 0x100, tc,
                "translate.physical-flag-cannot-bypass");

    auto functional = request(cpu, tc, TestVaddr + 0x100, MaxAddr, 4,
                              BaseMMU::Read);
    fault = static_cast<BaseMMU *>(mmu)->translateFunctional(
        functional, tc, BaseMMU::Read);
    expectFault(fault, ExceptionCode::LOAD_PAGE, TestVaddr + 0x100, tc,
                "translate.arch-functional-denial");

    auto proxy = request(cpu, tc, TestVaddr + 0x100, MaxAddr, 4,
        BaseMMU::Read, 0, Request::funcRequestorId);
    expect(static_cast<BaseMMU *>(mmu)->translateFunctional(
               proxy, tc, BaseMMU::Read) == NoFault,
           "translate.simulator-functional-proxy-bypass");

    auto physicalOnly = std::make_shared<Request>(
        TestVaddr + 0x100, 4, Request::UNCACHEABLE, cpu->dataRequestorId());
    physicalOnly->setContext(tc->contextId());
    expectFault(mmu->finalizePhysical(physicalOnly, tc, BaseMMU::Read),
                ExceptionCode::LOAD_PAGE, TestVaddr + 0x100, tc,
                "translate.finalize-physical");

    struct Completion : BaseMMU::Translation {
        int finished = 0;
        bool delayed = false;
        Fault result = NoFault;
        /** Record whether translation deferred its completion. */
        void markDelayed() override { delayed = true; }
        /** Record every completion without issuing any memory packet. */
        void finish(const Fault &fault, const RequestPtr &,
                    ThreadContext *, BaseMMU::Mode) override
        {
            ++finished;
            result = fault;
        }
    } completion;
    auto timing = request(cpu, tc, TestVaddr + 0x100, MaxAddr, 4,
                          BaseMMU::Read);
    mmu->translateTiming(timing, tc, &completion, BaseMMU::Read);
    expect(!completion.delayed && completion.finished == 1,
           "translate.bare-timing-one-completion");
    expectFault(completion.result, ExceptionCode::LOAD_PAGE,
                TestVaddr + 0x100, tc, "translate.timing-page-fault");

    if (isa->enableSspmp()) {
        clearEntries(spmp);
        expect(check(spmp, tc, TestBase, 4, BaseMMU::Read, PRV_M) == NoFault,
               "translate.effective-m-bypass");
    }
}

/** Verify PMP/PMA priority after a successful SPMP check. */
void
faultPriority(BaseCPU *cpu, ThreadContext *tc, MMU *mmu, SPMP *spmp)
{
    if (spmp->getNumEntries() == 0)
        return;
    clearEntries(spmp);
    mmu->getPMP()->pmpReset();
    tc->setMiscRegNoEffect(MISCREG_PRV, PRV_S);
    tc->setMiscRegNoEffect(MISCREG_STATUS, 0);
    tc->setMiscRegNoEffect(MISCREG_SATP, 0);
    auto denied = request(cpu, tc, TestVaddr, MaxAddr, 4, BaseMMU::Read);
    expectFault(mmu->translateAtomic(denied, tc, BaseMMU::Read),
                ExceptionCode::LOAD_PAGE, TestVaddr, tc,
                "priority.spmp-load-before-pmp");
    auto deniedStore = request(cpu, tc, TestVaddr, MaxAddr, 4,
                               BaseMMU::Write);
    expectFault(mmu->translateAtomic(deniedStore, tc, BaseMMU::Write),
                ExceptionCode::STORE_PAGE, TestVaddr, tc,
                "priority.spmp-store-before-pmp");
    auto deniedFetch = request(cpu, tc, TestVaddr, MaxAddr, 2,
                               BaseMMU::Execute);
    expectFault(mmu->translateAtomic(deniedFetch, tc, BaseMMU::Execute),
                ExceptionCode::INST_PAGE, TestVaddr, tc,
                "priority.spmp-fetch-before-pmp");
    auto deniedAmo = request(cpu, tc, TestVaddr, MaxAddr, 4,
        BaseMMU::Write, Request::ATOMIC_RETURN_OP);
    expectFault(mmu->translateAtomic(deniedAmo, tc, BaseMMU::Write),
                ExceptionCode::STORE_PAGE, TestVaddr, tc,
                "priority.spmp-amo-before-pmp");

    if (static_cast<ISA *>(tc->getIsaPtr())->rvType() == RV64) {
        const Addr high = Addr{1} << 63;
        auto highReq = request(cpu, tc, high, MaxAddr, 4, BaseMMU::Read);
        expectFault(mmu->translateAtomic(highReq, tc, BaseMMU::Read),
                    ExceptionCode::LOAD_PAGE, high, tc,
                    "priority.spmp-before-high-physical-bit");
    }

    napot(spmp, 0, TestVaddr, 16, CfgR | CfgW | CfgX);
    mmu->getPMP()->pmpReset();
    auto pmpDenied = request(cpu, tc, TestVaddr, MaxAddr, 4, BaseMMU::Read);
    expectFault(mmu->translateAtomic(pmpDenied, tc, BaseMMU::Read),
                ExceptionCode::LOAD_ACCESS, TestVaddr, tc,
                "priority.pmp-retained-after-spmp-allow");

    mmu->getPMP()->pmpReset();
    expect(mmu->getPMP()->pmpUpdateAddr(0, MaxAddr), "priority.pmp-address");
    expect(mmu->getPMP()->pmpUpdateCfg(0, 0x0f), "priority.pmp-config");
    clearEntries(spmp);
    const Addr uncacheable = 0x80004000;
    auto misaligned = request(cpu, tc, uncacheable + 1, MaxAddr, 4,
        BaseMMU::Read, static_cast<Request::Flags>(MMU::WordAlign));
    expectFault(mmu->translateAtomic(misaligned, tc, BaseMMU::Read),
                ExceptionCode::LOAD_PAGE, uncacheable + 1, tc,
                "priority.spmp-over-deferred-pma-misalignment");

    napot(spmp, 0, uncacheable, 0x1000, CfgR | CfgW | CfgX);
    auto pmaMisaligned = request(cpu, tc, uncacheable + 1, MaxAddr, 4,
        BaseMMU::Read, static_cast<Request::Flags>(MMU::WordAlign));
    expectFault(mmu->translateAtomic(pmaMisaligned, tc, BaseMMU::Read),
                ExceptionCode::LOAD_ADDR_MISALIGNED, uncacheable + 1, tc,
                "priority.pma-misalignment-retained");

    auto aligned = request(cpu, tc, uncacheable, MaxAddr, 4, BaseMMU::Read);
    expect(mmu->translateAtomic(aligned, tc, BaseMMU::Read) == NoFault &&
           aligned->isUncacheable() && aligned->isStrictlyOrdered(),
           "priority.pma-flags-retained");
}

/** Check alias selector behavior when feature availability varies. */
void
csrLegality(ISA *isa, SPMP *spmp, ThreadContext *tc)
{
    for (RegVal selector : {RegVal{0}, RegVal{0xfff}}) {
        tc->setMiscRegNoEffect(MISCREG_SISELECT, selector);
        expect(isa->indirectCSRCheck(CSR_SIREG, 0) != NoFault,
               "csr.unsupported-selector-illegal");
    }
    tc->setMiscRegNoEffect(MISCREG_SISELECT, 0x100);
    for (uint64_t alias : {CSR_SIREG3, CSR_SIREG4, CSR_SIREG5, CSR_SIREG6})
        expect(isa->indirectCSRCheck(alias, 0) != NoFault,
               "csr.reserved-alias-illegal");
    expect((isa->indirectCSRCheck(CSR_SIREG, 0) == NoFault) ==
           isa->enableSspmp(), "csr.disabled-versus-count-zero-legality");
    if (!isa->enableSspmp()) {
        expect(isa->readMiscReg(MISCREG_SIREG) == 0,
               "csr.disabled-alias-read-zero");
        isa->setMiscReg(MISCREG_SIREG, 0x1234);
    } else if (spmp->getNumEntries() == 0) {
        expect(isa->readMiscReg(MISCREG_SIREG) == 0,
               "csr.zero-count-alias-read-zero");
        isa->setMiscReg(MISCREG_SIREG, 0x1234);
    }

    for (RegVal selector : {RegVal{0}, RegVal{0xfff}, RegVal{0x100},
                            RegVal{0x13f}}) {
        isa->setMiscReg(MISCREG_SISELECT, selector);
        expect(isa->readMiscRegNoEffect(MISCREG_SISELECT) == selector,
               "csr.selector-warl-12-bits");
    }

    if (spmp->getNumEntries() != 0 && isa->enableSspmp()) {
        tc->setMiscRegNoEffect(MISCREG_SISELECT, 0x100);
        expect(spmp->spmpUpdateAddr(0, TestBase >> 2), "csr.canonical-address");
        expect(spmp->spmpUpdateCfg(0, CfgNA4 | CfgR | CfgU | CfgShared),
               "csr.canonical-cfg");
        expect(isa->readMiscReg(MISCREG_SIREG) == (TestBase >> 2),
               "csr.indirect-address-readback");
        expect(isa->readMiscReg(MISCREG_SIREG2) ==
               (CfgNA4 | CfgR | CfgU | CfgShared),
               "csr.indirect-full-config-readback");
        expect(spmp->spmpUpdateCfg(0, CfgNA4 | CfgW), "csr.reserved-rwx-write");
        expect(spmp->getCfg(0) == CfgNA4, "csr.reserved-rwx-canonicalization");
        expect(spmp->spmpUpdateCfg(0, CfgNA4 | CfgR | CfgShared),
               "csr.reserved-shared-write");
        expect(spmp->getCfg(0) == (CfgNA4 | CfgR),
               "csr.reserved-shared-canonicalization");
        expect(spmp->spmpUpdateAddr(0, MaxAddr), "csr.address-mask-write");
        const Addr addressMask = isa->rvType() == RV32 ? mask(32) : mask(54);
        expect(isa->readMiscReg(MISCREG_SIREG) == addressMask,
               "csr.canonical-xlen-address-mask-readback");
        expect(spmp->spmpUpdateCfg(0, CfgL | CfgR), "csr.lock-off-entry");
        expect(!spmp->spmpUpdateAddr(0, 0) &&
               !spmp->spmpUpdateCfg(0, 0) && spmp->getAddr(0) == addressMask,
               "csr.off-entry-address-and-config-lock");
        spmp->spmpReset();
    }
}

} // anonymous namespace

/** Bind the test CPU without modifying its architectural state. */
SPMPTest::SPMPTest(const RiscvSPMPTestParams &p)
    : SimObject(p), cpu(p.cpu)
{}

/** Run direct backend and translation checks and fail on the first error. */
void
SPMPTest::run()
{
    fatal_if(!FullSystem, "RiscvSPMPTest requires full-system mode");
    ThreadContext *tc = cpu->getContext(0);
    auto isa = static_cast<ISA *>(tc->getIsaPtr());
    auto mmu = static_cast<MMU *>(tc->getMMUPtr());
    SPMP *spmp = mmu->getSPMP();
    expect(spmp->getNumEntries() <= 64, "activation.backend-count-bound");
    expect(spmp->params().riscv_type == isa->rvType(),
           "activation.backend-hart-xlen");
    csrLegality(isa, spmp, tc);
    if (isa->enableSspmp()) {
        const RegVal original = tc->readMiscRegNoEffect(MISCREG_ISA);
        MISA attempted = original;
        attempted.rvh = 1;
        isa->setMiscReg(MISCREG_ISA, attempted);
        const MISA canonical = tc->readMiscRegNoEffect(MISCREG_ISA);
        expect(!canonical.rvh && canonical.rvs && canonical.rvu,
               "activation.misa-cannot-enable-h");
        tc->setMiscRegNoEffect(MISCREG_ISA, original);
    }
    if (isa->enableSspmp() && isa->rvType() == RV32) {
        tc->setMiscRegNoEffect(MISCREG_PRV, PRV_M);
        tc->setMiscReg(MISCREG_SATP, RegVal{1} << 31);
        expect(tc->readMiscRegNoEffect(MISCREG_SATP) == 0,
               "activation.rv32-sv32-write-retained-bare");
        if (spmp->getNumEntries() != 0) {
            clearEntries(spmp);
            tc->setMiscRegNoEffect(MISCREG_PRV, PRV_S);
            auto req = request(cpu, tc, TestVaddr, MaxAddr, 4, BaseMMU::Read);
            expectFault(mmu->translateAtomic(req, tc, BaseMMU::Read),
                        ExceptionCode::LOAD_PAGE, TestVaddr, tc,
                        "activation.rv32-sv32-attempt-still-bare-denial");
            tc->setMiscRegNoEffect(MISCREG_PRV, PRV_M);
        }
    }
    inactiveAccess(cpu, tc, mmu, isa, spmp);

    STATUS status = tc->readMiscRegNoEffect(MISCREG_STATUS);
    status.sum = 0;
    status.mxr = 0;
    tc->setMiscRegNoEffect(MISCREG_STATUS, status);
    tc->setMiscRegNoEffect(MISCREG_SATP, 0);
    tc->setMiscRegNoEffect(MISCREG_PRV, PRV_M);
    if (spmp->getNumEntries() && isa->enableSspmp()) {
        rangeTable(spmp, tc, isa->rvType());
        permissionTable(spmp, tc);
        translationPaths(cpu, tc, mmu, spmp);
        faultPriority(cpu, tc, mmu, spmp);
    }

    // MPRV data accesses use MPP; instruction fetches keep current M privilege.
    if (spmp->getNumEntries() && isa->enableSspmp()) {
        clearEntries(spmp);
        napot(spmp, 0, TestVaddr, 16, CfgR | CfgU);
        STATUS mstatus = tc->readMiscRegNoEffect(MISCREG_STATUS);
        mstatus.mprv = 1;
        mstatus.sum = 0;
        mstatus.mxr = 0;
        mstatus.mpp = PRV_S;
        tc->setMiscRegNoEffect(MISCREG_STATUS, mstatus);
        tc->setMiscRegNoEffect(MISCREG_PRV, PRV_M);
        tc->setMiscRegNoEffect(MISCREG_NMIE, 1);
        auto data = request(cpu, tc, TestVaddr, MaxAddr, 4, BaseMMU::Read);
        expectFault(mmu->translateAtomic(data, tc, BaseMMU::Read),
                    ExceptionCode::LOAD_PAGE, TestVaddr, tc,
                    "effective-privilege.mprv-mpp-s");
        mstatus.mpp = PRV_U;
        tc->setMiscRegNoEffect(MISCREG_STATUS, mstatus);
        auto user = request(cpu, tc, TestVaddr, MaxAddr, 4, BaseMMU::Read);
        expect(mmu->translateAtomic(user, tc, BaseMMU::Read) == NoFault,
               "effective-privilege.mprv-mpp-u");
        mstatus.mpp = PRV_M;
        tc->setMiscRegNoEffect(MISCREG_STATUS, mstatus);
        auto machine = request(cpu, tc, TestVaddr, MaxAddr, 4, BaseMMU::Read);
        expect(mmu->translateAtomic(machine, tc, BaseMMU::Read) == NoFault,
               "effective-privilege.mprv-mpp-m");
        auto fetch = request(cpu, tc, TestVaddr + 0x100, MaxAddr, 2,
                             BaseMMU::Execute);
        expect(mmu->translateAtomic(fetch, tc, BaseMMU::Execute) == NoFault,
               "effective-privilege.fetch-ignores-mprv");

        if (isa->enableSmrnmi()) {
            mstatus.mpp = PRV_S;
            tc->setMiscRegNoEffect(MISCREG_STATUS, mstatus);
            tc->setMiscRegNoEffect(MISCREG_NMIE, 0);
            auto nmieOff = request(cpu, tc, TestVaddr, MaxAddr, 4,
                                   BaseMMU::Read);
            expect(mmu->translateAtomic(nmieOff, tc, BaseMMU::Read) ==
                   NoFault, "effective-privilege.rnmi-nmie-zero-disables-mprv");
            tc->setMiscRegNoEffect(MISCREG_NMIE, 1);
            auto nmieOn = request(cpu, tc, TestVaddr, MaxAddr, 4,
                                  BaseMMU::Read);
            expectFault(mmu->translateAtomic(nmieOn, tc, BaseMMU::Read),
                        ExceptionCode::LOAD_PAGE, TestVaddr, tc,
                        "effective-privilege.rnmi-nmie-one-enables-mprv");
        } else {
            mstatus.mpp = PRV_S;
            tc->setMiscRegNoEffect(MISCREG_STATUS, mstatus);
            tc->setMiscRegNoEffect(MISCREG_NMIE, 0);
            auto nmieIrrelevant = request(cpu, tc, TestVaddr, MaxAddr, 4,
                                          BaseMMU::Read);
            expectFault(mmu->translateAtomic(nmieIrrelevant, tc, BaseMMU::Read),
                        ExceptionCode::LOAD_PAGE, TestVaddr, tc,
                        "effective-privilege.rnmi-disabled-mprv");
        }
        tc->setMiscRegNoEffect(MISCREG_NMIE, 0);
        tc->setMiscRegNoEffect(MISCREG_STATUS, 0);
        tc->setMiscRegNoEffect(MISCREG_PRV, PRV_M);
    }

    // SFENCE.VMA ordering itself is exercised by the guest workload; this
    // direct test confirms every translation reads the newly programmed entry.
    if (spmp->getNumEntries() && isa->enableSspmp()) {
        clearEntries(spmp);
        tc->setMiscRegNoEffect(MISCREG_PRV, PRV_S);
        tc->setMiscRegNoEffect(MISCREG_STATUS, 0);
        napot(spmp, 0, TestVaddr, 16, CfgR | CfgW | CfgX);
        auto before = request(cpu, tc, TestVaddr, MaxAddr, 4, BaseMMU::Read);
        expect(mmu->translateAtomic(before, tc, BaseMMU::Read) == NoFault,
               "reconfiguration.allow");
        expect(spmp->spmpUpdateCfg(0, CfgNAPOT | CfgR),
               "reconfiguration.update");
        auto after = request(cpu, tc, TestVaddr, MaxAddr, 4, BaseMMU::Write);
        expectFault(mmu->translateAtomic(after, tc, BaseMMU::Write),
                    ExceptionCode::STORE_PAGE, TestVaddr, tc,
                    "reconfiguration.deny-after-write");
        expect(spmp->spmpUpdateCfg(0, CfgNAPOT | CfgR | CfgW | CfgX),
               "reconfiguration.restore");
        auto restored = request(cpu, tc, TestVaddr, MaxAddr, 4,
                                BaseMMU::Write);
        expect(mmu->translateAtomic(restored, tc, BaseMMU::Write) == NoFault,
               "reconfiguration.allow-after-write");
    }

    // MMU reset preserves addresses and defined policy bits but clears A/L.
    if (spmp->getNumEntries() && isa->enableSspmp()) {
        expect(spmp->spmpUpdateAddr(0, TestBase >> 2), "reset.address");
        expect(spmp->spmpUpdateCfg(0, CfgNA4 | CfgL | CfgR | CfgU),
               "reset.config");
        tc->setMiscRegNoEffect(MISCREG_SISELECT, 0x100);
        mmu->reset();
        expect(spmp->getAddr(0) == (TestBase >> 2), "reset.address-retained");
        expect(spmp->getCfg(0) == (CfgR | CfgU), "reset-clears-a-and-l");
        expect(check(spmp, tc, TestBase, 4, BaseMMU::Read, PRV_S) != NoFault,
               "reset.enabled-empty-table-denies");
        auto denied = request(cpu, tc, TestBase, MaxAddr, 4, BaseMMU::Read);
        expectFault(mmu->translateAtomic(denied, tc, BaseMMU::Read),
                    ExceptionCode::LOAD_PAGE, TestBase, tc,
                    "reset.empty-table-translation-denies");
        expect(spmp->spmpUpdateAddr(0, MaxAddr) &&
               spmp->spmpUpdateCfg(0, CfgNAPOT | CfgR | CfgW | CfgX),
               "reset.program-baseline");
        expect(mmu->getPMP()->pmpUpdateAddr(0, MaxAddr) &&
               mmu->getPMP()->pmpUpdateCfg(0, 0x0f),
               "reset.program-pmp-baseline");
        auto allowed = request(cpu, tc, TestBase, MaxAddr, 4, BaseMMU::Read);
        expect(mmu->translateAtomic(allowed, tc, BaseMMU::Read) == NoFault,
               "reset.baseline-restores-access");
        isa->resetThread();
        expect(isa->readMiscRegNoEffect(MISCREG_SISELECT) == 0,
               "reset.selector");
    }

    inform("RiscvSPMPTest: direct fixture cases passed");
}

/** Seed checkpoint state without changing selector during later restore checks. */
void
SPMPTest::seedCheckpoint()
{
    ThreadContext *tc = cpu->getContext(0);
    auto isa = static_cast<ISA *>(tc->getIsaPtr());
    auto mmu = static_cast<MMU *>(tc->getMMUPtr());
    SPMP *spmp = mmu->getSPMP();
    fatal_if(!isa->enableSspmp() || spmp->getNumEntries() < 3,
             "Checkpoint SPMP fixture requires enabled SPMP and 3 entries");
    clearEntries(spmp);
    expect(spmp->spmpUpdateAddr(0, TestBase >> 2), "checkpoint.entry0-address");
    expect(spmp->spmpUpdateCfg(0, CfgNA4 | CfgL | CfgR | CfgW | CfgX),
           "checkpoint.entry0-locked");
    expect(spmp->spmpUpdateAddr(1, (TestBase + 0x100) >> 2),
           "checkpoint.entry1-address");
    expect(spmp->spmpUpdateCfg(1, CfgTOR | CfgL | CfgR | CfgW | CfgX),
           "checkpoint.entry1-locked-tor");
    napot(spmp, 2, TestBase + 0x200, 16,
          CfgR | CfgW | CfgX | CfgU | CfgShared);
    mmu->getPMP()->pmpReset();
    expect(mmu->getPMP()->pmpUpdateAddr(0, MaxAddr),
           "checkpoint.pmp-address");
    expect(mmu->getPMP()->pmpUpdateCfg(0, 0x0f),
           "checkpoint.pmp-config");
    tc->setMiscRegNoEffect(MISCREG_SISELECT, 0x102);
    tc->setMiscRegNoEffect(MISCREG_PRV, PRV_M);
    tc->setMiscRegNoEffect(MISCREG_SATP, 0);
    inform("RiscvSPMPTest: checkpoint state seeded");
}

/** Verify restored selector, raw entries, locks, TOR, and effective permissions. */
void
SPMPTest::verifyCheckpoint()
{
    ThreadContext *tc = cpu->getContext(0);
    auto isa = static_cast<ISA *>(tc->getIsaPtr());
    auto mmu = static_cast<MMU *>(tc->getMMUPtr());
    SPMP *spmp = mmu->getSPMP();
    fatal_if(!isa->enableSspmp() || spmp->getNumEntries() < 3,
             "Checkpoint SPMP fixture requires enabled SPMP and 3 entries");
    expect(isa->readMiscRegNoEffect(MISCREG_SISELECT) == 0x102,
           "checkpoint.selector-restored");
    expect(spmp->getAddr(0) == (TestBase >> 2) &&
           spmp->getCfg(0) == (CfgNA4 | CfgL | CfgR | CfgW | CfgX),
           "checkpoint.locked-entry-restored");
    expect(spmp->getAddr(1) == ((TestBase + 0x100) >> 2) &&
           spmp->getCfg(1) == (CfgTOR | CfgL | CfgR | CfgW | CfgX),
           "checkpoint.tor-entry-restored");
    expect(spmp->getCfg(2) ==
           (CfgNAPOT | CfgR | CfgW | CfgX | CfgU | CfgShared),
           "checkpoint.shared-entry-restored");
    tc->setMiscRegNoEffect(MISCREG_PRV, PRV_S);
    tc->setMiscRegNoEffect(MISCREG_SATP, 0);
    expect(check(spmp, tc, TestBase, 1, BaseMMU::Read, PRV_S) == NoFault,
           "checkpoint.restored-allow");
    expect(check(spmp, tc, TestBase + 0x1000, 1, BaseMMU::Read, PRV_S) !=
           NoFault, "checkpoint.restored-default-deny");
    expect(check(spmp, tc, TestBase + 4, 1, BaseMMU::Read, PRV_S) == NoFault &&
           check(spmp, tc, TestBase + 0xff, 1, BaseMMU::Read, PRV_S) == NoFault,
           "checkpoint.restored-tor-allow-boundaries");
    expect(check(spmp, tc, TestBase + 0x100, 1, BaseMMU::Read, PRV_S) != NoFault,
           "checkpoint.restored-tor-end-denied");
    auto translated = request(cpu, tc, TestBase + 8, MaxAddr, 4, BaseMMU::Read);
    expect(mmu->translateAtomic(translated, tc, BaseMMU::Read) == NoFault,
           "checkpoint.restored-translation-allow");
    auto denied = request(cpu, tc, TestBase + 0x1000, MaxAddr, 4, BaseMMU::Read);
    expectFault(mmu->translateAtomic(denied, tc, BaseMMU::Read),
                ExceptionCode::LOAD_PAGE, TestBase + 0x1000, tc,
                "checkpoint.restored-translation-deny");
    expect(!spmp->spmpUpdateCfg(0, 0), "checkpoint.lock-retained");
    expect(!spmp->spmpUpdateAddr(0, 0) && !spmp->spmpUpdateCfg(1, 0),
           "checkpoint.tor-predecessor-and-config-locks-retained");
    inform("RiscvSPMPTest: checkpoint state verified");
}

/** Request a fatal diagnostic when contexts sharing an MMU disagree. */
void
SPMPTest::rejectSharedProfile()
{
    ThreadContext *tc = cpu->getContext(0);
    auto isa = static_cast<ISA *>(tc->getIsaPtr());
    auto mmu = static_cast<MMU *>(tc->getMMUPtr());
    mmu->bindSPMPProfile(!isa->enableSspmp(), isa->rvType(),
                         isa->getPrivilegeModeSet());
}

} // namespace RiscvISA
} // namespace gem5
