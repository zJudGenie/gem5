#ifndef __ARCH_RISCV_SPMP_HH__
#define __ARCH_RISCV_SPMP_HH__

#include <vector>

#include "arch/generic/tlb.hh"
#include "arch/riscv/isa.hh"
#include "base/types.hh"
#include "base/addr_range.hh"
#include "mem/request.hh"
#include "params/SPMP.hh"
#include "sim/serialize.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace RiscvISA
{

/**
* This class helps to implement RISCV's s-mode physical memory
* protection (sPMP) primitive.
*/

class SPMP : public SimObject
{
  public:
    PARAMS(SPMP);
    SPMP(const Params &params);

  private:
    /** maximum number of entries in the sPMP table */
    int spmpEntries;

    /** address mask for 32 and 64 bit support */
    const Addr addrMask;

    // SXLEN address register: 64       54                      0
    // spmpmaddrX:             |--------| |------addr[55:2]-----|
    //                         32                               0
    //                         |----------addr[33:2]------------|
    // SXLEN config register: SXLEN-1      10         9      8       7             5       3        0
    // spmpcfgX:               |--reserved--||-shared-||-user-||-lock-||--reserved--||-addr-||-X W R-|

    /** This enum is used for encoding of address matching mode of
     * spmp address register, which is present in bits 3-4 (A) of
     * spmpcfg register for a spmp entry.
     * SPMP_OFF = null region (spmp disabled)
     * MP_TOR = top of range mode
     * SPMP_NA4 = naturally aligned four byte region
     * SPMP_NAPOT = naturally aligned power of two region, >= 8 bytes
     */
    enum spmpAmatch
    {
        SPMP_OFF = 0,
        SPMP_TOR = 1,
        SPMP_NA4 = 2,
        SPMP_NAPOT = 3
    };

    /** spmpcfg address range read permission mask */
    static constexpr uint64_t SPMP_READ = uint64_t{1} << 0;
    /** spmpcfg address range write permission mask */
    static constexpr uint64_t SPMP_WRITE = uint64_t{1} << 1;
    /** spmpcfg address range execute permission mask */
    static constexpr uint64_t SPMP_EXEC = uint64_t{1} << 2;
    /** spmpcfg A field mask */
    static constexpr uint64_t SPMP_A_MASK = uint64_t{3} << 3;
    /** spmpcfg address range locked mask */
    static constexpr uint64_t SPMP_LOCK = uint64_t{1} << 7;
    /** spmpcfg user mode mask, to select if rule applies to U or S mode */
    static constexpr uint64_t SPMP_USER = uint64_t{1} << 8;
    /** spmpcfg user mode mask, to select if rule is shared between U and S mode */
    static constexpr uint64_t SPMP_SHARED = uint64_t{1} << 9;

    /** variable to keep track of active number of rules any time */
    int numRules;

    /** single spmp entry struct*/
    struct SpmpEntry : public Serializable
    {
        /** addr range corresponding to a single spmp entry */
        AddrRange spmpAddr = AddrRange(0,0);
        /** raw addr in pmpaddr register for a spmp entry */
        Addr rawAddr = 0;
        /** pmpcfg reg value for a spmp entry */
        uint64_t spmpCfg = 0;

        void
        serialize(CheckpointOut &cp) const
        {
            SERIALIZE_SCALAR(rawAddr);
            SERIALIZE_SCALAR(spmpCfg);
        }
        void
        unserialize(CheckpointIn &cp)
        {
            UNSERIALIZE_SCALAR(rawAddr);
            UNSERIALIZE_SCALAR(spmpCfg);
        }
    };

    /** a table of pmp entries */
    std::vector<SpmpEntry> spmpTable;

  public:
      /** Return the number of implemented SPMP entries. */
      uint32_t getNumEntries() const { return spmpEntries; }

      /** Read the canonical address of an implemented entry. */
      Addr getAddr(uint32_t index) const { return spmpTable[index].rawAddr; }

      /** Read the complete canonical configuration of an implemented entry. */
      uint64_t getCfg(uint32_t index) const { return spmpTable[index].spmpCfg; }

      /**
       * spmpCheck checks if a particular memory access
       * is allowed based on the spmp rules.
       * @param req memory request.
       * @param mode mode of request (read, write, execute).
       * @param pmode effective privilege mode of memory (U, S, M).
       * @param tc thread context.
       * @param vaddr fallback fault address for a physical-only request.
       * @return Fault.
       */
      Fault spmpCheck(const RequestPtr &req, BaseMMU::Mode mode,
                    PrivilegeMode pmode, ThreadContext *tc,
                    Addr vaddr = 0);

      /**
       * spmpUpdateCfg updates the spmpcfg for a spmp
       * entry and calls spmpUpdateRule to update the
       * rule of corresponding spmp entry.
       * @param spmp_index pmp entry index.
       * @param this_cfg value to be written to spmpcfg.
       * @returns true if update spmpicfg success
       */
      bool spmpUpdateCfg(uint32_t spmp_index, uint64_t this_cfg);

      /**
       * spmpUpdateAddr updates the spmpaddr for a spmp
       * entry and calls spmpUpdateRule to update the
       * rule of corresponding spmp entry.
       * @param spmp_index pmp entry index.
       * @param this_addr value to be written to spmpaddr.
       * @returns true if update spmpaddri success
       */
      bool spmpUpdateAddr(uint32_t spmp_index, Addr this_addr);

      /**
       * spmpReset reset when reset signal in trigger from
       * CPU.
       */
      void spmpReset();

      /** Preserve architected state across a compatible CPU/MMU switch. */
      void takeOverFrom(const SPMP &old);

  private:
      /**
       * createAddrfault creates an address fault
       * if the spmp checks fail to pass for a given
       * access. This function is used by spmpCheck().
       * given spmp entry depending on the value
       * of spmpaddr and spmpcfg for that entry.
       * @param vaddr virtual address of the access.
       * @param mode mode of access(read, write, execute).
       * @return Fault.
       */
      Fault createAddrfault(Addr vaddr, BaseMMU::Mode mode);

      /**
       * createDefaultFault creates an address fault when numRules = 0
       * @param req memory request.
       * @param mode mode of request (read, write, execute).
       * @param pmode current privilege mode of memory (U, S, M).
       * @param vaddr optional parameter to pass vaddr of original
       * request for which a page table walk is consulted by pmp unit
       * @return Fault.
       */
      Fault createDefaultFault(const RequestPtr &req, BaseMMU::Mode mode,
                               PrivilegeMode pmode, Addr vaddr = 0);

      /**
       * spmpUpdateRule updates the spmp rule for a
       * given spmp entry depending on the value
       * of spmpaddr and spmpcfg for that entry.
       * @param spmp_index spmp entry index.
       */
      void spmpUpdateRule(uint32_t spmp_index);

      /**
       * spmpGetAField extracts the A field (address matching mode)
       * from an input spmpcfg register
       * @param cfg spmpcfg register value.
       * @return The A field.
       */
      inline uint8_t spmpGetAField(uint64_t cfg);

      /**
       * This function decodes a spmpaddr register value
       * into an address range when A field of spmpcfg
       * register is set to NAPOT mode (naturally aligned
       * power of two region).
       * @param spmpaddr input address from a spmp entry.
       * @return an address range.
       */
      inline AddrRange spmpDecodeNapot(Addr spmpaddr);

      void serialize(CheckpointOut &cp) const override;
      void unserialize(CheckpointIn &cp) override;
};

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_SPMP_HH__
