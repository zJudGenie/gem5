/*
 * Copyright (c) 2021 The Regents of the University of California
 * Copyright (c) 2023 Google LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "arch/riscv/pmp.hh"

#include <algorithm>
#include <limits>

#include "arch/generic/tlb.hh"
#include "arch/riscv/faults.hh"
#include "arch/riscv/isa.hh"
#include "arch/riscv/regs/misc.hh"
#include "base/addr_range.hh"
#include "base/types.hh"
#include "cpu/thread_context.hh"
#include "debug/PMP.hh"
#include "math.h"
#include "mem/request.hh"
#include "params/PMP.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace RiscvISA
{

/** Allocate the writable physical slots and initialize partition controls. */
PMP::PMP(const Params &params) :
    SimObject(params),
    pmpEntries(params.pmp_entries),
    partition(params.pmp_entries),
    delegationEnabled(false),
    addrMask(params.riscv_type == RV64
        ? (Addr{1} << 54) - 1 : (Addr{1} << 32) - 1),
    numRules(0)
{
    fatal_if(pmpEntries < 0 || pmpEntries > 64,
             "PMP supports between zero and 64 entries, got %d", pmpEntries);
    pmpTable.resize(pmpEntries);
}

/** Read a PMP address only while its physical slot belongs to PMP. */
Addr
PMP::getAddr(uint32_t index) const
{
    return index < getNumEntries() ? pmpTable[index].rawAddr : 0;
}

/** Read a PMP configuration only while its physical slot belongs to PMP. */
uint64_t
PMP::getCfg(uint32_t index) const
{
    return index < getNumEntries() ? pmpTable[index].pmpCfg : 0;
}

/** Set the delegation capability and restore its reset partition. */
void
PMP::bindDelegation(bool enabled)
{
    delegationEnabled = enabled;
    if (enabled && partition > pmpEntries)
        partition = pmpEntries;
}

/** Clamp pmpnum and reject delegation of a currently locked PMP slot. */
bool
PMP::setPartition(RegVal value)
{
    if (!delegationEnabled)
        return false;
    const uint32_t candidate = std::min<uint32_t>(value & 0x7f, pmpEntries);
    for (uint32_t i = candidate; i < getNumEntries(); ++i) {
        if (pmpTable[i].pmpCfg & PMP_LOCK)
            return false;
    }
    partition = candidate;
    numRules = 0;
    for (uint32_t i = 0; i < pmpEntries; ++i)
        pmpUpdateRule(i);
    return true;
}

/** Read visible delegated enable bits while retaining hidden stored values. */
uint64_t
PMP::getSpmpEnable() const
{
    const uint32_t count = delegationEnabled ? pmpEntries - partition : 0;
    if (count == 0)
        return 0;
    const uint64_t visible = count == 64 ? ~uint64_t{0} :
        (uint64_t{1} << count) - 1;
    return (spmpEnable >> partition) & visible;
}

/** Update only visible, unlocked physical-slot enable bits. */
void
PMP::setSpmpEnable(uint64_t value, uint64_t mask)
{
    const uint32_t count = delegationEnabled ? pmpEntries - partition : 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t logicalBit = uint64_t{1} << i;
        if (!(mask & logicalBit))
            continue;
        const uint32_t slot = partition + i;
        if (pmpTable[slot].pmpCfg & PMP_LOCK)
            continue;
        const uint64_t physBit = uint64_t{1} << slot;
        if (value & logicalBit)
            spmpEnable |= physBit;
        else
            spmpEnable &= ~physBit;
    }
}

/** Return canonical raw address for a physical slot. */
Addr
PMP::sharedAddr(uint32_t index) const
{
    return index < pmpTable.size() ? pmpTable[index].rawAddr : 0;
}

/** Return canonical full configuration for a physical slot. */
uint64_t
PMP::sharedCfg(uint32_t index) const
{
    return index < pmpTable.size() ? pmpTable[index].pmpCfg : 0;
}

/** Update a shared address subject to the current role locks. */
bool
PMP::updateSharedAddr(uint32_t index, Addr value, bool machine_bank)
{
    if (index >= pmpEntries)
        return false;
    value &= addrMask;
    if (index < getNumEntries())
        return pmpUpdateAddr(index, value);
    if (!machine_bank && (pmpTable[index].pmpCfg & PMP_LOCK))
        return false;
    pmpTable[index].rawAddr = value;
    for (int i = 0; i < pmpEntries; ++i)
        pmpUpdateRule(i);
    return true;
}

