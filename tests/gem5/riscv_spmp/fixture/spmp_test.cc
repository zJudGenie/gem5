#include "spmp_test.hh"

#include <memory>

#include "arch/riscv/faults.hh"
#include "arch/riscv/mmu.hh"
#include "arch/riscv/regs/misc.hh"
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

constexpr Addr TestVaddr = 0x40001000;
constexpr uint64_t CfgR = 1;
constexpr uint64_t CfgW = 2;
constexpr uint64_t CfgX = 4;
constexpr uint64_t CfgNAPOT = 3 << 3;
constexpr uint64_t CfgU = 1 << 8;

void
expect(bool condition, const char *caseId)
{
    fatal_if(!condition, "RiscvSPMPTest failed case %s", caseId);
}

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

void
clearEntries(SPMP *spmp)
{
    spmp->spmpReset();
    for (uint32_t i = 0; i < spmp->getNumEntries(); ++i)
        spmp->spmpUpdateCfg(i, 0);
}

void
napot(SPMP *spmp, uint32_t index, Addr base, Addr size, uint64_t permissions)
{
    const Addr raw = (base >> 2) | ((size >> 3) - 1);
    expect(spmp->spmpUpdateAddr(index, raw), "range.napot.address");
    expect(spmp->spmpUpdateCfg(index, permissions | CfgNAPOT),
           "range.napot.config");
}

} // anonymous namespace

SPMPTest::SPMPTest(const RiscvSPMPTestParams &p)
    : SimObject(p), cpu(p.cpu)
{}

void
SPMPTest::run()
{
    fatal_if(!FullSystem, "RiscvSPMPTest requires full-system mode");
    ThreadContext *tc = cpu->getContext(0);
    auto isa = static_cast<ISA *>(tc->getIsaPtr());
    auto mmu = static_cast<MMU *>(tc->getMMUPtr());
    SPMP *spmp = mmu->getSPMP();

    // 1. Synthetic invalid Request bounds in SPMP::spmpCheck()
    auto reqZero = request(cpu, tc, TestVaddr, 0, 0, BaseMMU::Read);
    expect(spmp->spmpCheck(reqZero, BaseMMU::Read, PRV_S, tc, TestVaddr) != NoFault,
           "range-zero-size-rejected");
    auto reqOverflow = request(cpu, tc, TestVaddr, MaxAddr - 3, 8, BaseMMU::Read);
    expect(spmp->spmpCheck(reqOverflow, BaseMMU::Read, PRV_S, tc, TestVaddr) != NoFault,
           "range-size-address-overflow");

    // 2. Functional debugger bypass vs normal translation enforcement
    clearEntries(spmp);
    mmu->getPMP()->pmpReset();
    expect(mmu->getPMP()->pmpUpdateAddr(0, MaxAddr), "translate.pmp-permissive-addr");
    expect(mmu->getPMP()->pmpUpdateCfg(0, 0x0f), "translate.pmp-permissive-cfg");
    tc->setMiscRegNoEffect(MISCREG_PRV, PRV_S);
    tc->setMiscRegNoEffect(MISCREG_STATUS, 0);
    tc->setMiscRegNoEffect(MISCREG_SATP, 0);

    auto functional = request(cpu, tc, TestVaddr, MaxAddr, 4, BaseMMU::Read);
    Fault fault = static_cast<BaseMMU *>(mmu)->translateFunctional(
        functional, tc, BaseMMU::Read);
    expectFault(fault, ExceptionCode::LOAD_PAGE, TestVaddr, tc,
                "translate.arch-functional-denial");

    auto proxy = request(cpu, tc, TestVaddr, MaxAddr, 4, BaseMMU::Read, 0,
                         Request::funcRequestorId);
    expect(static_cast<BaseMMU *>(mmu)->translateFunctional(
               proxy, tc, BaseMMU::Read) == NoFault,
           "translate.simulator-functional-proxy-bypass");

    auto physicalOnly = std::make_shared<Request>(
        TestVaddr, 4, Request::UNCACHEABLE, cpu->dataRequestorId());
    physicalOnly->setContext(tc->contextId());
    expectFault(mmu->finalizePhysical(physicalOnly, tc, BaseMMU::Read),
                ExceptionCode::LOAD_PAGE, TestVaddr, tc,
                "translate.finalize-physical");

    struct Completion : BaseMMU::Translation {
        int finished = 0;
        bool delayed = false;
        Fault result = NoFault;
        void markDelayed() override { delayed = true; }
        void finish(const Fault &fault, const RequestPtr &,
                    ThreadContext *, BaseMMU::Mode) override
        {
            ++finished;
            result = fault;
        }
    } completion;
    auto timing = request(cpu, tc, TestVaddr, MaxAddr, 4, BaseMMU::Read);
    mmu->translateTiming(timing, tc, &completion, BaseMMU::Read);
    expect(!completion.delayed && completion.finished == 1,
           "translate.bare-timing-one-completion");
    expectFault(completion.result, ExceptionCode::LOAD_PAGE,
                TestVaddr, tc, "translate.timing-page-fault");

    // 3. Direct PMAChecker integration at 0x80004000
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

    // 4. Smrnmi NMIE = 0 vs NMIE = 1 effect on MPRV effective privilege
    if (spmp->getNumEntries() && isa->enableSspmp() && isa->enableSmrnmi()) {
        clearEntries(spmp);
        napot(spmp, 0, TestVaddr, 16, CfgR | CfgU);
        STATUS mstatus = tc->readMiscRegNoEffect(MISCREG_STATUS);
        mstatus.mprv = 1;
        mstatus.sum = 0;
        mstatus.mxr = 0;
        mstatus.mpp = PRV_S;
        tc->setMiscRegNoEffect(MISCREG_STATUS, mstatus);
        tc->setMiscRegNoEffect(MISCREG_PRV, PRV_M);

        tc->setMiscRegNoEffect(MISCREG_NMIE, 0);
        auto nmieOff = request(cpu, tc, TestVaddr, MaxAddr, 4, BaseMMU::Read);
        expect(mmu->translateAtomic(nmieOff, tc, BaseMMU::Read) == NoFault,
               "effective-privilege.rnmi-nmie-zero-disables-mprv");

        tc->setMiscRegNoEffect(MISCREG_NMIE, 1);
        auto nmieOn = request(cpu, tc, TestVaddr, MaxAddr, 4, BaseMMU::Read);
        expectFault(mmu->translateAtomic(nmieOn, tc, BaseMMU::Read),
                    ExceptionCode::LOAD_PAGE, TestVaddr, tc,
                    "effective-privilege.rnmi-nmie-one-enables-mprv");

        tc->setMiscRegNoEffect(MISCREG_NMIE, 0);
        tc->setMiscRegNoEffect(MISCREG_STATUS, 0);
        tc->setMiscRegNoEffect(MISCREG_PRV, PRV_M);
    }

    inform("RiscvSPMPTest: direct fixture cases passed");
}

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
