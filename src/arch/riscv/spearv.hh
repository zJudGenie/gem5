/*
 * Copyright (c) 2026 SPEAR-V Port Contributors
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

#ifndef __ARCH_RISCV_SPEARV_HH__
#define __ARCH_RISCV_SPEARV_HH__

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "arch/riscv/page_size.hh"
#include "base/bitfield.hh"
#include "base/types.hh"

namespace gem5
{

namespace RiscvISA
{

enum TagMode : uint8_t
{
    MTAG0 = 0,
    MTAG32 = 1,
    MTAG64 = 2,
    MTAG128 = 3,
};

enum PageType : uint8_t
{
    PT_NORMAL = 0,
    PT_ENCLAVE = 1,
    PT_MONITOR = 2,
    PT_SHARED = 3,
    PT_PAGETABLE = 4,
    PT_FUTURE2 = 5,
    PT_FUTURE3 = 6,
    PT_FUTURE4 = 7,
};

enum PageLevel : uint8_t
{
    PL_KILO = 0,
    PL_MEGA = 1,
    PL_GIGA = 2,
    PL_INVALID = 3,
};

enum EnclaveAccessMode : uint8_t
{
    EAM_NORMAL = 0,
    EAM_SHM = 1,
    EAM_ENCLAVE = 2,
    EAM_FUTURE2 = 3,
};

enum TagControlBits : uint64_t
{
    TAG_CTRL_DISABLE_TAG_COMPARE_CHECKS = 1ULL << 0,
    TAG_CTRL_DISABLE_IMM_PT_CHECKS = 1ULL << 1,
    TAG_CTRL_NC_PTW_IN_E = 1ULL << 2,
    TAG_CTRL_DISABLE_IMM_WRITE_CHECK = 1ULL << 3,
};

constexpr uint64_t TAG_CTRL_MASK = 0xFULL;
constexpr uint16_t EID_INVALID = 0xFFFFU;

/**
 * 128-bit SPEAR-V page-granular memory tag.
 *
 * Bit layout (matching riscv-pk/machine/tag_api.h & cva6/include/riscv_pkg.sv):
 *   lo[15:0]   : id (16-bit EID)
 *   lo[18:16]  : page_type (PageType)
 *   lo[19]     : hpce (Huge Page Containing Enclave)
 *   lo[21:20]  : page_level (PageLevel)
 *   lo[22]     : validated
 *   lo[23]     : immutable
 *   lo[59:24]  : vpn (36-bit VPN, active in MTAG64/MTAG128)
 *   lo[63:60]  : pte_perms ({u,x,w,r} = (pte >> 1) & 0xf, active in MTAG64/128)
 *   hi[9:0]    : mpk (bits [73:64], 10-bit MPK, active in MTAG128)
 *   hi[63:10]  : unused (bits [127:74])
 */
struct MemoryTag
{
    uint64_t lo = 0;
    uint64_t hi = 0;

    constexpr MemoryTag() = default;
    constexpr MemoryTag(uint64_t _lo, uint64_t _hi) : lo(_lo), hi(_hi) {}

    static constexpr MemoryTag
    fromU32(uint32_t word0)
    {
        return MemoryTag(static_cast<uint64_t>(word0), 0ULL);
    }

    static constexpr MemoryTag
    fromU64(uint64_t word0)
    {
        return MemoryTag(word0, 0ULL);
    }

    static constexpr MemoryTag
    fromU128(uint64_t word0, uint64_t word1)
    {
        return MemoryTag(word0, word1);
    }

    constexpr void
    clear()
    {
        lo = 0;
        hi = 0;
    }

    constexpr void
    maskForMode(TagMode mode)
    {
        switch (mode) {
          case MTAG32:
            lo = bits(lo, 31, 0);
            hi = 0;
            break;
          case MTAG64:
            hi = 0;
            break;
          case MTAG128:
            break;
          case MTAG0:
          default:
            lo = 0;
            hi = 0;
            break;
        }
    }

    constexpr uint16_t id() const
    {
        return static_cast<uint16_t>(bits(lo, 15, 0));
    }
    constexpr void setId(uint16_t val)
    {
        lo = insertBits(lo, 15, 0, static_cast<uint64_t>(val));
    }

    constexpr PageType pageType() const
    {
        return static_cast<PageType>(bits(lo, 18, 16));
    }
    constexpr void setPageType(PageType val)
    {
        lo = insertBits(lo, 18, 16, static_cast<uint64_t>(val));
    }

    constexpr bool hpce() const
    {
        return static_cast<bool>(bits(lo, 19));
    }
    constexpr void setHpce(bool val)
    {
        lo = insertBits(lo, 19, static_cast<uint64_t>(val ? 1 : 0));
    }

    constexpr PageLevel pageLevel() const
    {
        return static_cast<PageLevel>(bits(lo, 21, 20));
    }
    constexpr void setPageLevel(PageLevel val)
    {
        lo = insertBits(lo, 21, 20, static_cast<uint64_t>(val));
    }

    constexpr bool validated() const
    {
        return static_cast<bool>(bits(lo, 22));
    }
    constexpr void setValidated(bool val)
    {
        lo = insertBits(lo, 22, static_cast<uint64_t>(val ? 1 : 0));
    }

    constexpr bool immutable() const
    {
        return static_cast<bool>(bits(lo, 23));
    }
    constexpr void setImmutable(bool val)
    {
        lo = insertBits(lo, 23, static_cast<uint64_t>(val ? 1 : 0));
    }

    constexpr uint64_t vpn() const
    {
        return bits(lo, 59, 24);
    }
    constexpr void setVpn(uint64_t val)
    {
        lo = insertBits(lo, 59, 24, val);
    }

