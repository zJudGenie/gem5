#include "arch/riscv/spmp.hh"

#include "arch/riscv/faults.hh"
#include "arch/riscv/pagetable.hh"
#include "arch/riscv/pmp.hh"
#include "arch/riscv/regs/misc.hh"
#include "base/bitfield.hh"
#include "base/logging.hh"
#include "cpu/thread_context.hh"
#include "debug/SPMP.hh"

namespace gem5
{

namespace RiscvISA
{

/** Initialize the implemented table and the physical address mask. */
SPMP::SPMP(const Params &params)
    : SimObject(params),
      spmpEntries(params.spmp_entries),
      delegation(params.delegation_enabled),
      addrMask(params.riscv_type == RV64
          ? (Addr{1} << 54) - 1 : (Addr{1} << 32) - 1),
      numRules(0)
{
    fatal_if(!delegation && (spmpEntries < 0 || spmpEntries > 64),
             "SPMP supports between zero and 64 entries, got %d",
             spmpEntries);
    if (!delegation)
        spmpTable.resize(spmpEntries);
}

/** Bind this SPMP view to its canonical PMP storage owner. */
void
SPMP::bindExtensions(PMP *owner, bool enable_mask, bool delegate)
{
    pmp = owner;
    enableMask = enable_mask;
    fatal_if(delegation != delegate,
             "SPMP configured delegation mode differs from the hart profile");
    delegation = delegate;
    if (pmp)
        pmp->bindDelegation(delegate);
}

/** Return the live number of logical SPMP entries. */
uint32_t
SPMP::getNumEntries() const
{
    return delegation && pmp ? pmp->getCapacity() - pmp->getPartition() :
        spmpEntries;
}

/** Return the backing capacity for this SPMP profile. */
uint32_t
SPMP::getCapacity() const
{
    return delegation && pmp ? pmp->getCapacity() : spmpEntries;
}

/** Read a canonical SPMP address, returning zero for hidden indices. */
Addr
SPMP::getAddr(uint32_t index) const
{
    if (index >= getNumEntries())
        return 0;
    // The selected platform interpretation maps logical SPMP indices onto
    // the physical suffix beginning at pmpnum; resizing can remap survivors.
    return delegation ? pmp->sharedAddr(pmp->getPartition() + index) :
        spmpTable[index].rawAddr;
}

/** Read a canonical SPMP configuration, returning zero for hidden indices. */
uint64_t
SPMP::getCfg(uint32_t index) const
{
    if (index >= getNumEntries())
        return 0;
    return delegation ? pmp->sharedCfg(pmp->getPartition() + index) :
        spmpTable[index].spmpCfg;
}

/** Return only enable bits within the live logical SPMP range. */
uint64_t
SPMP::getEnable() const
{
    if (delegation)
        return pmp->getSpmpEnable();
    const uint32_t count = getNumEntries();
    const uint64_t visible = count == 64 ? ~uint64_t{0} :
        (count == 0 ? 0 : (uint64_t{1} << count) - 1);
    return spmpEnable & visible;
}

/** Apply visible enable writes while respecting mapped entry locks. */
void
SPMP::setEnable(uint64_t value, uint64_t mask)
{
    if (delegation) {
        pmp->setSpmpEnable(value, mask);
        return;
    }
    const uint32_t count = getNumEntries();
    const uint64_t visible = count == 64 ? ~uint64_t{0} :
        (count == 0 ? 0 : (uint64_t{1} << count) - 1);
    uint64_t writable = visible & mask;
    for (uint32_t i = 0; i < count; ++i)
        if (getCfg(i) & SPMP_LOCK)
            writable &= ~(uint64_t{1} << i);
    spmpEnable = (spmpEnable & ~writable) | (value & writable);
}

/** Check one physical access against the first overlapping SPMP entry. */
Fault
SPMP::spmpCheck(const RequestPtr &req, BaseMMU::Mode mode,
                PrivilegeMode pmode, ThreadContext *tc, Addr vaddr)
{
    const uint32_t count = getNumEntries();
    if (pmode == PrivilegeMode::PRV_M || count == 0)
        return NoFault;

    const RegVal satp = tc->readMiscRegNoEffect(MISCREG_SATP);
    const bool rv32 = addrMask == ((Addr{1} << 32) - 1);
    const RegVal satpMode = rv32 ? bits(satp, 31) : bits(satp, 63, 60);
    if (satpMode != AddrXlateMode::BARE)
        return NoFault;

    // AMOs require both permissions and always report store/AMO page faults.
    // LR/SC use LLSC rather than the atomic-operation flags: LR remains a load.
    const bool amo = req->isAtomic() || req->hasAtomicOpFunctor();
    if (amo)
        mode = BaseMMU::Write;
    const Addr faultVaddr = req->hasVaddr() ? req->getVaddr() : vaddr;
    const Addr physLimit = (addrMask + 1) << 2;
    const Addr paddr = req->getPaddr();
    const Addr size = req->getSize();
    if (size == 0 || paddr >= physLimit ||
        size - 1 > (physLimit - 1) - paddr) {
        return createAddrfault(faultVaddr, mode);
    }
    const Addr last = paddr + size - 1;

    DPRINTF(SPMP, "Checking SPMP permissions for pa=%#x size=%#x\n",
            paddr, size);
    const uint64_t enabled = getEnable();
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t cfg = getCfg(i);
        if (enableMask && !(enabled & (uint64_t{1} << i)))
            continue;
        const uint8_t a = spmpGetAField(cfg);
        const AddrRange range = getRange(i);
        if (a == SPMP_OFF || !range.valid() || range.start() >= range.end())
            continue;

        if (paddr >= range.end() || last < range.start())
            continue;

        if (paddr < range.start() || last >= range.end())
            return createAddrfault(faultVaddr, mode);

        const bool userRule = (cfg & SPMP_USER) != 0;
        const bool shared = (cfg & SPMP_SHARED) != 0;
        const uint64_t rwx = cfg & (SPMP_READ | SPMP_WRITE | SPMP_EXEC);
        bool read = (cfg & SPMP_READ) != 0;
        bool write = (cfg & SPMP_WRITE) != 0;
        bool execute = (cfg & SPMP_EXEC) != 0;

        if (pmode == PrivilegeMode::PRV_U) {
            if (!userRule)
                return createAddrfault(faultVaddr, mode);
            // Figure 4 is labelled R,W,X, not the packed bit order X,W,R.
            if (shared && rwx == (SPMP_READ | SPMP_WRITE))
                write = execute = false;
            else if (shared && rwx ==
                     (SPMP_READ | SPMP_WRITE | SPMP_EXEC))
                read = write = false;
        } else if (userRule && !shared) {
            if (!(tc->readMiscRegNoEffect(MISCREG_STATUS) & STATUS_SUM_MASK))
                return createAddrfault(faultVaddr, mode);
            execute = false;
        }

        const bool allow = mode == BaseMMU::Read ? read :
            mode == BaseMMU::Write ? write : execute;
        if (!allow || (amo && !read))
            return createAddrfault(faultVaddr, mode);
        return NoFault;
    }

