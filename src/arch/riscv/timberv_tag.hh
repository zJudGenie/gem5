/*
 * Timber-V Tag Memory for gem5 RISC-V port
 *
 * Implements tag storage for the Timber-V TEE extension.
 * Tags are 2-bit values per-word (granularity = 8 bytes for RV64).
 *
 * Tag values:
 *   T_NORMAL   = 0  - Normal (untrusted) data
 *   T_CALLABLE = 1  - Callable enclave entry point
 *   T_UTRUSTED = 2  - User-trusted enclave data
 *   T_STRUSTED = 3  - Supervisor-trusted enclave data
 *
 * Access matrix (row = mode, col = tag at address):
 *   mode 0 (UN): N=RWX, C=X,    UT=---,  ST=---
 *   mode 1 (SN): N=RWX, C=X,    UT=---,  ST=---
 *   mode 2 (UT): N=RWX, C=RX,   UT=RWX,  ST=---
 *   mode 3 (ST): N=RWX, C=RWX,  UT=RW,   ST=RWX
 */

#ifndef __ARCH_RISCV_TIMBERV_TAG_HH__
#define __ARCH_RISCV_TIMBERV_TAG_HH__

#include <cstdint>
#include <unordered_map>

#include "base/types.hh"

namespace gem5
{
namespace RiscvISA
{

// Tag type values (2-bit)
enum TimberVTagType : uint64_t
{
    TIMBV_T_NORMAL   = 0,
    TIMBV_T_CALLABLE = 1,
    TIMBV_T_UTRUSTED = 2,
    TIMBV_T_STRUSTED = 3,
};

// Access types for tag check
enum TimberVAccessType : int
{
    TIMBV_LOAD  = 0,
    TIMBV_STORE = 1,
    TIMBV_FETCH = 2,
};

// Tag granularity: one 2-bit tag per 64-bit word (8 bytes)
static constexpr Addr TIMBV_TAG_GRANULARITY = 8;

/**
 * Global Timber-V Tag Memory storage.
 *
 * Implements a sparse map from aligned physical addresses to 2-bit tags.
 * Uninitialized memory defaults to T_NORMAL (0).
 *
 * This singleton is accessible from instruction execution code via
 * timberVTags().
 */
class TimberVTagMemory
{
  public:
    static TimberVTagMemory &instance()
    {
        static TimberVTagMemory inst;
        return inst;
    }

    /**
     * Read the 2-bit tag stored at a physical address.
     * Addresses are aligned to TIMBV_TAG_GRANULARITY.
     * Returns TIMBV_T_NORMAL if no tag has been set.
     */
    uint64_t loadTag(Addr addr) const
    {
        addr = alignAddr(addr);
        auto it = tagMap.find(addr);
        if (it != tagMap.end())
            return it->second & 0x3;
        return TIMBV_T_NORMAL;
    }

    /**
     * Write a 2-bit tag at a physical address.
     */
    void storeTag(Addr addr, uint64_t tag)
    {
        addr = alignAddr(addr);
        tagMap[addr] = tag & 0x3;
    }

    /**
     * Check whether an access is permitted by the tag-based access matrix.
     *
     * @param addr      Physical address of the access
     * @param prv       Privilege mode (0=U, 1=S, 3=M)
     * @param secLevel  Security level (0=normal, 1=secure)
     * @param accType   0=load, 1=store, 2=fetch
     * @return true if the access is permitted
     *
     * Machine mode (prv==3) always returns true (bypasses tag checks).
     */
    bool checkAccess(Addr addr, uint8_t prv, uint8_t secLevel,
                     int accType) const
    {
        // Machine mode bypasses all tag checks
        if (prv == 3 /* PRV_M */)
            return true;

        // mode = (prv==PRV_S) | (secLevel<<1)
        uint8_t mode = (prv == 1 ? 1 : 0) | (secLevel << 1);
        uint64_t tag = loadTag(addr);

        // access_matrix[mode][tag]: bits are R=1(load),W=2(store),X=4(fetch)
        static const uint8_t access_matrix[4][4] = {
         // from\to  NORMAL  CALLABLE  UTRUSTED  STRUSTED
         /* UN */  { 0x7,    0x4,      0x0,      0x0 },
         /* SN */  { 0x7,    0x4,      0x0,      0x0 },
         /* UT */  { 0x7,    0x5,      0x7,      0x0 },
         /* ST */  { 0x7,    0x7,      0x3,      0x7 },
        };

        if (mode > 3 || tag > 3)
            return false;

        return (access_matrix[mode][tag] & (1 << accType)) != 0;
    }

    void reset() { tagMap.clear(); }

  private:
    TimberVTagMemory() = default;
    TimberVTagMemory(const TimberVTagMemory &) = delete;
    TimberVTagMemory &operator=(const TimberVTagMemory &) = delete;

    static Addr alignAddr(Addr addr)
    {
        return (addr / TIMBV_TAG_GRANULARITY) * TIMBV_TAG_GRANULARITY;
    }

    std::unordered_map<Addr, uint64_t> tagMap;
};

inline TimberVTagMemory &timberVTags()
{
    return TimberVTagMemory::instance();
}

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_TIMBERV_TAG_HH__