/** Update shared configuration through the logical SPMP bank. */
bool
PMP::updateSharedCfg(uint32_t index, uint64_t value, bool machine_bank)
{
    if (index >= pmpEntries)
        return false;
    auto &entry = pmpTable[index];
    if (index < getNumEntries())
        return pmpUpdateCfg(index, value & 0xff);
    const bool locked = entry.pmpCfg & PMP_LOCK;
    if (locked && !machine_bank)
        return false;
    const uint64_t defined = 0x9f | (uint64_t{1} << 8) | (uint64_t{1} << 9);
    value &= defined;
    if (!(value & 1))
        value &= ~uint64_t{2};
    if (!(value & (uint64_t{1} << 8)))
        value &= ~(uint64_t{1} << 9);
    entry.pmpCfg = value;
    for (int i = 0; i < pmpEntries; ++i)
        pmpUpdateRule(i);
    return true;
}

/** Copy raw shared state and the partition from a compatible backend. */
void
PMP::takeOverFrom(const PMP &old)
{
    fatal_if(pmpEntries != old.pmpEntries ||
             delegationEnabled != old.delegationEnabled,
             "PMP takeover requires matching capacity and delegation mode");
    partition = old.partition;
    spmpEnable = old.spmpEnable;
    for (uint32_t i = 0; i < pmpEntries; ++i) {
        pmpTable[i].rawAddr = old.pmpTable[i].rawAddr;
        pmpTable[i].pmpCfg = old.pmpTable[i].pmpCfg;
    }
    for (uint32_t i = 0; i < pmpEntries; ++i)
        pmpUpdateRule(i);
}

/** Check the first overlapping PMP entry against the complete request. */
Fault
PMP::pmpCheck(const RequestPtr &req, BaseMMU::Mode mode,
              PrivilegeMode pmode, ThreadContext *tc, Addr vaddr)
{
    // First determine if pmp table should be consulted
    if (numRules == 0) {
        // If at least one PMP entry is implemented, but all PMP entries’ A
        // fields are set to OFF, then all S-mode and U-mode memory accesses
        // will fail.
        return createDefaultFault(req, mode, pmode, vaddr);
    }

    if (req->hasVaddr()) {
        DPRINTF(PMP, "Checking pmp permissions for va: %#x , pa: %#x\n",
                req->getVaddr(), req->getPaddr());
    }
    else { // this access is corresponding to a page table walk
        DPRINTF(PMP, "Checking pmp permissions for pa: %#x\n",
                req->getPaddr());
    }

    if (req->getSize() == 0 ||
        req->getSize() - 1 > std::numeric_limits<Addr>::max() -
                               req->getPaddr())
        return createAddrfault(req->hasVaddr() ? req->getVaddr() : vaddr,
                               mode);
    const Addr first = req->getPaddr();
    const Addr last = first + req->getSize() - 1;
    for (int i = 0; i < getNumEntries(); i++) {
        const AddrRange &range = pmpTable[i].pmpAddr;
        if (pmpGetAField(pmpTable[i].pmpCfg) == PMP_OFF || !range.valid() ||
            first >= range.end() || last < range.start())
            continue;
        if (first < range.start() || last >= range.end())
            return createAddrfault(req->hasVaddr() ? req->getVaddr() : vaddr,
                                   mode);

        {
            uint8_t this_cfg = pmpTable[i].pmpCfg;

            if ((pmode == PrivilegeMode::PRV_M) &&
                                    (PMP_LOCK & this_cfg) == 0) {
                return NoFault;
            } else if ((mode == BaseMMU::Mode::Read) &&
                                        (PMP_READ & this_cfg)) {
                return NoFault;
            } else if ((mode == BaseMMU::Mode::Write) &&
                                        (PMP_WRITE & this_cfg)) {
                return NoFault;
            } else if ((mode == BaseMMU::Mode::Execute) &&
                                        (PMP_EXEC & this_cfg)) {
                return NoFault;
            } else {
                if (req->hasVaddr()) {
                    return createAddrfault(req->getVaddr(), mode);
                } else {
                    return createAddrfault(vaddr, mode);
                }
            }
        }
    }
    return createDefaultFault(req, mode, pmode, vaddr);
}

