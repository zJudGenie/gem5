/*
 * Copyright (c) 2026 Timber-V Project
 * All rights reserved.
 *
 * Timber-V Security Architecture and Domain Controller SimObject
 */

#include "arch/riscv/timberv.hh"

#include "arch/riscv/faults.hh"
#include "arch/riscv/mmu.hh"
#include "base/logging.hh"
#include "cpu/thread_context.hh"
#include "debug/TimberV.hh"

namespace gem5
{
namespace RiscvISA
{

TimberV::TimberV(const Params &params)
    : SimObject(params),
      secLevel(0),
      mtstatus(0),
      currentTtcbPtr(0)
{
}

void
TimberV::reset()
{
    secLevel = 0;
    mtstatus = 0;
    currentTtcbPtr = 0;
    timberVTags().reset();
}

bool
TimberV::trustedModeswitch(Addr paddr, PrivilegeMode pmode, uint64_t tag,
                           ThreadContext *tc)
{
    if (secLevel == 0) {
        if (tag == TIMBV_T_CALLABLE) {
            if (pmode == PrivilegeMode::PRV_U) {
                if (!isUtRunnable()) {
                    warn("Modeswitch to TRUSTED @ %#x failed: enclave not runnable!\n", paddr);
                    return false;
                }
            }
            setSecLevel(1);
            inform("Timber-V: Domain Modeswitch to TRUSTED (secLevel=1, TU-mode) @ %#x\n", paddr);
            if (tc) {
                tc->setMiscRegNoEffect(MISCREG_MTSTATUS, getMtstatus());
                tc->setMiscRegNoEffect(MISCREG_STSTATUS, getMtstatus());
                auto mmu = dynamic_cast<RiscvISA::MMU *>(tc->getMMUPtr());
                if (mmu) {
                    mmu->flushAll();
                }
            }
        }
    } else if (secLevel == 1 && tag == TIMBV_T_NORMAL) {
        setSecLevel(0);
        inform("Timber-V: Domain Modeswitch to NORMAL (secLevel=0, UN-mode) @ %#x\n", paddr);
        if (tc) {
            tc->setMiscRegNoEffect(MISCREG_MTSTATUS, getMtstatus());
            tc->setMiscRegNoEffect(MISCREG_STSTATUS, getMtstatus());
            auto mmu = dynamic_cast<RiscvISA::MMU *>(tc->getMMUPtr());
            if (mmu) {
                mmu->flushAll();
            }
        }
    }
    return true;
}

Fault
TimberV::checkAccess(const RequestPtr &req, BaseMMU::Mode mode,
                     PrivilegeMode pmode, ThreadContext *tc, Addr vaddr)
{
    if (pmode == PrivilegeMode::PRV_M) {
        return NoFault;
    }

    Addr fault_vaddr = req->hasVaddr() ? req->getVaddr() : vaddr;

    // On instruction fetch, evaluate Table III fetch check & horizontal domain transitions
    if (mode == BaseMMU::Execute) {
        Addr paddr = req->getPaddr();
        if (isTimberVActive()) {
            if (!timberVTags().checkAccess(paddr, (uint8_t)pmode, secLevel, TIMBV_FETCH)) {
                return std::make_shared<TagMismatchFault>(fault_vaddr);
            }
        }
        uint64_t tag = timberVTags().loadTag(paddr);
        if (!trustedModeswitch(paddr, pmode, tag, tc)) {
            return std::make_shared<TagMismatchFault>(fault_vaddr);
        }
    }

    // Table III: Global Tag Isolation Policy for Load and Store
    if (mode != BaseMMU::Execute && isTimberVActive()) {
        int accType = (mode == BaseMMU::Read) ? TIMBV_LOAD : TIMBV_STORE;
        Addr start = req->getPaddr();
        Addr end = req->getPaddr() + (req->getSize() ? req->getSize() - 1 : 0);
        for (Addr a = start; ; a += TIMBV_TAG_GRANULARITY) {
            if (!timberVTags().checkAccess(a, (uint8_t)pmode, secLevel, accType)) {
                return std::make_shared<TagMismatchFault>(fault_vaddr);
            }
            if ((a / TIMBV_TAG_GRANULARITY) == (end / TIMBV_TAG_GRANULARITY))
                break;
        }
    }

    return NoFault;
}

Fault
TimberV::createAccessFault(Addr vaddr, BaseMMU::Mode mode)
{
    ExceptionCode code;
    if (mode == BaseMMU::Read) {
        code = ExceptionCode::LOAD_ACCESS;
    } else if (mode == BaseMMU::Write) {
        code = ExceptionCode::STORE_ACCESS;
    } else {
        code = ExceptionCode::INST_ACCESS;
    }
    return std::make_shared<AddressFault>(vaddr, code);
}

void
TimberV::serialize(CheckpointOut &cp) const
{
    SERIALIZE_SCALAR(secLevel);
    SERIALIZE_SCALAR(mtstatus);
    SERIALIZE_SCALAR(currentTtcbPtr);
}

void
TimberV::unserialize(CheckpointIn &cp)
{
    UNSERIALIZE_SCALAR(secLevel);
    UNSERIALIZE_SCALAR(mtstatus);
    UNSERIALIZE_SCALAR(currentTtcbPtr);
}

} // namespace RiscvISA
} // namespace gem5