    constexpr uint8_t ptePerms() const
    {
        return static_cast<uint8_t>(bits(lo, 63, 60));
    }
    constexpr void setPtePerms(uint8_t val)
    {
        lo = insertBits(lo, 63, 60, static_cast<uint64_t>(val));
    }

    constexpr uint16_t mpk() const
    {
        return static_cast<uint16_t>(bits(hi, 9, 0));
    }
    constexpr void setMpk(uint16_t val)
    {
        hi = insertBits(hi, 9, 0, static_cast<uint64_t>(val));
    }

    constexpr uint64_t unused() const
    {
        return bits(hi, 63, 10);
    }
    constexpr void setUnused(uint64_t val)
    {
        hi = insertBits(hi, 63, 10, val);
    }

    constexpr bool
    operator==(const MemoryTag &other) const
    {
        return lo == other.lo && hi == other.hi;
    }

    constexpr bool
    operator!=(const MemoryTag &other) const
    {
        return !(*this == other);
    }
};

struct MpKeySlot
{
    uint16_t mpk = 0;
    bool wd = false;
};

/**
 * 4x 11-bit MPK configuration in CSR_U_MPKEY_CONFIG (0x069).
 * Matches CVA6 riscv_pkg.sv:342-351:
 *   slot 0: mpk[9:0],   wd[10]
 *   slot 1: mpk[20:11], wd[21]
 *   slot 2: mpk[31:22], wd[32]
 *   slot 3: mpk[42:33], wd[43]
 */
struct MpKeyConfig
{
    static constexpr unsigned NumSlots = 4;
    static constexpr unsigned SlotBits = 11;
    static constexpr uint64_t ValidMask = mask(NumSlots * SlotBits);

    uint64_t raw = 0;

    constexpr MpKeyConfig() = default;
    constexpr explicit MpKeyConfig(uint64_t val) : raw(val) {}

    constexpr MpKeySlot
    slot(unsigned idx) const
    {
        assert(idx < NumSlots);
        const unsigned base = idx * SlotBits;
        return MpKeySlot{
            static_cast<uint16_t>(bits(raw, base + 9, base)),
            static_cast<bool>(bits(raw, base + 10))
        };
    }

    constexpr void
    setSlot(unsigned idx, uint16_t mpk, bool wd)
    {
        assert(idx < NumSlots);
        const unsigned base = idx * SlotBits;
        raw = insertBits(raw, base + 9, base, static_cast<uint64_t>(mpk));
        raw = insertBits(raw, base + 10, static_cast<uint64_t>(wd ? 1 : 0));
    }

    constexpr bool
    matches(uint16_t tag_mpk, bool is_write) const
    {
        for (unsigned i = 0; i < NumSlots; ++i) {
            const MpKeySlot s = slot(i);
            if (tag_mpk == s.mpk && (!is_write || !s.wd)) {
                return true;
            }
        }
        return false;
    }
};

constexpr size_t
tagBytesForMode(TagMode mode)
{
    switch (mode) {
      case MTAG32:
        return 4;
      case MTAG64:
        return 8;
      case MTAG128:
        return 16;
      case MTAG0:
      default:
        return 0;
    }
}

constexpr uint8_t
pteToTagPerms(uint64_t pte)
{
    return static_cast<uint8_t>(bits(pte, 4, 1));
}

constexpr Addr
ppnToTagPaddr(uint64_t ppn, Addr tagStorageBase, uint64_t tagDramBase,
              TagMode mode, bool highWord = false)
{
    if (mode == MTAG0 || mode > MTAG128 || tagStorageBase == 0 ||
        ppn < tagDramBase) {
        return 0;
    }
    const unsigned shift = static_cast<unsigned>(mode) + 1;
    const Addr offset = (static_cast<Addr>(ppn - tagDramBase) << shift) +
                        ((mode == MTAG128 && highWord) ? 8ULL : 0ULL);
    return tagStorageBase + offset;
}

constexpr uint64_t
tagStorageEndPpn(Addr tagStorageBase, uint64_t tagDramBase,
                 TagMode mode, Addr memEndPaddr)
{
    if (mode == MTAG0 || mode > MTAG128 || tagStorageBase == 0 ||
        memEndPaddr == 0) {
        return 0;
    }
    const uint64_t memEndPpn =
        (memEndPaddr >> PageShift) + ((memEndPaddr & mask(PageShift)) ? 1 : 0);
    if (memEndPpn <= tagDramBase) {
        return 0;
    }
    const unsigned shift = static_cast<unsigned>(mode) + 1;
    const uint64_t numPages = memEndPpn - tagDramBase;
    if (numPages > (MaxAddr >> shift)) {
        return memEndPpn;
    }
    const Addr tagStorageBytes = static_cast<Addr>(numPages) << shift;
    if (tagStorageBase > MaxAddr - tagStorageBytes) {
        return memEndPpn;
    }
    const Addr tagEndPaddr = tagStorageBase + tagStorageBytes;
    const uint64_t tagEndPpn =
        (tagEndPaddr >> PageShift) + ((tagEndPaddr & mask(PageShift)) ? 1 : 0);
    return std::max(memEndPpn, tagEndPpn);
}

constexpr bool
ppnHasTag(uint64_t ppn, Addr tagStorageBase, uint64_t tagDramBase,
          TagMode mode, Addr memEndPaddr)
{
    if (mode == MTAG0 || mode > MTAG128 || tagStorageBase == 0 ||
        ppn < tagDramBase) {
        return false;
    }
    const uint64_t endPpn =
        tagStorageEndPpn(tagStorageBase, tagDramBase, mode, memEndPaddr);
    return ppn < endPpn;
}

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_SPEARV_HH__
