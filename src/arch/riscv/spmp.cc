#include "arch/riscv/spmp.hh"

#include "arch/riscv/faults.hh"
#include "arch/riscv/pagetable.hh"
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
      addrMask(params.riscv_type == RV64
          ? (Addr{1} << 54) - 1 : (Addr{1} << 32) - 1),
      numRules(0)
{
    fatal_if(spmpEntries < 0 || spmpEntries > 64,
             "SPMP supports between zero and 64 entries, got %d",
             spmpEntries);
    spmpTable.resize(spmpEntries);
}

/** Check one physical access against the first overlapping SPMP entry. */
Fault
SPMP::spmpCheck(const RequestPtr &req, BaseMMU::Mode mode,
                PrivilegeMode pmode, ThreadContext *tc, Addr vaddr)
{
    if (pmode == PrivilegeMode::PRV_M || spmpEntries == 0)
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
    for (uint32_t i = 0; i < spmpEntries; ++i) {
        const SpmpEntry &entry = spmpTable[i];
        const uint8_t a = spmpGetAField(entry.spmpCfg);
        const AddrRange &range = entry.spmpAddr;
        if (a == SPMP_OFF || !range.valid() || range.start() >= range.end())
            continue;

        if (paddr >= range.end() || last < range.start())
            continue;

        if (paddr < range.start() || last >= range.end())
            return createAddrfault(faultVaddr, mode);

        const uint64_t cfg = entry.spmpCfg;
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
    if (pmode == PrivilegeMode::PRV_M || spmpEntries == 0)
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
SPMP::spmpDecodeNapot(Addr spmpaddr)
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
SPMP::spmpUpdateCfg(uint32_t spmp_index, uint64_t this_cfg)
{
    if (spmp_index >= static_cast<uint32_t>(spmpEntries)) {
        DPRINTF(SPMP, "Cannot update configuration: index %u exceeds %d entries\n",
                spmp_index, spmpEntries);
        return false;
    }

    SpmpEntry &entry = spmpTable[spmp_index];
    if (entry.spmpCfg & SPMP_LOCK) {
        DPRINTF(SPMP, "Cannot update locked entry %u configuration\n", spmp_index);
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

/** Reset A and L while retaining the remaining configuration and addresses. */
void
SPMP::spmpReset()
{
    for (SpmpEntry &entry : spmpTable)
        entry.spmpCfg &= ~(SPMP_A_MASK | SPMP_LOCK);
    for (uint32_t i = 0; i < static_cast<uint32_t>(spmpEntries); ++i)
        spmpUpdateRule(i);
}

/** Apply the entry and successor TOR locks before storing a masked address. */
bool
SPMP::spmpUpdateAddr(uint32_t spmp_index, Addr this_addr)
{
    if (spmp_index >= static_cast<uint32_t>(spmpEntries)) {
        DPRINTF(SPMP, "Cannot update address: index %u exceeds %d entries\n",
                spmp_index, spmpEntries);
        return false;
    }

    if (spmpTable[spmp_index].spmpCfg & SPMP_LOCK)
        return false;
    const uint32_t next = spmp_index + 1;
    if (next < static_cast<uint32_t>(spmpEntries) &&
        (spmpTable[next].spmpCfg & SPMP_LOCK) &&
        spmpGetAField(spmpTable[next].spmpCfg) == SPMP_TOR) {
        return false;
    }

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
    fatal_if(spmpEntries != old.spmpEntries,
             "SPMP takeover requires matching entry counts (%d != %d)",
             spmpEntries, old.spmpEntries);
    for (uint32_t i = 0; i < static_cast<uint32_t>(spmpEntries); ++i) {
        spmpTable[i].rawAddr = old.spmpTable[i].rawAddr;
        spmpTable[i].spmpCfg = old.spmpTable[i].spmpCfg;
    }
    for (uint32_t i = 0; i < static_cast<uint32_t>(spmpEntries); ++i)
        spmpUpdateRule(i);
}

/** Serialize raw architected state using the established checkpoint keys. */
void
SPMP::serialize(CheckpointOut &cp) const
{
    int cptSpmpEntries = spmpEntries;
    SERIALIZE_SCALAR(cptSpmpEntries);
    for (int i = 0; i < spmpEntries; ++i)
        spmpTable[i].serializeSection(cp, csprintf("Entry%d", i));
}

/** Restore raw state before deriving ranges so TOR sees restored predecessors. */
void
SPMP::unserialize(CheckpointIn &cp)
{
    int cptSpmpEntries;
    UNSERIALIZE_SCALAR(cptSpmpEntries);
    if (cptSpmpEntries != spmpEntries) {
        fatal("Current SPMP table size (%d) differs from checkpoint SPMP table "
              "size (%d).", spmpEntries, cptSpmpEntries);
    }

    for (int i = 0; i < spmpEntries; ++i)
        spmpTable[i].unserializeSection(cp, csprintf("Entry%d", i));
    for (uint32_t i = 0; i < static_cast<uint32_t>(spmpEntries); ++i)
        spmpUpdateRule(i);
}

} // namespace RiscvISA
} // namespace gem5