Fault
PMP::createAddrfault(Addr vaddr, BaseMMU::Mode mode)
{
    ExceptionCode code;
    if (mode == BaseMMU::Read) {
        code = ExceptionCode::LOAD_ACCESS;
    } else if (mode == BaseMMU::Write) {
        code = ExceptionCode::STORE_ACCESS;
    } else {
        code = ExceptionCode::INST_ACCESS;
    }
    warn("pmp access fault.\n");
    return std::make_shared<AddressFault>(vaddr, code);
}

/** Apply the default PMP policy using the currently owned prefix count. */
Fault
PMP::createDefaultFault(const RequestPtr &req, BaseMMU::Mode mode,
                        PrivilegeMode pmode, Addr vaddr)
{
    if (pmode == PrivilegeMode::PRV_M || getNumEntries() == 0) {
        return NoFault;
    } else if (req->hasVaddr()) {
        return createAddrfault(req->getVaddr(), mode);
    } else {
        return createAddrfault(vaddr, mode);
    }
}

inline uint8_t
PMP::pmpGetAField(uint64_t cfg)
{
    // to get a field from pmpcfg register
    uint8_t a = cfg >> 3;
    return a & 0x03;
}


/** Write a visible PMP byte without discarding the physical SPMP fields. */
bool
PMP::pmpUpdateCfg(uint32_t pmp_index, uint8_t this_cfg)
{
    if (pmp_index >= getNumEntries()) {
        DPRINTF(PMP, "Can't update pmp entry config %u"
                " because the index exceed the size of pmp entries %u",
                pmp_index, pmpEntries);
        return false;
    }

    DPRINTF(PMP, "Update pmp config with %u for pmp entry: %u \n",
                                    (unsigned)this_cfg, pmp_index);
    if (pmpTable[pmp_index].pmpCfg & PMP_LOCK) {
        DPRINTF(PMP, "Update pmp entry config %u failed because it locked\n",
                pmp_index);
        return false;
    }
    const uint8_t defined = PMP_READ | PMP_WRITE | PMP_EXEC | PMP_A_MASK |
                            PMP_LOCK;
    this_cfg &= defined;
    if ((this_cfg & PMP_READ) == 0)
        this_cfg &= ~PMP_WRITE;
    pmpTable[pmp_index].pmpCfg =
        (pmpTable[pmp_index].pmpCfg & ~uint64_t{0xff}) | this_cfg;
    for (uint32_t i = 0; i < pmpEntries; ++i)
        pmpUpdateRule(i);
    return true;
}

/** Rebuild one physical range and count active entries in the PMP prefix. */
/** Decode one physical range and count only PMP-owned active entries. */
void
PMP::pmpUpdateRule(uint32_t pmp_index)
{
    // In qemu, the rule is updated whenever
    // pmpaddr/pmpcfg is written

    numRules = 0;
    Addr prevAddr = 0;

    if (pmp_index >= 1) {
        prevAddr = pmpTable[pmp_index - 1].rawAddr;
    }

    Addr this_addr = pmpTable[pmp_index].rawAddr;
    uint64_t this_cfg = pmpTable[pmp_index].pmpCfg;
    AddrRange this_range;

    switch (pmpGetAField(this_cfg)) {
      // checking the address matching mode of pmp entry
      case PMP_OFF:
        // null region (pmp disabled)
        this_range = AddrRange(0, 0);
        break;
      case PMP_TOR:
        // top of range mode
        this_range = AddrRange(prevAddr << 2,
            this_addr <= prevAddr ? prevAddr << 2 : this_addr << 2);
        break;
      case PMP_NA4:
        // naturally aligned four byte region
        this_range = AddrRange(this_addr << 2, ((this_addr << 2) + 4));
        break;
      case PMP_NAPOT:
        // naturally aligned power of two region, >= 8 bytes
        this_range = AddrRange(pmpDecodeNapot(this_addr));
        break;
      default:
        this_range = AddrRange(0,0);
    }

    pmpTable[pmp_index].pmpAddr = this_range;

    for (int i = 0; i < getNumEntries(); i++) {
        const uint8_t a_field = pmpGetAField(pmpTable[i].pmpCfg);
      if (PMP_OFF != a_field) {
          numRules++;
      }
    }
}

