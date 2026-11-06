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
    slots.resize(params.num_slots);
    for (size_t i = 0; i < slots.size(); i++) {
        slots[i].base = 0;
        slots[i].bound = (Addr)-1;
        slots[i].flags = 0x7;
    }
}

void
TimberV::updateBase(uint32_t index, Addr base, PrivilegeMode prv)
{
    if (index < slots.size()) {
        if (prv == PrivilegeMode::PRV_S && secLevel == 0) {
            // SN cannot overwrite an existing ST entry
            if (slots[index].flags & SPMP_ST) {
                warn("Cannot overwrite Timber-V ST entry base from SN! Ignoring.\n");
                return;
            }
            // Changing base from SN automatically clears ACK
            slots[index].flags &= ~SPMP_ACK;
        }
        slots[index].base = base;
    }
}

void
TimberV::updateBound(uint32_t index, Addr bound, PrivilegeMode prv)
{
    if (index < slots.size()) {
        if (prv == PrivilegeMode::PRV_S && secLevel == 0) {
            // SN cannot overwrite an existing ST entry
            if (slots[index].flags & SPMP_ST) {
                warn("Cannot overwrite Timber-V ST entry bound from SN! Ignoring.\n");
                return;
            }
            // Changing bound from SN automatically clears ACK
            slots[index].flags &= ~SPMP_ACK;
        }
        slots[index].bound = bound;
    }
}

void
TimberV::updateFlags(uint32_t index, uint8_t flags, PrivilegeMode prv)
{
    if (index < slots.size()) {
        if (prv == PrivilegeMode::PRV_S && secLevel == 0) {
            // SN cannot write ST flag or overwrite existing ST entry
            if ((flags & SPMP_ST) || (slots[index].flags & SPMP_ST)) {
                warn("Cannot (over)write Timber-V ST entry from SN! Ignoring.\n");
                return;
            }
            // SN automatically clears ACK
            flags &= ~SPMP_ACK;
        }
        slots[index].flags = flags;
    }
}

void
TimberV::reset()
{
    secLevel = 0;
    mtstatus = 0;
    currentTtcbPtr = 0;
    for (size_t i = 0; i < slots.size(); i++) {
        slots[i].base = 0;
        slots[i].bound = (Addr)-1;
        slots[i].flags = 0x7;
    }
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

    // Timber-V slot permission checking matching Spike
    if (pmode == PrivilegeMode::PRV_S) {
        if (mode == BaseMMU::Execute) {
            if (secLevel == 1) { // ST mode fetch
                bool st_found = false;
                for (size_t i = 0; i < slots.size(); i++) {
                    Addr base = slots[i].base;
                    Addr bound = slots[i].bound;
                    uint8_t flags = slots[i].flags;
                    if (req->getPaddr() >= base &&
                        req->getPaddr() + req->getSize() - 1 <= bound &&
                        (flags & (1 << SPMP_X_BIT))) {
                        if (flags & SPMP_ST) {
                            st_found = true;
                            break;
                        }
                    }
                }
                if (!st_found) {
                    return createAccessFault(fault_vaddr, mode);
                }
            }
            // SN mode fetch is allowed by SPMP
        }
        // S-mode load/store: S can read/write anything in physical memory
    } else if (pmode == PrivilegeMode::PRV_U) {
        bool u_ok = false;
        for (size_t i = 0; i < slots.size(); i++) {
            Addr base = slots[i].base;
            Addr bound = slots[i].bound;
            uint8_t flags = slots[i].flags;
            uint8_t perm = flags & 0x7;

            if (req->getPaddr() >= base &&
                req->getPaddr() + req->getSize() - 1 <= bound) {
                bool r_ok = (mode == BaseMMU::Mode::Read) && (perm & (1 << SPMP_R_BIT));
                bool w_ok = (mode == BaseMMU::Mode::Write) && (perm & (1 << SPMP_W_BIT));
                bool x_ok = (mode == BaseMMU::Mode::Execute) && (perm & (1 << SPMP_X_BIT));

                if (r_ok || w_ok || x_ok) {
                    // UT fetch requires entry with T-flag which is ACK'ed
                    if (secLevel == 1 && mode == BaseMMU::Execute) {
                        if (!(flags & SPMP_T) || !(flags & SPMP_ACK)) {
                            continue;
                        }
                    }
                    // User mode cannot access ST range
                    if (flags & SPMP_ST) {
                        continue;
                    }
                    u_ok = true;
                    break;
                }
            }
        }

        if (!u_ok) {
            return createAccessFault(fault_vaddr, mode);
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
    for (size_t i = 0; i < slots.size(); i++) {
        slots[i].serializeSection(cp, csprintf("TimberVSlot%zu", i));
    }
    SERIALIZE_SCALAR(secLevel);
    SERIALIZE_SCALAR(mtstatus);
    SERIALIZE_SCALAR(currentTtcbPtr);
}

void
TimberV::unserialize(CheckpointIn &cp)
{
    for (size_t i = 0; i < slots.size(); i++) {
        slots[i].unserializeSection(cp, csprintf("TimberVSlot%zu", i));
    }
    UNSERIALIZE_SCALAR(secLevel);
    UNSERIALIZE_SCALAR(mtstatus);
    UNSERIALIZE_SCALAR(currentTtcbPtr);
}

} // namespace RiscvISA
} // namespace gem5