    return createDefaultFault(req, mode, pmode, faultVaddr);
}

/** Construct the SPMP page fault corresponding to the access type. */
Fault
SPMP::createAddrfault(Addr vaddr, BaseMMU::Mode mode)
{
    ExceptionCode code;
    if (mode == BaseMMU::Mode::Read)
        code = ExceptionCode::LOAD_PAGE;
    else if (mode == BaseMMU::Mode::Write)
        code = ExceptionCode::STORE_PAGE;
    else
        code = ExceptionCode::INST_PAGE;
    DPRINTF(SPMP, "SPMP page fault at virtual address %#x\n", vaddr);
    return std::make_shared<AddressFault>(vaddr, code);
}

/** Deny S/U accesses when an implemented table has no matching rule. */
Fault
SPMP::createDefaultFault(const RequestPtr &req, BaseMMU::Mode mode,
                         PrivilegeMode pmode, Addr vaddr)
{
    if (pmode == PrivilegeMode::PRV_M || getNumEntries() == 0)
        return NoFault;
    const Addr faultVaddr = req->hasVaddr() ? req->getVaddr() : vaddr;
    return createAddrfault(faultVaddr, mode);
}

/** Extract only the two encoded A bits before narrowing the result. */
inline uint8_t
SPMP::spmpGetAField(uint64_t cfg)
{
    return static_cast<uint8_t>((cfg >> 3) & 0x03);
}

/** Decode a masked NAPOT address into a range clipped to physical space. */
inline AddrRange
SPMP::spmpDecodeNapot(Addr spmpaddr) const
{
    spmpaddr &= addrMask;
    const Addr physLimit = (addrMask + 1) << 2;
    if (spmpaddr == addrMask)
        return AddrRange(0, physLimit);

    const uint64_t n = ctz64(~spmpaddr);
    const Addr lowMask = (Addr{1} << n) - 1;
    const Addr base = (spmpaddr & ~lowMask) << 2;
    const Addr size = Addr{1} << (n + 3);
    return AddrRange(base, base + size);
}

/** Canonicalize an accepted configuration and refresh its derived ranges. */
bool
SPMP::spmpUpdateCfg(uint32_t spmp_index, uint64_t this_cfg, bool machine_bank)
{
    if (spmp_index >= getNumEntries()) {
        DPRINTF(SPMP, "Cannot update configuration: "
                "index %u exceeds %d entries\n",
                spmp_index, getCapacity());
        return false;
    }

    const uint64_t old_cfg = getCfg(spmp_index);
    if (delegation && pmp) {
        const uint64_t defined = 0x9f | SPMP_USER | SPMP_SHARED;
        this_cfg &= defined;
        if (!(this_cfg & SPMP_READ))
            this_cfg &= ~SPMP_WRITE;
        if (!(this_cfg & SPMP_USER))
            this_cfg &= ~SPMP_SHARED;
        return pmp->updateSharedCfg(pmp->getPartition() + spmp_index,
                                    this_cfg, machine_bank);
    }
    SpmpEntry &entry = spmpTable[spmp_index];
    if ((old_cfg & SPMP_LOCK) && !machine_bank) {
        DPRINTF(SPMP, "Cannot update locked entry %u configuration\n",
                spmp_index);
        return false;
    }

    const uint64_t defined = SPMP_READ | SPMP_WRITE | SPMP_EXEC | SPMP_A_MASK |
                             SPMP_LOCK | SPMP_USER | SPMP_SHARED;
    this_cfg &= defined;
    // Reserved RWX (R=0,W=1) becomes W=0; reserved SHARED/U becomes
    // non-shared. All defined bits in legal encodings survive readback.
    if ((this_cfg & SPMP_READ) == 0)
        this_cfg &= ~SPMP_WRITE;
    if ((this_cfg & SPMP_USER) == 0)
        this_cfg &= ~SPMP_SHARED;
    entry.spmpCfg = this_cfg;
    for (uint32_t i = 0; i < static_cast<uint32_t>(spmpEntries); ++i)
        spmpUpdateRule(i);
    return true;
}

/** Rebuild one decoded range and maintain the configured-rule count. */
void
SPMP::spmpUpdateRule(uint32_t spmp_index)
{
    if (delegation)
        return;
    const Addr raw = spmpTable[spmp_index].rawAddr & addrMask;
    const Addr previous = spmp_index == 0 ? 0 :
        (spmpTable[spmp_index - 1].rawAddr & addrMask);
    const uint8_t a = spmpGetAField(spmpTable[spmp_index].spmpCfg);

    switch (a) {
      case SPMP_OFF:
        spmpTable[spmp_index].spmpAddr = AddrRange(0, 0);
        break;
      case SPMP_TOR: {
        const Addr start = previous << 2;
        const Addr end = raw <= previous ? start : raw << 2;
        spmpTable[spmp_index].spmpAddr = AddrRange(start, end);
        break;
      }
      case SPMP_NA4: {
        const Addr start = raw << 2;
        spmpTable[spmp_index].spmpAddr = AddrRange(start, start + 4);
        break;
      }
      case SPMP_NAPOT:
        spmpTable[spmp_index].spmpAddr = spmpDecodeNapot(raw);
        break;
      default:
        spmpTable[spmp_index].spmpAddr = AddrRange(0, 0);
    }

    numRules = 0;
    for (const SpmpEntry &entry : spmpTable) {
        if (spmpGetAField(entry.spmpCfg) != SPMP_OFF)
            ++numRules;
    }
}

/** Decode a range using the logical predecessor in the current partition. */
AddrRange
SPMP::getRange(uint32_t spmp_index) const
{
    if (!delegation)
        return spmpTable[spmp_index].spmpAddr;
    const uint64_t cfg = getCfg(spmp_index);
    const Addr raw = getAddr(spmp_index) & addrMask;
    const Addr previous = spmp_index == 0 ? 0 :
        (getAddr(spmp_index - 1) & addrMask);
    switch (spmpGetAField(cfg)) {
      case SPMP_OFF: return AddrRange(0, 0);
      case SPMP_TOR: {
        const Addr start = previous << 2;
        return AddrRange(start, raw <= previous ? start : raw << 2);
      }
      case SPMP_NA4: return AddrRange(raw << 2, (raw << 2) + 4);
      case SPMP_NAPOT: return spmpDecodeNapot(raw);
      default: return AddrRange(0, 0);
    }
}

/**
 * Reset standalone A/L and enable state, retaining addresses and other fields.
 * In delegation mode the MMU resets all canonical shared state through PMP.
 */
void
SPMP::spmpReset()
{
    if (!delegation)
        spmpEnable = 0;
    for (SpmpEntry &entry : spmpTable)
        entry.spmpCfg &= ~(SPMP_A_MASK | SPMP_LOCK);
    if (!delegation) {
        for (uint32_t i = 0; i < static_cast<uint32_t>(spmpEntries); ++i)
            spmpUpdateRule(i);
    }
}

/** Apply the entry and successor TOR locks before storing a masked address. */
bool
SPMP::spmpUpdateAddr(uint32_t spmp_index, Addr this_addr, bool machine_bank)
{
    if (spmp_index >= getNumEntries()) {
        DPRINTF(SPMP, "Cannot update address: index %u exceeds %d entries\n",
                spmp_index, getCapacity());
        return false;
    }

    if (!machine_bank) {
        if (getCfg(spmp_index) & SPMP_LOCK)
            return false;
        const uint32_t next = spmp_index + 1;
        if (next < getNumEntries() &&
            (getCfg(next) & SPMP_LOCK) &&
            spmpGetAField(getCfg(next)) == SPMP_TOR) {
            return false;
        }
    }
    if (delegation)
        return pmp->updateSharedAddr(pmp->getPartition() + spmp_index,
                                     this_addr & addrMask, machine_bank);
    spmpTable[spmp_index].rawAddr = this_addr & addrMask;
    for (uint32_t i = 0; i < static_cast<uint32_t>(spmpEntries); ++i)
        spmpUpdateRule(i);
    return true;
}

/** Copy raw locked state without replaying writes or changing child ownership. */
void
SPMP::takeOverFrom(const SPMP &old)
{
    fatal_if(addrMask != old.addrMask,
             "SPMP takeover requires matching hart XLEN");
    fatal_if(getCapacity() != old.getCapacity() ||
             delegation != old.delegation || enableMask != old.enableMask,
             "SPMP takeover requires matching entry counts (%d != %d)",
             getCapacity(), old.getCapacity());
    if (!delegation) {
        for (uint32_t i = 0; i < static_cast<uint32_t>(spmpEntries); ++i) {
            spmpTable[i].rawAddr = old.spmpTable[i].rawAddr;
            spmpTable[i].spmpCfg = old.spmpTable[i].spmpCfg;
        }
    }
    if (!delegation)
        spmpEnable = old.spmpEnable;
    if (!delegation) {
        for (uint32_t i = 0; i < static_cast<uint32_t>(spmpEntries); ++i)
            spmpUpdateRule(i);
    }
}

/** Serialize raw architected state using the established checkpoint keys. */
void
SPMP::serialize(CheckpointOut &cp) const
{
    int cptSpmpEntries = getCapacity();
    int cptEnableMask = enableMask;
    int cptDelegation = delegation;
    SERIALIZE_SCALAR(cptEnableMask);
    SERIALIZE_SCALAR(cptDelegation);
    SERIALIZE_SCALAR(cptSpmpEntries);
    if (!delegation) {
        SERIALIZE_SCALAR(spmpEnable);
        for (int i = 0; i < spmpEntries; ++i)
            spmpTable[i].serializeSection(cp, csprintf("Entry%d", i));
    }
}

/** Restore raw state before deriving ranges so TOR sees restored predecessors. */
void
SPMP::unserialize(CheckpointIn &cp)
{
    int cptSpmpEntries;
    int cptEnableMask;
    int cptDelegation;
    UNSERIALIZE_SCALAR(cptEnableMask);
    UNSERIALIZE_SCALAR(cptDelegation);
    UNSERIALIZE_SCALAR(cptSpmpEntries);
    if (cptSpmpEntries != getCapacity()) {
        fatal("Current SPMP table size (%d) differs from checkpoint SPMP "
              "table size (%d).", getCapacity(), cptSpmpEntries);
    }
    fatal_if((cptEnableMask != 0) != enableMask ||
             (cptDelegation != 0) != delegation,
             "SPMP checkpoint extension profile is incompatible");

    if (!delegation) {
        UNSERIALIZE_SCALAR(spmpEnable);
        for (int i = 0; i < spmpEntries; ++i)
            spmpTable[i].unserializeSection(cp, csprintf("Entry%d", i));
        for (uint32_t i = 0; i < static_cast<uint32_t>(spmpEntries); ++i)
            spmpUpdateRule(i);
    }
}

} // namespace RiscvISA
} // namespace gem5