/** Reset partition and enable controls, retaining raw addresses and R/W/X. */
void
PMP::pmpReset()
{
    if (delegationEnabled)
        partition = pmpEntries;
    spmpEnable = 0;
    for (uint32_t i = 0; i < pmpTable.size(); i++) {
        pmpTable[i].pmpCfg &= ~(PMP_A_MASK | PMP_LOCK);
        pmpUpdateRule(i);
    }
}

/** Write a visible PMP address subject to entry and successor TOR locks. */
bool
PMP::pmpUpdateAddr(uint32_t pmp_index, Addr this_addr)
{
    if (pmp_index >= getNumEntries()) {
        DPRINTF(PMP, "Can't update pmp entry address %u"
                " because the index exceed the size of pmp entries %u",
                pmp_index, pmpEntries);
        return false;
    }

    DPRINTF(PMP, "Update pmp addr %#x for pmp entry %u \n",
                                      (this_addr << 2), pmp_index);

    if (pmpTable[pmp_index].pmpCfg & PMP_LOCK) {
        DPRINTF(PMP, "Update pmp entry %u failed because the lock bit set\n",
                pmp_index);
        return false;
    } else if (pmp_index + 1 < getNumEntries() &&
               ((pmpTable[pmp_index+1].pmpCfg & PMP_LOCK) != 0) &&
               pmpGetAField(pmpTable[pmp_index+1].pmpCfg) == PMP_TOR) {
        DPRINTF(PMP, "Update pmp entry %u failed because the entry %u lock bit"
                " set and A field is TOR\n",
                pmp_index, pmp_index+1);
        return false;
    }

    // just writing the raw addr in the pmp table
    // will convert it into a range, once cfg
    // reg is written
    pmpTable[pmp_index].rawAddr = this_addr & addrMask;
    for (int index = 0; index < pmpEntries; index++) {
        pmpUpdateRule(index);
    }

    return true;
}

AddrRange
PMP::pmpDecodeNapot(Addr pmpaddr)
{
    pmpaddr &= addrMask;
    if (pmpaddr == addrMask) {
        const Addr physLimit = (addrMask + 1) << 2;
        return AddrRange(0, physLimit);
    } else {
        uint64_t t1 = ctz64(~pmpaddr);
        uint64_t range = (1ULL << (t1+3));

        // pmpaddr reg encodes bits 55-2 of a
        // 56 bit physical address for RV64
        uint64_t base = mbits(pmpaddr, 63, t1) << 2;
        AddrRange this_range(base, base+range);
        return this_range;
    }
}

/** Save canonical physical entries and shared protection controls. */
void
PMP::serialize(CheckpointOut &cp) const
{
    int cptPmpEntries = pmpEntries;
    int cptDelegationEnabled = delegationEnabled;
    int cptPartition = partition;
    SERIALIZE_SCALAR(cptPmpEntries);
    SERIALIZE_SCALAR(cptDelegationEnabled);
    SERIALIZE_SCALAR(cptPartition);
    SERIALIZE_SCALAR(spmpEnable);
    for (int i = 0; i < pmpEntries; i++) {
        pmpTable[i].serializeSection(cp, csprintf("Entry%d", i));
    }
}
/** Restore all raw state before rebuilding logical PMP ranges. */
void
PMP::unserialize(CheckpointIn &cp)
{
    int cptPmpEntries;
    int cptDelegationEnabled;
    int cptPartition;
    UNSERIALIZE_SCALAR(cptPmpEntries);
    UNSERIALIZE_SCALAR(cptDelegationEnabled);
    UNSERIALIZE_SCALAR(cptPartition);
    UNSERIALIZE_SCALAR(spmpEnable);
    if (cptPmpEntries != pmpEntries) {
        fatal("Current PMP table size (%d) differs from checkpoint PMP table "
              "size (%d).",
              pmpEntries, cptPmpEntries);
    }
    fatal_if((cptDelegationEnabled != 0) != delegationEnabled ||
             cptPartition < 0 || cptPartition > pmpEntries,
             "PMP checkpoint capacity or delegation profile is incompatible");
    partition = cptPartition;

    for (int i = 0; i < pmpEntries; i++) {
        PmpEntry *tmp = &pmpTable[i];
        tmp->unserializeSection(cp, csprintf("Entry%d", i));
    }
    for (int i = 0; i < pmpEntries; i++)
        pmpUpdateRule(i);
}

} // namespace RiscvISA
} // namespace gem5
