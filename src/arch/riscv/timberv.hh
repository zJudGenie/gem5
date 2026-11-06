/*
 * Copyright (c) 2026 Timber-V Project
 * All rights reserved.
 *
 * Timber-V Security Architecture and Domain Controller SimObject
 */

#ifndef __ARCH_RISCV_TIMBERV_HH__
#define __ARCH_RISCV_TIMBERV_HH__

#include <vector>

#include "arch/generic/tlb.hh"
#include "arch/riscv/faults.hh"
#include "arch/riscv/isa.hh"
#include "arch/riscv/regs/misc.hh"
#include "arch/riscv/timberv_tag.hh"
#include "base/types.hh"
#include "mem/request.hh"
#include "params/TimberV.hh"
#include "sim/serialize.hh"
#include "sim/sim_object.hh"

namespace gem5
{
namespace RiscvISA
{

class TimberV : public SimObject
{
  public:
    PARAMS(TimberV);
    TimberV(const Params &params);

    // Timber-V tstatus flags
    static constexpr uint64_t TSTATUS_EN = 0x00000001;
    static constexpr uint64_t TSTATUS_MO = 0x00000002; // Mode: 1=Secure, 0=Normal (read-only)
    static constexpr uint64_t TSTATUS_UE = 0x00010000; // User Enable (runnable)
    static constexpr uint64_t TSTATUS_UI = 0x00020000; // UT Interrupted

    // Timber-V slot flag bits
    static constexpr uint8_t SPMP_R_BIT   = 0;
    static constexpr uint8_t SPMP_W_BIT   = 1;
    static constexpr uint8_t SPMP_X_BIT   = 2;
    static constexpr uint8_t SPMP_T_BIT   = 3;
    static constexpr uint8_t SPMP_ACK_BIT = 4;
    static constexpr uint8_t SPMP_ST_BIT  = 5;

    static constexpr uint8_t SPMP_T   = (1 << SPMP_T_BIT);
    static constexpr uint8_t SPMP_ACK = (1 << SPMP_ACK_BIT);
    static constexpr uint8_t SPMP_ST  = (1 << SPMP_ST_BIT);

    struct SlotEntry : public Serializable
    {
        Addr base = 0;
        Addr bound = (Addr)-1;
        uint8_t flags = 0x7; // Default RWX

        void serialize(CheckpointOut &cp) const override
        {
            SERIALIZE_SCALAR(base);
            SERIALIZE_SCALAR(bound);
            SERIALIZE_SCALAR(flags);
        }
        void unserialize(CheckpointIn &cp) override
        {
            UNSERIALIZE_SCALAR(base);
            UNSERIALIZE_SCALAR(bound);
            UNSERIALIZE_SCALAR(flags);
        }
    };

    uint8_t getSecLevel() const { return secLevel; }
    void setSecLevel(uint8_t s) { secLevel = s ? 1 : 0; }

    uint64_t getMtstatus() const
    {
        uint64_t v = mtstatus;
        if (secLevel) {
            v |= TSTATUS_MO;
        } else {
            v &= ~TSTATUS_MO;
        }
        return v;
    }

    void setMtstatus(uint64_t val)
    {
        uint64_t old_mode = mtstatus & TSTATUS_MO;
        mtstatus = (val & ~TSTATUS_MO) | old_mode;
    }

    bool isTimberVActive() const { return (mtstatus & TSTATUS_EN) != 0; }
    bool isUtRunnable() const { return (mtstatus & TSTATUS_UE) != 0 && (mtstatus & TSTATUS_UI) == 0; }

    void notifyInterrupt(PrivilegeMode prv)
    {
        if (isTimberVActive() && secLevel == 1 &&
            prv == PrivilegeMode::PRV_U && isUtRunnable()) {
            mtstatus |= TSTATUS_UI;
        }
    }

    void updateTtcb(Addr new_ttcb) { currentTtcbPtr = new_ttcb; }
    Addr getTtcb() const { return currentTtcbPtr; }

    void updateBase(uint32_t index, Addr base, PrivilegeMode prv = PrivilegeMode::PRV_M);
    void updateBound(uint32_t index, Addr bound, PrivilegeMode prv = PrivilegeMode::PRV_M);
    void updateFlags(uint32_t index, uint8_t flags, PrivilegeMode prv = PrivilegeMode::PRV_M);

    Addr getBase(uint32_t index) const { return index < slots.size() ? slots[index].base : 0; }
    Addr getBound(uint32_t index) const { return index < slots.size() ? slots[index].bound : 0; }
    uint8_t getFlags(uint32_t index) const { return index < slots.size() ? slots[index].flags : 0; }

    void reset();

    bool trustedModeswitch(Addr paddr, PrivilegeMode pmode, uint64_t tag, ThreadContext *tc);

    Fault checkAccess(const RequestPtr &req, BaseMMU::Mode mode,
                      PrivilegeMode pmode, ThreadContext *tc, Addr vaddr = 0);

    void serialize(CheckpointOut &cp) const override;
    void unserialize(CheckpointIn &cp) override;

  private:
    Fault createAccessFault(Addr vaddr, BaseMMU::Mode mode);

    uint8_t secLevel;
    uint64_t mtstatus;
    Addr currentTtcbPtr;
    std::vector<SlotEntry> slots;
};

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_TIMBERV_HH__
