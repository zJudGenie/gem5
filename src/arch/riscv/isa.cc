/*
 * Copyright (c) 2016 RISC-V Foundation
 * Copyright (c) 2016 The University of Virginia
 * Copyright (c) 2020 Barkhausen Institut
 * Copyright (c) 2022 Google LLC
 * Copyright (c) 2024 University of Rostock
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

#include "arch/riscv/isa.hh"

#include <ctime>
#include <set>
#include <sstream>

#include "arch/riscv/faults.hh"
#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/interrupts.hh"
#include "arch/riscv/mmu.hh"
#include "arch/riscv/pagetable.hh"
#include "arch/riscv/pcstate.hh"
#include "arch/riscv/pmp.hh"
#include "arch/riscv/spmp.hh"
#include "arch/riscv/timberv.hh"
#include "arch/riscv/regs/float.hh"
#include "arch/riscv/regs/int.hh"
#include "arch/riscv/regs/misc.hh"
#include "arch/riscv/regs/vector.hh"
#include "arch/riscv/system.hh"
#include "base/bitfield.hh"
#include "base/compiler.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/base.hh"
#include "debug/Checkpoint.hh"
#include "debug/LLSC.hh"
#include "debug/MatRegs.hh"
#include "debug/RiscvMisc.hh"
#include "debug/VecRegs.hh"
#include "dev/riscv/clint.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "params/RiscvISA.hh"
#include "sim/pseudo_inst.hh"

namespace gem5
{

namespace RiscvISA
{

[[maybe_unused]]
const std::array<const char *, NUM_MISCREGS> MiscRegNames = {{
    [MISCREG_PRV]           = "PRV",
    [MISCREG_ISA]           = "ISA",
    [MISCREG_VENDORID]      = "VENDORID",
    [MISCREG_ARCHID]        = "ARCHID",
    [MISCREG_IMPID]         = "IMPID",
    [MISCREG_HARTID]        = "HARTID",
    [MISCREG_STATUS]        = "STATUS",
    [MISCREG_IP]            = "IP",
    [MISCREG_IE]            = "IE",
    [MISCREG_CYCLE]         = "CYCLE",
    [MISCREG_TIME]          = "TIME",
    [MISCREG_INSTRET]       = "INSTRET",
    [MISCREG_HPMCOUNTER03]  = "HPMCOUNTER03",
    [MISCREG_HPMCOUNTER04]  = "HPMCOUNTER04",
    [MISCREG_HPMCOUNTER05]  = "HPMCOUNTER05",
    [MISCREG_HPMCOUNTER06]  = "HPMCOUNTER06",
    [MISCREG_HPMCOUNTER07]  = "HPMCOUNTER07",
    [MISCREG_HPMCOUNTER08]  = "HPMCOUNTER08",
    [MISCREG_HPMCOUNTER09]  = "HPMCOUNTER09",
    [MISCREG_HPMCOUNTER10]  = "HPMCOUNTER10",
    [MISCREG_HPMCOUNTER11]  = "HPMCOUNTER11",
    [MISCREG_HPMCOUNTER12]  = "HPMCOUNTER12",
    [MISCREG_HPMCOUNTER13]  = "HPMCOUNTER13",
    [MISCREG_HPMCOUNTER14]  = "HPMCOUNTER14",
    [MISCREG_HPMCOUNTER15]  = "HPMCOUNTER15",
    [MISCREG_HPMCOUNTER16]  = "HPMCOUNTER16",
    [MISCREG_HPMCOUNTER17]  = "HPMCOUNTER17",
    [MISCREG_HPMCOUNTER18]  = "HPMCOUNTER18",
    [MISCREG_HPMCOUNTER19]  = "HPMCOUNTER19",
    [MISCREG_HPMCOUNTER20]  = "HPMCOUNTER20",
    [MISCREG_HPMCOUNTER21]  = "HPMCOUNTER21",
    [MISCREG_HPMCOUNTER22]  = "HPMCOUNTER22",
    [MISCREG_HPMCOUNTER23]  = "HPMCOUNTER23",
    [MISCREG_HPMCOUNTER24]  = "HPMCOUNTER24",
    [MISCREG_HPMCOUNTER25]  = "HPMCOUNTER25",
    [MISCREG_HPMCOUNTER26]  = "HPMCOUNTER26",
    [MISCREG_HPMCOUNTER27]  = "HPMCOUNTER27",
    [MISCREG_HPMCOUNTER28]  = "HPMCOUNTER28",
    [MISCREG_HPMCOUNTER29]  = "HPMCOUNTER29",
    [MISCREG_HPMCOUNTER30]  = "HPMCOUNTER30",
    [MISCREG_HPMCOUNTER31]  = "HPMCOUNTER31",
    [MISCREG_HPMEVENT03]    = "HPMEVENT03",
    [MISCREG_HPMEVENT04]    = "HPMEVENT04",
    [MISCREG_HPMEVENT05]    = "HPMEVENT05",
    [MISCREG_HPMEVENT06]    = "HPMEVENT06",
    [MISCREG_HPMEVENT07]    = "HPMEVENT07",
    [MISCREG_HPMEVENT08]    = "HPMEVENT08",
    [MISCREG_HPMEVENT09]    = "HPMEVENT09",
    [MISCREG_HPMEVENT10]    = "HPMEVENT10",
    [MISCREG_HPMEVENT11]    = "HPMEVENT11",
    [MISCREG_HPMEVENT12]    = "HPMEVENT12",
    [MISCREG_HPMEVENT13]    = "HPMEVENT13",
    [MISCREG_HPMEVENT14]    = "HPMEVENT14",
    [MISCREG_HPMEVENT15]    = "HPMEVENT15",
    [MISCREG_HPMEVENT16]    = "HPMEVENT16",
    [MISCREG_HPMEVENT17]    = "HPMEVENT17",
    [MISCREG_HPMEVENT18]    = "HPMEVENT18",
    [MISCREG_HPMEVENT19]    = "HPMEVENT19",
    [MISCREG_HPMEVENT20]    = "HPMEVENT20",
    [MISCREG_HPMEVENT21]    = "HPMEVENT21",
    [MISCREG_HPMEVENT22]    = "HPMEVENT22",
    [MISCREG_HPMEVENT23]    = "HPMEVENT23",
    [MISCREG_HPMEVENT24]    = "HPMEVENT24",
    [MISCREG_HPMEVENT25]    = "HPMEVENT25",
    [MISCREG_HPMEVENT26]    = "HPMEVENT26",
    [MISCREG_HPMEVENT27]    = "HPMEVENT27",
    [MISCREG_HPMEVENT28]    = "HPMEVENT28",
    [MISCREG_HPMEVENT29]    = "HPMEVENT29",
    [MISCREG_HPMEVENT30]    = "HPMEVENT30",
    [MISCREG_HPMEVENT31]    = "HPMEVENT31",
    [MISCREG_TSELECT]       = "TSELECT",
    [MISCREG_TDATA1]        = "TDATA1",
    [MISCREG_TDATA2]        = "TDATA2",
    [MISCREG_TDATA3]        = "TDATA3",
    [MISCREG_DCSR]          = "DCSR",
    [MISCREG_DPC]           = "DPC",
    [MISCREG_DSCRATCH]      = "DSCRATCH",

    [MISCREG_MEDELEG]       = "MEDELEG",
    [MISCREG_MIDELEG]       = "MIDELEG",
    [MISCREG_MTVEC]         = "MTVEC",
    [MISCREG_MCOUNTEREN]    = "MCOUNTEREN",
    [MISCREG_MSCRATCH]      = "MSCRATCH",
    [MISCREG_MEPC]          = "MEPC",
    [MISCREG_MCAUSE]        = "MCAUSE",
    [MISCREG_MTVAL]         = "MTVAL",
    [MISCREG_PMPCFG0]        = "PMPCFG0",
    [MISCREG_PMPCFG1]        = "PMPCFG1",
    [MISCREG_PMPCFG2]        = "PMPCFG2",
    [MISCREG_PMPCFG3]        = "PMPCFG3",
    [MISCREG_PMPCFG4]        = "PMPCFG4",
    [MISCREG_PMPCFG5]        = "PMPCFG5",
    [MISCREG_PMPCFG6]        = "PMPCFG6",
    [MISCREG_PMPCFG7]        = "PMPCFG7",
    [MISCREG_PMPCFG8]        = "PMPCFG8",
    [MISCREG_PMPCFG9]        = "PMPCFG9",
    [MISCREG_PMPCFG10]        = "PMPCFG10",
    [MISCREG_PMPCFG11]        = "PMPCFG11",
    [MISCREG_PMPCFG12]        = "PMPCFG12",
    [MISCREG_PMPCFG13]        = "PMPCFG13",
    [MISCREG_PMPCFG14]        = "PMPCFG14",
    [MISCREG_PMPCFG15]        = "PMPCFG15",
    [MISCREG_PMPADDR00]      = "PMPADDR00",
    [MISCREG_PMPADDR01]      = "PMPADDR01",
    [MISCREG_PMPADDR02]      = "PMPADDR02",
    [MISCREG_PMPADDR03]      = "PMPADDR03",
    [MISCREG_PMPADDR04]      = "PMPADDR04",
    [MISCREG_PMPADDR05]      = "PMPADDR05",
    [MISCREG_PMPADDR06]      = "PMPADDR06",
    [MISCREG_PMPADDR07]      = "PMPADDR07",
    [MISCREG_PMPADDR08]      = "PMPADDR08",
    [MISCREG_PMPADDR09]      = "PMPADDR09",
    [MISCREG_PMPADDR10]      = "PMPADDR10",
    [MISCREG_PMPADDR11]      = "PMPADDR11",
    [MISCREG_PMPADDR12]      = "PMPADDR12",
    [MISCREG_PMPADDR13]      = "PMPADDR13",
    [MISCREG_PMPADDR14]      = "PMPADDR14",
    [MISCREG_PMPADDR15]      = "PMPADDR15",
    [MISCREG_PMPADDR16]      = "PMPADDR16",
    [MISCREG_PMPADDR17]      = "PMPADDR17",
    [MISCREG_PMPADDR18]      = "PMPADDR18",
    [MISCREG_PMPADDR19]      = "PMPADDR19",
    [MISCREG_PMPADDR20]      = "PMPADDR20",
    [MISCREG_PMPADDR21]      = "PMPADDR21",
    [MISCREG_PMPADDR22]      = "PMPADDR22",
    [MISCREG_PMPADDR23]      = "PMPADDR23",
    [MISCREG_PMPADDR24]      = "PMPADDR24",
    [MISCREG_PMPADDR25]      = "PMPADDR25",
    [MISCREG_PMPADDR26]      = "PMPADDR26",
    [MISCREG_PMPADDR27]      = "PMPADDR27",
    [MISCREG_PMPADDR28]      = "PMPADDR28",
    [MISCREG_PMPADDR29]      = "PMPADDR29",
    [MISCREG_PMPADDR30]      = "PMPADDR30",
    [MISCREG_PMPADDR31]      = "PMPADDR31",
    [MISCREG_PMPADDR32]      = "PMPADDR32",
    [MISCREG_PMPADDR33]      = "PMPADDR33",
    [MISCREG_PMPADDR34]      = "PMPADDR34",
    [MISCREG_PMPADDR35]      = "PMPADDR35",
    [MISCREG_PMPADDR36]      = "PMPADDR36",
    [MISCREG_PMPADDR37]      = "PMPADDR37",
    [MISCREG_PMPADDR38]      = "PMPADDR38",
    [MISCREG_PMPADDR39]      = "PMPADDR39",
    [MISCREG_PMPADDR40]      = "PMPADDR40",
    [MISCREG_PMPADDR41]      = "PMPADDR41",
    [MISCREG_PMPADDR42]      = "PMPADDR42",
    [MISCREG_PMPADDR43]      = "PMPADDR43",
    [MISCREG_PMPADDR44]      = "PMPADDR44",
    [MISCREG_PMPADDR45]      = "PMPADDR45",
    [MISCREG_PMPADDR46]      = "PMPADDR46",
    [MISCREG_PMPADDR47]      = "PMPADDR47",
    [MISCREG_PMPADDR48]      = "PMPADDR48",
    [MISCREG_PMPADDR49]      = "PMPADDR49",
    [MISCREG_PMPADDR50]      = "PMPADDR50",
    [MISCREG_PMPADDR51]      = "PMPADDR51",
    [MISCREG_PMPADDR52]      = "PMPADDR52",
    [MISCREG_PMPADDR53]      = "PMPADDR53",
    [MISCREG_PMPADDR54]      = "PMPADDR54",
    [MISCREG_PMPADDR55]      = "PMPADDR55",
    [MISCREG_PMPADDR56]      = "PMPADDR56",
    [MISCREG_PMPADDR57]      = "PMPADDR57",
    [MISCREG_PMPADDR58]      = "PMPADDR58",
    [MISCREG_PMPADDR59]      = "PMPADDR59",
    [MISCREG_PMPADDR60]      = "PMPADDR60",
    [MISCREG_PMPADDR61]      = "PMPADDR61",
    [MISCREG_PMPADDR62]      = "PMPADDR62",
    [MISCREG_PMPADDR63]      = "PMPADDR63",
    [MISCREG_RESERVED01]    = "",
    [MISCREG_RESERVED02]    = "",
    [MISCREG_STVEC]         = "STVEC",
    [MISCREG_SCOUNTEREN]    = "SCOUNTEREN",
    [MISCREG_SSCRATCH]      = "SSCRATCH",
    [MISCREG_SEPC]          = "SEPC",
    [MISCREG_SCAUSE]        = "SCAUSE",
    [MISCREG_STVAL]         = "STVAL",
    [MISCREG_SATP]          = "SATP",
    [MISCREG_SENVCFG]       = "SENVCFG",

    [MISCREG_RESERVED03]    = "",
    [MISCREG_RESERVED04]    = "",
    [MISCREG_RESERVED05]    = "",
    [MISCREG_RESERVED06]    = "",
    [MISCREG_RESERVED07]    = "",
    [MISCREG_FFLAGS]        = "FFLAGS",
    [MISCREG_FRM]           = "FRM",
    [MISCREG_TRACE]         = "TRACE",

    [MISCREG_VSTART]        = "VSTART",
    [MISCREG_VXSAT]         = "VXSAT",
    [MISCREG_VXRM]          = "VXRM",
    [MISCREG_VCSR]          = "VCSR",
    [MISCREG_VL]            = "VL",
    [MISCREG_VTYPE]         = "VTYPE",
    [MISCREG_VLENB]         = "VLENB",

    // H-extension (RV64) registers

    [MISCREG_HVIP]          = "HVIP",

    [MISCREG_MTINST]        = "MTINST",
    [MISCREG_MTVAL2]        = "MTVAL2",

    [MISCREG_HSTATUS]       = "HSTATUS",
    [MISCREG_HEDELEG]       = "HEDELEG",
    [MISCREG_HIDELEG]       = "HIDELEG",
    [MISCREG_HCOUNTEREN]    = "HCOUNTEREN",
    [MISCREG_HGEIE]         = "HGEIE",
    [MISCREG_HTVAL]         = "HTVAL",
    [MISCREG_HTINST]        = "HTINST",
    [MISCREG_HGEIP]         = "HGEIP",

    [MISCREG_HENVCFG]       = "HENVCFG",
    [MISCREG_HGATP]         = "HGATP",
    [MISCREG_HCONTEXT]      = "HCONTEXT",
    [MISCREG_HTIMEDELTA]    = "HTIMEDELTA",

    [MISCREG_VSSTATUS]      = "VSSTATUS",
    [MISCREG_VSTVEC]        = "VSTVEC",
    [MISCREG_VSSCRATCH]     = "VSSCRATCH",
    [MISCREG_VSEPC]         = "VSEPC",
    [MISCREG_VSCAUSE]       = "VSCAUSE",
    [MISCREG_VSTVAL]        = "VSTVAL",
    [MISCREG_VSATP]         = "VSATP",
    [MISCREG_VIRT]          = "VIRT",

    // H-extension (RV64) registers end here

    [MISCREG_NMIVEC]        = "NMIVEC",
    [MISCREG_NMIE]          = "NMIE",
    [MISCREG_NMIP]          = "NMIP",
    [MISCREG_MNSCRATCH]     = "MNSCRATCH",
    [MISCREG_MNEPC]         = "MNEPC",
    [MISCREG_MNCAUSE]       = "MNCAUSE",
    [MISCREG_MNSTATUS]      = "MNSTATUS",

    [MISCREG_MTSTATUS]      = "MTSTATUS",
    [MISCREG_STSTATUS]      = "STSTATUS",
    [MISCREG_MTTCB]         = "MTTCB",
    [MISCREG_STTCB]         = "STTCB",
    [MISCREG_STTVEC]        = "STTVEC",
    [MISCREG_STSCRATCH]     = "STSCRATCH",

    // following are rv32 only registers
    [MISCREG_MSTATUSH]      = "MSTATUSH",

    [MISCREG_CYCLEH]         = "CYCLEH",
    [MISCREG_TIMEH]          = "TIMEH",
    [MISCREG_INSTRETH]       = "INSTRETH",
    [MISCREG_HPMCOUNTER03H]  = "HPMCOUNTER03H",
    [MISCREG_HPMCOUNTER04H]  = "HPMCOUNTER04H",
    [MISCREG_HPMCOUNTER05H]  = "HPMCOUNTER05H",
    [MISCREG_HPMCOUNTER06H]  = "HPMCOUNTER06H",
    [MISCREG_HPMCOUNTER07H]  = "HPMCOUNTER07H",
    [MISCREG_HPMCOUNTER08H]  = "HPMCOUNTER08H",
    [MISCREG_HPMCOUNTER09H]  = "HPMCOUNTER09H",
    [MISCREG_HPMCOUNTER10H]  = "HPMCOUNTER10H",
    [MISCREG_HPMCOUNTER11H]  = "HPMCOUNTER11H",
    [MISCREG_HPMCOUNTER12H]  = "HPMCOUNTER12H",
    [MISCREG_HPMCOUNTER13H]  = "HPMCOUNTER13H",
    [MISCREG_HPMCOUNTER14H]  = "HPMCOUNTER14H",
    [MISCREG_HPMCOUNTER15H]  = "HPMCOUNTER15H",
    [MISCREG_HPMCOUNTER16H]  = "HPMCOUNTER16H",
    [MISCREG_HPMCOUNTER17H]  = "HPMCOUNTER17H",
    [MISCREG_HPMCOUNTER18H]  = "HPMCOUNTER18H",
    [MISCREG_HPMCOUNTER19H]  = "HPMCOUNTER19H",
    [MISCREG_HPMCOUNTER20H]  = "HPMCOUNTER20H",
    [MISCREG_HPMCOUNTER21H]  = "HPMCOUNTER21H",
    [MISCREG_HPMCOUNTER22H]  = "HPMCOUNTER22H",
    [MISCREG_HPMCOUNTER23H]  = "HPMCOUNTER23H",
    [MISCREG_HPMCOUNTER24H]  = "HPMCOUNTER24H",
    [MISCREG_HPMCOUNTER25H]  = "HPMCOUNTER25H",
    [MISCREG_HPMCOUNTER26H]  = "HPMCOUNTER26H",
    [MISCREG_HPMCOUNTER27H]  = "HPMCOUNTER27H",
    [MISCREG_HPMCOUNTER28H]  = "HPMCOUNTER28H",
    [MISCREG_HPMCOUNTER29H]  = "HPMCOUNTER29H",
    [MISCREG_HPMCOUNTER30H]  = "HPMCOUNTER30H",
    [MISCREG_HPMCOUNTER31H]  = "HPMCOUNTER31H",

    [MISCREG_JVT] = "JVT",

    [MISCREG_SISELECT]      = "SISELECT",
    [MISCREG_SIREG]         = "SIREG",
    [MISCREG_SIREG2]        = "SIREG2",
    [MISCREG_SIREG3]        = "SIREG3",
    [MISCREG_SIREG4]        = "SIREG4",
    [MISCREG_SIREG5]        = "SIREG5",
    [MISCREG_SIREG6]        = "SIREG6",
    [MISCREG_MISELECT]      = "MISELECT",
    [MISCREG_MIREG]         = "MIREG",
    [MISCREG_MIREG2]        = "MIREG2",
    [MISCREG_MIREG3]        = "MIREG3",
    [MISCREG_MIREG4]        = "MIREG4",
    [MISCREG_MIREG5]        = "MIREG5",
    [MISCREG_MIREG6]        = "MIREG6",
    [MISCREG_SPMPEN]        = "SPMPEN",
    [MISCREG_SPMPENH]       = "SPMPENH",
    [MISCREG_MPMPDELEG]     = "MPMPDELEG",


    [MISCREG_FFLAGS_EXE]    = "FFLAGS_EXE",
}};

namespace
{

/* Not applicable to RISCV */
RegClass vecElemClass(VecElemClass, VecElemClassName, 0, debug::IntRegs);
RegClass vecPredRegClass(VecPredRegClass, VecPredRegClassName, 0,
        debug::IntRegs);
RegClass matRegClass(MatRegClass, MatRegClassName, 0, debug::MatRegs);
RegClass ccRegClass(CCRegClass, CCRegClassName, 0, debug::IntRegs);

} // anonymous namespace

/** Construct the ISA and reject incompatible SPMP extension profiles. */
ISA::ISA(const Params &p) : BaseISA(p, "riscv"),
    _rvType(p.riscv_type), enableRvv(p.enable_rvv), vlen(p.vlen), elen(p.elen),
    _privilegeModeSet(p.privilege_mode_set),
    _wfiResumeOnPending(p.wfi_resume_on_pending), _enableZcd(p.enable_Zcd),
    _enableSmrnmi(p.enable_Smrnmi), _enableSspmp(p.enable_Sspmp),
    _enableSspmpen(p.enable_Sspmpen),
    _enableSmpmpdeleg(p.enable_Smpmpdeleg)
{
    fatal_if(_enableSspmp && _privilegeModeSet != enums::MSU,
             "Sspmp requires the non-virtualized MSU privilege profile");
    fatal_if((_enableSspmpen || _enableSmpmpdeleg) && !_enableSspmp,
             "Sspmpen and Smpmpdeleg require Sspmp");
    fatal_if((_enableSspmpen || _enableSmpmpdeleg) &&
             _privilegeModeSet != enums::MSU,
             "Sspmpen and Smpmpdeleg require the non-virtualized MSU "
             "privilege profile");
    _regClasses.push_back(&intRegClass);
    _regClasses.push_back(&floatRegClass);
    _regClasses.push_back(&vecRegClass);
    _regClasses.push_back(&vecElemClass);
    _regClasses.push_back(&vecPredRegClass);
    _regClasses.push_back(&matRegClass);
    _regClasses.push_back(&ccRegClass);
    _regClasses.push_back(&miscRegClass);

    if (enableRvv) {
        fatal_if(p.vlen < p.elen, "VLEN should be greater or equal",
                 "than ELEN. Ch. 2RISC-V vector spec.");

        inform("RVV enabled, VLEN = %d bits, ELEN = %d bits", p.vlen, p.elen);
    }

    miscRegFile.resize(NUM_PHYS_MISCREGS);
    clear();
}

/** Attach a context only when its SPMP backend matches the hart profile. */
void
ISA::setThreadContext(ThreadContext *_tc)
{
    BaseISA::setThreadContext(_tc);
    auto mmu = static_cast<MMU *>(tc->getMMUPtr());
    const auto spmp = mmu->getSPMP();
    fatal_if(spmp->params().riscv_type != _rvType,
             "SPMP architecture must match the hart's RISC-V type");
    mmu->bindSPMPProfile(_enableSspmp, _rvType, _privilegeModeSet,
                         _enableSspmpen, _enableSmpmpdeleg);
}

bool ISA::inUserMode() const
{
    return miscRegFile[MISCREG_PRV] == PRV_U;
}

/** Copy architectural registers after checking source and destination profiles. */
void
ISA::copyRegsFrom(ThreadContext *src)
{
    // Validate protection compatibility before copying any architectural state.
    auto dstMmu = static_cast<MMU *>(tc->getMMUPtr());
    auto srcMmu = static_cast<MMU *>(src->getMMUPtr());
    dstMmu->validateTakeOverFrom(srcMmu);

    // First loop through the integer registers.
    for (auto &id: intRegClass)
        tc->setReg(id, src->getReg(id));

    // Second loop through the float registers.
    for (auto &id: floatRegClass)
        tc->setReg(id, src->getReg(id));

    // Third loop through the vector registers.
    RiscvISA::VecRegContainer vc;
    for (auto &id: vecRegClass) {
        src->getReg(id, &vc);
        tc->setReg(id, &vc);
    }

    // Copying Misc Regs
    for (int i = 0; i < NUM_PHYS_MISCREGS; i++)
        tc->setMiscRegNoEffect(i, src->readMiscRegNoEffect(i));

    // Lastly copy PC/NPC
    tc->pcState(src->pcState());
}

void ISA::clear()
{
    std::fill(miscRegFile.begin(), miscRegFile.end(), 0);

    miscRegFile[MISCREG_PRV] = PRV_M;
    miscRegFile[MISCREG_VENDORID] = 0;
    miscRegFile[MISCREG_ARCHID] = 0;
    miscRegFile[MISCREG_IMPID] = 0;

    MISA misa = 0;
    STATUS status = 0;

    // default config arch isa string is rv64(32)imafdc
    misa.rvi = misa.rvm = misa.rva = misa.rvf = misa.rvd = misa.rvc = 1;

    switch (getPrivilegeModeSet()) {
        case enums::M:
          break;
        case enums::MU:
          misa.rvu = 1;
          break;
        case enums::MSU:
          misa.rvs = misa.rvu = 1;
          break;
        case enums::MHSU:
          misa.rvh = misa.rvs = misa.rvu = 1;
          inform("RVH enabled.");
          break;
        default:
          panic("Privilege mode set config should not reach here");
    }

    // mark FS is initial
    status.fs = FPUStatus::INITIAL;

    // _rvType dependent init.
    switch (_rvType) {
        case RV32:
          misa.rv32_mxl = 1;
          break;
        case RV64:
          misa.rv64_mxl = 2;
          status.uxl = status.sxl = 2;
          if (misa.rvh) {
              HSTATUS hstatus = 0;
              hstatus.vsxl = 2;
              miscRegFile[MISCREG_HSTATUS] = hstatus;

              STATUS vsstatus = 0;
              vsstatus.uxl = 2;
              vsstatus.fs = FPUStatus::INITIAL;

              if (getEnableRvv())
                vsstatus.vs = VPUStatus::INITIAL;

              miscRegFile[MISCREG_VSSTATUS] = vsstatus;
          }
          break;
        default:
          panic("%s: Unknown _rvType: %d", name(), (int)_rvType);
    }
    if (getEnableRvv()) {
        status.vs = VPUStatus::INITIAL;
        misa.rvv = 1;
    }

    miscRegFile[MISCREG_ISA] = misa;
    miscRegFile[MISCREG_STATUS] = status;
    miscRegFile[MISCREG_MCOUNTEREN] = 0x7;
    miscRegFile[MISCREG_SCOUNTEREN] = 0x7;
    // don't set it to zero; software may try to determine the supported
    // triggers, starting at zero. simply set a different value here.
    miscRegFile[MISCREG_TSELECT] = 1;
    miscRegFile[MISCREG_NMIE] = enableSmrnmi() ? 0 : 1;
}

Fault
ISA::hpmCounterCheck(int misc_reg, ExtMachInst machInst) const
{
    int hpmcounter = 0;
    if (misc_reg >= MISCREG_CYCLEH) {
        hpmcounter = misc_reg - MISCREG_CYCLEH;
    } else {
        hpmcounter = misc_reg - MISCREG_CYCLE;
    }

    if (hpmcounter < 0 || hpmcounter > 31)
        panic("Illegal HPM counter %d\n", hpmcounter);

    PrivilegeMode prv = (PrivilegeMode)readMiscRegNoEffect(MISCREG_PRV);
    MISA misa = readMiscRegNoEffect(MISCREG_ISA);
    RiscvSystem *sys = dynamic_cast<RiscvSystem *>(tc->getSystemPtr());
    bool is_time_csr = misc_reg == MISCREG_TIME || misc_reg == MISCREG_TIMEH;

    // Check if the clint is exists in system
    if (is_time_csr && (sys == nullptr || sys->getClint() == nullptr)) {
        return std::make_shared<IllegalInstFault>("Can't find CLINT in system",
                                                  machInst);
    }

    RegVal mcounteren = miscRegFile[MISCREG_MCOUNTEREN];
    RegVal hcounteren = misa.rvh ? miscRegFile[MISCREG_HCOUNTEREN] : 0;
    RegVal scounteren = misa.rvs ? miscRegFile[MISCREG_SCOUNTEREN] : 0;

    // Paragraph 3.1.11 RISCV Privileged Spec 20211203
    // firstly, respect mcounteren disallowing the counter
    if (prv < PRV_M && (bits(mcounteren, hpmcounter) == 0)) {
        return std::make_shared<IllegalInstFault>(
            csprintf("Counter %s is disabled from mcounteren bit %d\n",
            MiscRegNames[misc_reg], hpmcounter), machInst);
    }

    // then, if h-extension is on and virtualization
    // is enabled, respect hcounteren for VS and VU
    if (misa.rvh && virtualizationEnabled() && prv < PRV_HS &&
        bits(hcounteren, hpmcounter) == 0)
    {
        return std::make_shared<VirtualInstFault>(
            csprintf("Counter %s is disabled from hcounteren bit %d\n",
            MiscRegNames[misc_reg], hpmcounter), machInst);
    }

    // finally, respect scounteren for PRV_U (either with V = 1 or 0)
    if (prv == PRV_U && bits(scounteren, hpmcounter) == 0) {
        return std::make_shared<IllegalInstFault>(
            csprintf("Counter %s is disabled from scounteren bit %d\n",
            MiscRegNames[misc_reg], hpmcounter), machInst);
    }

    return NoFault;
}

/** Read raw misc state, resolving protection registers canonically. */
RegVal
ISA::readMiscRegNoEffect(RegIndex idx) const
{
    // Direct protection registers have one authoritative backend copy.
    switch (idx) {
      case MISCREG_SPMPEN:
        if (!_enableSspmpen)
            return 0;
        {
            const RegVal value = static_cast<MMU *>(tc->getMMUPtr())->
                getSPMP()->getEnable();
            return _rvType == RV32 ? bits(value, 31, 0) : value;
        }
      case MISCREG_SPMPENH:
        if (!_enableSspmpen || _rvType != RV32)
            return 0;
        return bits(static_cast<MMU *>(tc->getMMUPtr())->getSPMP()->
                    getEnable(), 63, 32);
      case MISCREG_MPMPDELEG:
        if (!_enableSmpmpdeleg)
            return 0;
        return static_cast<MMU *>(tc->getMMUPtr())->getPMP()->getPartition();
      case MISCREG_PMPCFG0 ... MISCREG_PMPCFG15:
        {
            auto pmp = static_cast<MMU *>(tc->getMMUPtr())->getPMP();
            const unsigned perReg = _rvType == RV32 ? 4 : 8;
            const unsigned base = (idx - MISCREG_PMPCFG0) * 4;
            RegVal value = 0;
            for (unsigned i = 0; i < perReg; ++i)
                value |= (pmp->getCfg(base + i) & 0xff) << (8 * i);
            return value;
        }
      case MISCREG_PMPADDR00 ... MISCREG_PMPADDR63:
        return static_cast<MMU *>(tc->getMMUPtr())->getPMP()->getAddr(
            idx - MISCREG_PMPADDR00);
      default:
        break;
    }
    // Illegal CSR
    panic_if(idx > NUM_PHYS_MISCREGS, "Illegal CSR index %#x\n", idx);
    DPRINTF(RiscvMisc, "Reading MiscReg %s (%d): %#x.\n",
            MiscRegNames[idx], idx, miscRegFile[idx]);
    return miscRegFile[idx];
}

/** Read architectural misc state, resolving indirect aliases in the backend. */
RegVal
ISA::readMiscReg(RegIndex idx)
{
    switch (idx) {
      case MISCREG_MIREG:
      case MISCREG_MIREG2:
        {
            if (!_enableSmpmpdeleg)
                return 0;
            const RegVal selector = readMiscRegNoEffect(MISCREG_MISELECT);
            if (selector < 0x100 || selector > 0x13f)
                return 0;
            auto spmp = static_cast<MMU *>(tc->getMMUPtr())->getSPMP();
            const uint32_t entry = selector - 0x100;
            const RegVal value = idx == MISCREG_MIREG ?
                spmp->getAddr(entry) : spmp->getCfg(entry);
            return _rvType == RV32 ? bits(value, 31, 0) : value;
        }
      case MISCREG_MIREG3:
      case MISCREG_MIREG4:
      case MISCREG_MIREG5:
      case MISCREG_MIREG6:
        return 0;
      case MISCREG_SIREG:
      case MISCREG_SIREG2:
        {
            if (!_enableSspmp)
                return 0;
            const RegVal selector = readMiscRegNoEffect(MISCREG_SISELECT);
            if (selector < 0x100 || selector > 0x13f)
                return 0;
            const auto spmp = static_cast<MMU *>(tc->getMMUPtr())->getSPMP();
            const uint32_t entry = selector - 0x100;
            if (entry >= spmp->getNumEntries())
                return 0;
            const RegVal val = idx == MISCREG_SIREG ?
                spmp->getAddr(entry) : spmp->getCfg(entry);
            return tc->pcState().as<PCState>().rvType() == RV32 ?
                bits(val, 31, 0) : val;
        }
      case MISCREG_SPMPEN:
      case MISCREG_SPMPENH:
      case MISCREG_MPMPDELEG:
        return readMiscRegNoEffect(idx);
      case MISCREG_PMPCFG0 ... MISCREG_PMPCFG15:
      case MISCREG_PMPADDR00 ... MISCREG_PMPADDR63:
        return readMiscRegNoEffect(idx);
      case MISCREG_SIREG3:
      case MISCREG_SIREG4:
      case MISCREG_SIREG5:
      case MISCREG_SIREG6:
        // Instruction accesses fault in indirectCSRCheck before reaching here.
        return 0;
      case MISCREG_HARTID:
        return tc->contextId();
      case MISCREG_CYCLE:
            return static_cast<RegVal>(tc->getCpuPtr()->curCycle());
      case MISCREG_CYCLEH:
            return bits<RegVal>(tc->getCpuPtr()->curCycle(), 63, 32);
      case MISCREG_TIME: {
          RiscvSystem *sys = dynamic_cast<RiscvSystem *>(tc->getSystemPtr());
          panic_if(!sys, "read MISCREG_TIME not in RiscvSystem");
          return sys->tryReadMtime();
      }
      case MISCREG_TIMEH: {
          RiscvSystem *sys = dynamic_cast<RiscvSystem *>(tc->getSystemPtr());
          panic_if(!sys, "read MISCREG_TIME not in RiscvSystem");
          return bits(sys->tryReadMtime(), 63, 32);
      }
      case MISCREG_INSTRET:
            return static_cast<RegVal>(tc->getCpuPtr()->totalInsts());
      case MISCREG_INSTRETH:
            return bits<RegVal>(tc->getCpuPtr()->totalInsts(), 63, 32);
      case MISCREG_IP:
        {
            auto ic = dynamic_cast<RiscvISA::Interrupts *>(
                    tc->getCpuPtr()->getInterruptController(tc->threadId()));
            return ic->readIP();
        }
      case MISCREG_HVIP:
        {
            auto ic = dynamic_cast<RiscvISA::Interrupts *>(
                    tc->getCpuPtr()->getInterruptController(tc->threadId()));
            return ic->readHVIP();
        }
      case MISCREG_IE:
        {
            auto ic = dynamic_cast<RiscvISA::Interrupts *>(
                    tc->getCpuPtr()->getInterruptController(tc->threadId()));
            return ic->readIE();
        }
      case MISCREG_MIDELEG:
        {
            MISA misa = readMiscRegNoEffect(MISCREG_ISA);
            auto mideleg_val = readMiscRegNoEffect(MISCREG_MIDELEG);

            // Note: old versions of bbl don't like HS_INTERRUPTS on mideleg
            // In those cases, the bootloader panics.
            // You can replace with the line below, however H-extension
            // is incomplete this way.
            // we only check misa to avoid unused var error
            // return misa.rvh ? mideleg_val : mideleg_val;
            return misa.rvh ? mideleg_val | HS_INTERRUPTS : mideleg_val;

        }
      case MISCREG_SEPC:
      case MISCREG_MEPC:
      case MISCREG_MNEPC:
        {
            MISA misa = readMiscRegNoEffect(MISCREG_ISA);
            auto val = readMiscRegNoEffect(idx);
            // if compressed instructions are disabled, epc[1] is set to 0
            if (misa.rvc == 0)
                return mbits(val, 63, 2);
            // epc[0] is always 0
            else
                return mbits(val, 63, 1);
        }
      case MISCREG_HSTATUS:
        {
            auto val = readMiscRegNoEffect(idx);
            // hardwire vsxl
            return val |= (2ULL << VSXL_OFFSET);
        }
      case MISCREG_VSSTATUS:
      case MISCREG_STATUS:
        {
            // Updating the SD bit.
            // . Per RISC-V ISA Manual, vol II, section 3.1.6.6, page 26,
            // the SD bit is a read-only bit indicating whether any of
            // FS, VS, and XS fields being in the respective dirty state.
            // . Per section 3.1.6, page 20, the SD bit is the most
            // significant bit of the MSTATUS CSR for both RV32 and RV64.
            // . Per section 3.1.6.6, page 29, the explicit formula for
            // updating the SD is,
            //   SD = ((FS==DIRTY) | (XS==DIRTY) | (VS==DIRTY))
            // . Ideally, we want to update the SD after every relevant
            // instruction, however, lazily updating the Status register
            // upon its read produces the same effect as well.
            STATUS status = readMiscRegNoEffect(idx);
            uint64_t sd_bit = \
                (status.xs == 3) || (status.fs == 3) || (status.vs == 3);
            // For RV32, the SD bit is at index 31
            // For RV64, the SD bit is at index 63.
            switch (_rvType) {
                case RV32:
                    status.rv32_sd = sd_bit;
                    break;
                case RV64:
                    status.rv64_sd = sd_bit;
                    break;
                default:
                    panic("%s: Unknown _rvType: %d", name(), (int)_rvType);
            }
            // Check status.mpp
            MISA misa = readMiscRegNoEffect(MISCREG_ISA);
            switch(status.mpp) {
                case PRV_U:
                    status.mpp = (misa.rvu) ? PRV_U : PRV_M;
                    break;
                case PRV_S:
                    if (misa.rvs)
                        status.mpp = PRV_S;
                    else
                        status.mpp = (misa.rvu) ? PRV_U : PRV_M;
                    break;
                case PRV_M:
                    break;
                default:
                    status.mpp = (misa.rvu) ? PRV_U : PRV_M;
            }

            setMiscRegNoEffect(idx, status);
            return readMiscRegNoEffect(idx);
        }
      case MISCREG_VLENB:
        {
            return getVecLenInBytes();
        }
      case MISCREG_VTYPE:
        {
            auto rpc = tc->pcState().as<PCState>();
            return rpc.vtype();
        }
      case MISCREG_VL:
        {
            auto rpc = tc->pcState().as<PCState>();
            return (RegVal)rpc.vl();
        }
      case MISCREG_VCSR:
        {
            return readMiscRegNoEffect(MISCREG_VXSAT) |
                  (readMiscRegNoEffect(MISCREG_VXRM) << 1);
        }
        break;

      case MISCREG_MNSTATUS:
        {
            NSTATUS nstatus = readMiscRegNoEffect(idx);
            nstatus.nmie = readMiscRegNoEffect(MISCREG_NMIE);
            // Check nstatus.mnpp
            MISA misa = readMiscRegNoEffect(MISCREG_ISA);
            switch(nstatus.mnpp) {
                case PRV_U:
                    nstatus.mnpp = (misa.rvu) ? PRV_U : PRV_M;
                    break;
                case PRV_S:
                    if (misa.rvs)
                        nstatus.mnpp = PRV_S;
                    else
                        nstatus.mnpp = (misa.rvu) ? PRV_U : PRV_M;
                    break;
                case PRV_M:
                    break;
                default:
                    nstatus.mnpp = (misa.rvu) ? PRV_U : PRV_M;
            }
            return nstatus;
        }

      case MISCREG_MTSTATUS:
      case MISCREG_STSTATUS:
        {
            auto mmu = dynamic_cast<RiscvISA::MMU *>(tc->getMMUPtr());
            if (mmu && mmu->getTimberV()) {
                return mmu->getTimberV()->getMtstatus();
            }
            return readMiscRegNoEffect(idx);
        }

      case MISCREG_MTTCB:
      case MISCREG_STTCB:
        {
            auto mmu = dynamic_cast<RiscvISA::MMU *>(tc->getMMUPtr());
            if (mmu && mmu->getTimberV()) {
                return mmu->getTimberV()->getTtcb();
            }
            return readMiscRegNoEffect(idx);
        }

      default:
        // Try reading HPM counters
        // As a placeholder, all HPM counters are just cycle counters
        if (idx >= MISCREG_HPMCOUNTER03 &&
                idx <= MISCREG_HPMCOUNTER31) {
            return tc->getCpuPtr()->curCycle();

        } else if (idx >= MISCREG_HPMCOUNTER03H &&
                idx <= MISCREG_HPMCOUNTER31H) {
            return bits<RegVal>(tc->getCpuPtr()->curCycle(), 63, 32);
        }
        return readMiscRegNoEffect(idx);
    }
}

void
ISA::setMiscRegNoEffect(RegIndex idx, RegVal val)
{
    // Illegal CSR
    panic_if(idx > NUM_PHYS_MISCREGS, "Illegal CSR index %#x\n", idx);
    DPRINTF(RiscvMisc, "Setting MiscReg %s (%d) to %#x.\n",
            MiscRegNames[idx], idx, val);
    miscRegFile[idx] = val;
}

/** Write architectural misc state, delegating indirect WARL and lock handling. */
void
ISA::setMiscReg(RegIndex idx, RegVal val)
{
    if (idx >= MISCREG_CYCLE && idx <= MISCREG_HPMCOUNTER31) {
        // Ignore writes to HPM counters for now
        warn("Ignoring write to miscreg %s.\n", MiscRegNames[idx]);
    } else {
        switch (idx) {
          case MISCREG_MISELECT:
            setMiscRegNoEffect(idx, bits(val, 11, 0));
            break;
          case MISCREG_MIREG:
          case MISCREG_MIREG2:
            {
                if (!_enableSmpmpdeleg)
                    break;
                const RegVal selector = readMiscRegNoEffect(MISCREG_MISELECT);
                if (selector < 0x100 || selector > 0x13f)
                    break;
                auto spmp = static_cast<MMU *>(tc->getMMUPtr())->getSPMP();
                const uint32_t entry = selector - 0x100;
                if (_rvType == RV32)
                    val = bits(val, 31, 0);
                if (idx == MISCREG_MIREG)
                    spmp->spmpUpdateAddr(entry, val, true);
                else
                    spmp->spmpUpdateCfg(entry, val, true);
            }
            break;
          case MISCREG_MIREG3:
          case MISCREG_MIREG4:
          case MISCREG_MIREG5:
          case MISCREG_MIREG6:
            break;
          case MISCREG_SISELECT:
            setMiscRegNoEffect(idx, bits(val, 11, 0));
            break;
          case MISCREG_SIREG:
          case MISCREG_SIREG2:
            {
                if (!_enableSspmp)
                    break;
                const RegVal selector = readMiscRegNoEffect(MISCREG_SISELECT);
                if (selector < 0x100 || selector > 0x13f)
                    break;
                auto spmp = static_cast<MMU *>(tc->getMMUPtr())->getSPMP();
                const uint32_t entry = selector - 0x100;
                if (entry >= spmp->getNumEntries())
                    break;
                if (tc->pcState().as<PCState>().rvType() == RV32)
                    val = bits(val, 31, 0);
                if (idx == MISCREG_SIREG)
                    spmp->spmpUpdateAddr(entry, val);
                else
                    spmp->spmpUpdateCfg(entry, val);
            }
            break;
          case MISCREG_SIREG3:
          case MISCREG_SIREG4:
          case MISCREG_SIREG5:
          case MISCREG_SIREG6:
            break;

          case MISCREG_SPMPEN:
            if (_enableSspmpen) {
                const uint64_t mask = _rvType == RV32 ? 0xffffffffULL :
                    ~uint64_t{0};
                static_cast<MMU *>(tc->getMMUPtr())->getSPMP()->setEnable(
                    val, mask);
            }
            break;
          case MISCREG_SPMPENH:
            if (_enableSspmpen && _rvType == RV32) {
                static_cast<MMU *>(tc->getMMUPtr())->getSPMP()->setEnable(
                    val << 32, 0xffffffff00000000ULL);
            }
            break;
          case MISCREG_MPMPDELEG:
            if (_enableSmpmpdeleg) {
                auto pmp = static_cast<MMU *>(tc->getMMUPtr())->getPMP();
                pmp->setPartition(val);
            }
            break;

          // From section 3.7.1 of RISCV priv. specs
          // V1.12, the odd-numbered configuration
          // registers are illegal for RV64 and
          // each 64 bit CFG register hold configurations
          // for 8 PMP entries.

          case MISCREG_PMPCFG0 ... MISCREG_PMPCFG15:
            {
                // PMP registers should only be modified in M mode
                assert(readMiscRegNoEffect(MISCREG_PRV) == PRV_M);

                const unsigned entriesPerReg = _rvType == RV32 ? 4 : 8;
                const unsigned base = (idx - MISCREG_PMPCFG0) * 4;
                auto pmp = static_cast<MMU *>(tc->getMMUPtr())->getPMP();
                for (unsigned i = 0; i < entriesPerReg; ++i) {
                    uint8_t cfg_val = (val >> (8*i)) & 0xff;
                    pmp->pmpUpdateCfg(base + i, cfg_val);
                }
            }
            break;
          case MISCREG_PMPADDR00 ... MISCREG_PMPADDR63:
            {
                // PMP registers should only be modified in M mode
                assert(readMiscRegNoEffect(MISCREG_PRV) == PRV_M);

                auto pmp = static_cast<MMU *>(tc->getMMUPtr())->getPMP();
                uint32_t pmp_index = idx-MISCREG_PMPADDR00;
                pmp->pmpUpdateAddr(pmp_index, val);
            } break;
          case MISCREG_STTCB:
            {
                auto mmu = dynamic_cast<RiscvISA::MMU *>(tc->getMMUPtr());
                auto prv = (PrivilegeMode)readMiscRegNoEffect(MISCREG_PRV);
                if (prv == PRV_S && mmu && mmu->getTimberV() && mmu->getTimberV()->getSecLevel() == 0) {
                    warn("Cannot set STTCB from SN mode @ pc %#x\n", tc->pcState().instAddr());
                    return;
                }
                if (mmu && mmu->getTimberV()) {
                    mmu->getTimberV()->updateTtcb(val);
                }
                setMiscRegNoEffect(idx, val);
                setMiscRegNoEffect(MISCREG_MTTCB, val);
            } break;
          case MISCREG_MTTCB:
            {
                auto mmu = dynamic_cast<RiscvISA::MMU *>(tc->getMMUPtr());
                if (mmu && mmu->getTimberV()) {
                    mmu->getTimberV()->updateTtcb(val);
                }
                setMiscRegNoEffect(idx, val);
                setMiscRegNoEffect(MISCREG_STTCB, val);
            } break;
          case MISCREG_STSTATUS:
            {
                auto mmu = dynamic_cast<RiscvISA::MMU *>(tc->getMMUPtr());
                auto prv = (PrivilegeMode)readMiscRegNoEffect(MISCREG_PRV);
                if (prv == PRV_S && mmu && mmu->getTimberV() && mmu->getTimberV()->getSecLevel() == 0) {
                    warn("Cannot set STSTATUS from SN mode @ pc %#x\n", tc->pcState().instAddr());
                    return;
                }
                if (mmu && mmu->getTimberV()) {
                    mmu->getTimberV()->setMtstatus(val);
                    RegVal new_status = mmu->getTimberV()->getMtstatus();
                    setMiscRegNoEffect(idx, new_status);
                    setMiscRegNoEffect(MISCREG_MTSTATUS, new_status);
                } else {
                    setMiscRegNoEffect(idx, val);
                }
            } break;
          case MISCREG_MTSTATUS:
            {
                auto mmu = dynamic_cast<RiscvISA::MMU *>(tc->getMMUPtr());
                if (mmu && mmu->getTimberV()) {
                    mmu->getTimberV()->setMtstatus(val);
                    RegVal new_status = mmu->getTimberV()->getMtstatus();
                    setMiscRegNoEffect(idx, new_status);
                    setMiscRegNoEffect(MISCREG_STSTATUS, new_status);
                } else {
                    setMiscRegNoEffect(idx, val);
                }
            } break;
          case MISCREG_IP:
            {
                RegVal mask = MI_MASK[getPrivilegeModeSet()];
                auto ic = dynamic_cast<RiscvISA::Interrupts *>(
                    tc->getCpuPtr()->getInterruptController(tc->threadId()));
                val = (val & mask) | (ic->readIP() & ~mask);
                ic->setIP(val);
            }
            break;
          case MISCREG_HVIP:
            {
                auto ic = dynamic_cast<RiscvISA::Interrupts *>(
                    tc->getCpuPtr()->getInterruptController(tc->threadId()));
                ic->setHVIP(val);
            }
            break;
          case MISCREG_IE:
            {
                val = val & MI_MASK[getPrivilegeModeSet()];
                auto ic = dynamic_cast<RiscvISA::Interrupts *>(
                    tc->getCpuPtr()->getInterruptController(tc->threadId()));
                ic->setIE(val);
            }
            break;
          case MISCREG_SATP:
            {
                // The walker implements Sv39, not Sv32. Keep enabled RV32
                // harts Bare: treating bit 31 as an RV64 PPN bit here while
                // SPMP sees a mode bit would let software evade protection.
                if (_enableSspmp && _rvType == RV32) {
                    setMiscRegNoEffect(idx, 0);
                    break;
                }
                // we only support bare and Sv39 mode; setting a different mode
                // shall have no effect (see 4.1.12 in priv ISA manual)
                SATP cur_val = readMiscRegNoEffect(idx);
                SATP new_val = val;
                if (new_val.mode != AddrXlateMode::BARE &&
                    new_val.mode != AddrXlateMode::SV39)
                    new_val.mode = cur_val.mode;

                // TLB flush can be elided here
                // --- From the RISCV Privileged Spec 20250508, p.129 ---
                // "Not imposing upon implementations to flush
                // address-translation caches upon satp writes reduces
                // the cost of context switches provided a sufficiently
                // large ASID space."

                setMiscRegNoEffect(idx, new_val);
            }
            break;

          case MISCREG_VSATP:
            {
                // we only support bare and Sv39 mode; setting a different mode
                // shall have no effect (see 4.1.12 in priv ISA manual)
                SATP cur_val = readMiscRegNoEffect(idx);
                SATP new_val = val;
                if (new_val.mode != AddrXlateMode::BARE &&
                    new_val.mode != AddrXlateMode::SV39)
                    new_val.mode = cur_val.mode;

                setMiscRegNoEffect(idx, new_val);
            }
            break;

          case MISCREG_SENVCFG:
            {
                // panic on write to bitfields that aren't implemented in gem5
                SENVCFG panic_mask = 0;
                panic_mask.pmm = 3;

                SENVCFG wpri_mask = 0;
                wpri_mask.wpri_1 = ~wpri_mask.wpri_1;
                wpri_mask.wpri_2 = ~wpri_mask.wpri_2;
                wpri_mask.wpri_3 = ~wpri_mask.wpri_3;

                if ((panic_mask & val) != 0) {
                    panic("Tried to write to an unimplemented bitfield in the "
                    "senvcfg CSR!\nThe attempted write was:\n %" PRIu64 "\n",
                    val);
                }

                setMiscRegNoEffect(idx, val & ~wpri_mask);
            }
            break;
          case MISCREG_HGATP:
            {
                // TLB flush can be elided here
                // --- From the RISCV Privileged Spec 20250508, p.171 ---
                // Note that writing hgatp does not imply any ordering
                // constraints between page-table updates and
                // subsequent G-stage address translations.
                // If the new virtual machine’s guest physical page tables have
                // been modified, or if a VMID is reused,
                // it may be necessary to execute an HFENCE.GVMA instruction
                // (see Section 21.3.2) before or after writing hgatp.

                SATP cur_val = readMiscRegNoEffect(idx);
                SATP new_val = val;

                if (new_val.mode != AddrXlateMode::BARE &&
                    new_val.mode != AddrXlateMode::SV39)
                {
                    new_val.mode = cur_val.mode;
                }

                setMiscRegNoEffect(idx, new_val);

            }
            break;
          case MISCREG_TSELECT:
            {
                // we don't support debugging, so always set a different value
                // than written
                setMiscRegNoEffect(idx, val + 1);
            }
            break;
          case MISCREG_ISA:
            {
                MISA cur_misa = (MISA)readMiscRegNoEffect(MISCREG_ISA);
                MISA new_misa = (MISA)val;
                // only allow to disable compressed instructions
                // if the following instruction is 4-byte aligned
                if (new_misa.rvc == 0 &&
                        bits(tc->pcState().as<RiscvISA::PCState>().npc(),
                            2, 0) != 0) {
                    new_misa.rvc = new_misa.rvc | cur_misa.rvc;
                }
                if (!getEnableRvv()) {
                    new_misa.rvv = 0;
                }
                new_misa.rvs = cur_misa.rvs;
                new_misa.rvu = cur_misa.rvu;
                if (_enableSspmp)
                    new_misa.rvh = 0;
                setMiscRegNoEffect(idx, new_misa);
            }
            break;
          case MISCREG_STATUS:
            {
                if (_rvType != RV32) {
                    // SXL and UXL are hard-wired to 64 bit
                    auto cur = readMiscRegNoEffect(idx);
                    val &= ~(STATUS_SXL_MASK | STATUS_UXL_MASK);
                    val |= cur & (STATUS_SXL_MASK | STATUS_UXL_MASK);
                }
                if (!getEnableRvv()) {
                    // Always OFF is rvv is disabled.
                    val &= ~STATUS_VS_MASK;
                }
                setMiscRegNoEffect(idx, val);
            }
            break;
          case MISCREG_VSSTATUS:
            {

                auto cur = readMiscRegNoEffect(idx);
                auto wmask_map = CSRWriteMasks[RV64][getPrivilegeModeSet()];
                auto sstatus_wmask = wmask_map.find(CSR_VSSTATUS)->second;
                val = (cur & ~sstatus_wmask) | val;
                setMiscRegNoEffect(idx, val);
            }
            break;
          case MISCREG_VXSAT:
            {
                setMiscRegNoEffect(idx, val & 0x1);
            }
            break;
          case MISCREG_VXRM:
            {
                setMiscRegNoEffect(idx, val & 0x3);
            }
            break;
          case MISCREG_VCSR:
            {
                setMiscRegNoEffect(MISCREG_VXSAT, val & 0x1);
                setMiscRegNoEffect(MISCREG_VXRM, (val & 0x6) >> 1);
            }
            break;
          case MISCREG_PRV:
            {
                setMiscRegNoEffect(idx, val);
            }
            break;
          case MISCREG_VIRT:
            {
                tc->getMMUPtr()->flushAll();
                setMiscRegNoEffect(idx, val);
            }
            break;
          case MISCREG_MNSTATUS:
            {
                NSTATUS nstatus = val;
                setMiscRegNoEffect(MISCREG_NMIE,
                    (RegVal)nstatus.nmie | readMiscRegNoEffect(MISCREG_NMIE));
                setMiscRegNoEffect(idx, val);
            }
            break;
          case MISCREG_JVT:
            {
                setMiscRegNoEffect(idx, rvSext(val));
            }
            break;

          case MISCREG_FFLAGS_EXE:
            {
                RegVal new_val = readMiscRegNoEffect(MISCREG_FFLAGS);
                new_val |= (val & FFLAGS_MASK);
                setMiscRegNoEffect(MISCREG_FFLAGS, new_val);
            }
            break;
          default:
            setMiscRegNoEffect(idx, val);
        }
    }
}

/** Serialize architectural state and the feature profile that interprets it. */
void
ISA::serialize(CheckpointOut &cp) const
{
    BaseISA::serialize(cp);

    DPRINTF(Checkpoint, "Serializing Riscv Misc Registers\n");
    SERIALIZE_CONTAINER(miscRegFile);
    int cptSspmpEnabled = _enableSspmp;
    int cptSspmpenEnabled = _enableSspmpen;
    int cptSmpmpdelegEnabled = _enableSmpmpdeleg;
    int cptSspmpRvType = _rvType;
    int cptSspmpPrivilegeSet = _privilegeModeSet;
    SERIALIZE_SCALAR(cptSspmpEnabled);
    SERIALIZE_SCALAR(cptSspmpenEnabled);
    SERIALIZE_SCALAR(cptSmpmpdelegEnabled);
    SERIALIZE_SCALAR(cptSspmpRvType);
    SERIALIZE_SCALAR(cptSspmpPrivilegeSet);
}

/** Restore architectural state only when its feature profile is compatible. */
void
ISA::unserialize(CheckpointIn &cp)
{
    DPRINTF(Checkpoint, "Unserializing Riscv Misc Registers\n");
    UNSERIALIZE_CONTAINER(miscRegFile);
    int cptSspmpEnabled = 0;
    int cptSspmpenEnabled = 0;
    int cptSmpmpdelegEnabled = 0;
    int cptSspmpRvType = 0;
    int cptSspmpPrivilegeSet = 0;
    const bool hasEnabled = optParamIn(cp, "cptSspmpEnabled", cptSspmpEnabled,
                                       false);
    const bool hasSspmpen = optParamIn(cp, "cptSspmpenEnabled",
                                       cptSspmpenEnabled, false);
    const bool hasSmpmpdeleg = optParamIn(cp, "cptSmpmpdelegEnabled",
                                          cptSmpmpdelegEnabled, false);
    const bool hasRvType = optParamIn(cp, "cptSspmpRvType", cptSspmpRvType,
                                      false);
    const bool hasPrivilegeSet = optParamIn(
        cp, "cptSspmpPrivilegeSet", cptSspmpPrivilegeSet, false);
    fatal_if(hasEnabled != hasRvType || hasRvType != hasPrivilegeSet ||
             hasEnabled != hasSspmpen || hasEnabled != hasSmpmpdeleg,
             "Incomplete SPMP ISA configuration in checkpoint");
    if (!hasEnabled) {
        fatal_if(_enableSspmp || _enableSspmpen || _enableSmpmpdeleg,
                 "Legacy checkpoint has no SPMP ISA metadata; restore "
                 "requires Sspmp disabled");
        warn("Restoring legacy RISC-V checkpoint without SPMP ISA metadata");
    } else {
        fatal_if((cptSspmpEnabled != 0) != _enableSspmp ||
                 (cptSspmpenEnabled != 0) != _enableSspmpen ||
                 (cptSmpmpdelegEnabled != 0) != _enableSmpmpdeleg ||
                 cptSspmpRvType != static_cast<int>(_rvType) ||
                 cptSspmpPrivilegeSet != static_cast<int>(_privilegeModeSet),
                 "SPMP feature, XLEN, or privilege profile differs from "
                 "checkpoint");
    }
    if (_enableSspmp) {
        const MISA misa = miscRegFile[MISCREG_ISA];
        fatal_if(!misa.rvs || !misa.rvu || misa.rvh,
                 "Checkpoint MISA is incompatible with the enabled MSU "
                 "Sspmp profile");
        fatal_if(_rvType == RV32 && miscRegFile[MISCREG_SATP] != 0,
                 "Enabled RV32 Sspmp requires Bare SATP; this walker "
                 "does not implement Sv32");
    }
}

void
ISA::handleLockedSnoop(PacketPtr pkt, Addr cacheBlockMask)
{
    Addr& load_reservation_addr = load_reservation_addrs[tc->contextId()];

    if (load_reservation_addr == INVALID_RESERVATION_ADDR)
        return;
    Addr snoop_addr = pkt->getAddr() & cacheBlockMask;
    DPRINTF(LLSC, "Locked snoop on address %x.\n", snoop_addr);
    if ((load_reservation_addr & cacheBlockMask) == snoop_addr)
        load_reservation_addr = INVALID_RESERVATION_ADDR;
}


void
ISA::handleLockedRead(const RequestPtr &req)
{
    Addr& load_reservation_addr = load_reservation_addrs[tc->contextId()];

    load_reservation_addr = req->getPaddr();
    DPRINTF(LLSC, "[cid:%d]: Reserved address %x.\n",
            req->contextId(), req->getPaddr());
}

bool
ISA::handleLockedWrite(const RequestPtr &req, Addr cacheBlockMask)
{
    Addr& load_reservation_addr = load_reservation_addrs[tc->contextId()];
    bool lr_addr_empty = (load_reservation_addr == INVALID_RESERVATION_ADDR);

    // Normally RISC-V uses zero to indicate success and nonzero to indicate
    // failure (right now only 1 is reserved), but in gem5 zero indicates
    // failure and one indicates success, so here we conform to that (it should
    // be switched in the instruction's implementation)

    DPRINTF(LLSC, "[cid:%d]: load_reservation_addrs empty? %s.\n",
            req->contextId(),
            lr_addr_empty ? "yes" : "no");
    if (!lr_addr_empty) {
        DPRINTF(LLSC, "[cid:%d]: addr = %x.\n", req->contextId(),
                req->getPaddr() & cacheBlockMask);
        DPRINTF(LLSC, "[cid:%d]: last locked addr = %x.\n", req->contextId(),
                load_reservation_addr & cacheBlockMask);
    }
    if (lr_addr_empty ||
            (load_reservation_addr & cacheBlockMask)
            != ((req->getPaddr() & cacheBlockMask))) {
        req->setExtraData(0);
        int stCondFailures = tc->readStCondFailures();
        tc->setStCondFailures(++stCondFailures);
        if (stCondFailures % WARN_FAILURE == 0) {
            warn("%i: context %d: %d consecutive SC failures.\n",
                    curTick(), tc->contextId(), stCondFailures);
        }

        // Must clear any reservations
        load_reservation_addr = INVALID_RESERVATION_ADDR;

        return false;
    }
    if (req->isUncacheable()) {
        req->setExtraData(2);
    }

    // Must clear any reservations
    load_reservation_addr = INVALID_RESERVATION_ADDR;

    DPRINTF(LLSC, "[cid:%d]: SC success! Current locked addr = %x.\n",
            req->contextId(), load_reservation_addr & cacheBlockMask);
    return true;
}

void
ISA::globalClearExclusive()
{
    tc->getCpuPtr()->wakeup(tc->threadId());
    Addr& load_reservation_addr = load_reservation_addrs[tc->contextId()];
    load_reservation_addr = INVALID_RESERVATION_ADDR;
}

/** Reset the indirect selector along with the hart's architectural reset. */
void
ISA::resetThread()
{
    setMiscRegNoEffect(MISCREG_SISELECT, 0);
    setMiscRegNoEffect(MISCREG_MISELECT, 0);
    Reset().invoke(tc);
}

bool
ISA::virtualizationEnabled() const
{
    // Virtualized when V-bit is 1
    return readMiscRegNoEffect(MISCREG_VIRT) == 1;
}

void
ISA::swapToVirtCSR(
    uint64_t& csr, RegIndex& midx, std::string& csrName)
{
    switch (csr) {
        case CSR_SSTATUS:  csr = CSR_VSSTATUS;  break;
        case CSR_STVEC:    csr = CSR_VSTVEC;    break;
        case CSR_SSCRATCH: csr = CSR_VSSCRATCH; break;
        case CSR_SEPC:     csr = CSR_VSEPC;     break;
        case CSR_SCAUSE:   csr = CSR_VSCAUSE;   break;
        case CSR_STVAL:    csr = CSR_VSTVAL;    break;
        case CSR_SATP:     csr = CSR_VSATP;     break;
        case CSR_SIP:      csr = CSR_VSIP;      break;
        case CSR_SIE:      csr = CSR_VSIE;      break;
        default:           return;              break;
    }

    // Lookup new midx and csrName
    auto csr_data_it = getCSRDataMap().find(csr);
    if (csr_data_it == getCSRDataMap().end()) {
        panic("Bad remapping of virtualized CSR");
    }
    midx = csr_data_it->second.physIndex;
    csrName = csr_data_it->second.name;
}

Fault
ISA::tvmChecks(uint64_t csr, PrivilegeMode pm, ExtMachInst machInst)
{
    if (csr == CSR_SATP) {
        STATUS status = readMiscReg(MISCREG_STATUS);
        if (pm != PRV_M && status.tvm == 1) {
            return std::make_shared<IllegalInstFault>(
                    "SATP access with TVM enabled\n",
                    machInst);
        }
    } else if (csr == CSR_VSATP) {
        if (virtualizationEnabled()) {
        HSTATUS hstatus = readMiscReg(MISCREG_HSTATUS);
        if (hstatus.vtvm == 1)
            return std::make_shared<VirtualInstFault>(
                "VSATP access with hstatus.vtvm enabled",
                machInst);
        }
    } else if (csr == CSR_HGATP) {
        STATUS status = readMiscReg(MISCREG_STATUS);
        if (pm != PRV_M && status.tvm == 1) {
            return std::make_shared<IllegalInstFault>(
                    "HGATP access with TVM enabled\n",
                    machInst);
        }
    }


    return NoFault;
}

/** Validate indirect CSR selections and reject unsupported extension banks. */
Fault
ISA::indirectCSRCheck(uint64_t csr, ExtMachInst machInst) const
{
    switch (csr) {
      case CSR_MISELECT:
      case CSR_MIREG:
      case CSR_MIREG2:
      case CSR_MIREG3:
      case CSR_MIREG4:
      case CSR_MIREG5:
      case CSR_MIREG6:
        if (!_enableSmpmpdeleg)
            return std::make_shared<IllegalInstFault>(
                "Machine indirect CSRs require Smpmpdeleg", machInst);
        if (csr == CSR_MISELECT)
            return NoFault;
        if (csr != CSR_MIREG && csr != CSR_MIREG2)
            return std::make_shared<IllegalInstFault>(
                "Reserved machine indirect CSR alias", machInst);
        if (readMiscRegNoEffect(MISCREG_MISELECT) < 0x100 ||
            readMiscRegNoEffect(MISCREG_MISELECT) > 0x13f)
            return std::make_shared<IllegalInstFault>(
                "Unsupported machine indirect CSR selection", machInst);
        return NoFault;
      case CSR_SISELECT:
      case CSR_SIREG:
      case CSR_SIREG2:
      case CSR_SIREG3:
      case CSR_SIREG4:
      case CSR_SIREG5:
      case CSR_SIREG6:
        break;
      default:
        if (csr == CSR_SPMPEN || csr == CSR_SPMPENH) {
            if (!_enableSspmpen)
                return std::make_shared<IllegalInstFault>(
                    "SPMP enable CSRs require Sspmpen", machInst);
        } else if (csr == CSR_MPMPDELEG && !_enableSmpmpdeleg) {
            return std::make_shared<IllegalInstFault>(
                "mpmpdeleg requires Smpmpdeleg", machInst);
        }
        return NoFault;
    }

    // Guest indirect CSRs are deferred; never fall through to the host bank.
    if (virtualizationEnabled()) {
        return std::make_shared<VirtualInstFault>(
            "Guest supervisor indirect CSRs are not implemented", machInst);
    }
    if (csr == CSR_SISELECT)
        return NoFault;

    if (csr != CSR_SIREG && csr != CSR_SIREG2) {
        return std::make_shared<IllegalInstFault>(
            "Reserved supervisor indirect CSR alias", machInst);
    }
    const RegVal selector = readMiscRegNoEffect(MISCREG_SISELECT);
    if (selector < 0x100 || selector > 0x13f) {
        return std::make_shared<IllegalInstFault>(
            "Unsupported supervisor indirect CSR selection", machInst);
    }
    if (!_enableSspmp) {
        return std::make_shared<IllegalInstFault>(
            "SPMP indirect CSRs require Sspmp to be enabled", machInst);
    }
    return NoFault;
}

RegVal
ISA::backdoorReadCSRAllBits(ExecContext *xc, uint64_t csr)
{
    auto csr_it = getCSRDataMap().find(csr);

    // panic if the method was used with bad csr idx
    panic_if(csr_it == getCSRDataMap().end(),
        "Illegal CSR passed to backdoorReadCSRAllBits");

    auto midx = csr_it->second.physIndex;

    RegVal readval = xc->readMiscReg(midx);

    // Special handling for FCSR
    if (csr == CSR_FCSR) {
        readval = (xc->readMiscReg(MISCREG_FFLAGS) |
            (xc->readMiscReg(MISCREG_FRM) << FRM_OFFSET));
    }

    return readval;
}

RegVal
ISA::readCSR(ExecContext *xc, uint64_t csr)
{
    auto csr_it = getCSRDataMap().find(csr);

    // panic if the method was used with bad csr idx
    panic_if(csr_it == getCSRDataMap().end(), "Illegal CSR passed to readCSR");


    auto mask_it = getCSRMaskMap().find(csr);
    RegVal maskVal = (mask_it == getCSRMaskMap().end()) ?
                        mask(64) : mask_it->second;

    RegVal readval = backdoorReadCSRAllBits(xc, csr) & maskVal;


    // Some registers need additional masking/shifting
    // to read the correct value
    switch (csr) {
        case CSR_SIP: case CSR_SIE:
        case CSR_HIP: case CSR_HIE:
        {
            RegVal mideleg = xc->readMiscReg(MISCREG_MIDELEG);
            readval &= mideleg;
            break;
        }
        // VSIP and VSIE bits are stored one bit to the left
        // However reads expect them in SIP and SIE positions!
        case CSR_VSIP: case CSR_VSIE:
        {
            RegVal hideleg = xc->readMiscReg(MISCREG_HIDELEG);
            readval &= hideleg;
            readval >>= 1;
            break;
        }

        case CSR_HVIP:
        {
            INTERRUPT mip = xc->readMiscReg(MISCREG_IP);
            readval |= (mip.vssi << 2);
            break;
        }
        default: break;
    }

    return readval;
}

void
ISA::writeCSR(ExecContext *xc, uint64_t csr, RegVal writeData)
{
    auto csr_it = getCSRDataMap().find(csr);

    // panic if the method was used with bad csr idx
    panic_if(csr_it == getCSRDataMap().end(),
        "Illegal CSR passed to writeCSR");

    // find physical register id (MISCREG == physical)
    auto midx = csr_it->second.physIndex;

    switch (csr) {
        case CSR_SIP: case CSR_SIE:
        case CSR_HIP: case CSR_HIE:
        {
            RegVal mideleg = xc->readMiscReg(MISCREG_MIDELEG);
            writeData &= mideleg;
            break;
        }

        // Special case for VSIP & VSIE
        // We presented the bits in SIP and SIE (one position right)
        // But we shift them up one again so that they don't overwrite
        // the actual SIP and SIE but instead take their actual places.
        // This happens because XIP and XIE are shared registers
        // but VS-level should be unaware that its SIP and SIE bits are stored
        // elsewhere.
        case CSR_VSIP: case CSR_VSIE:
        {
            RegVal hideleg = xc->readMiscReg(MISCREG_HIDELEG);
            writeData <<= 1;
            writeData &= hideleg;
            break;
        }
        default:
            break;
    }

    auto& csr_read_masks = getCSRMaskMap();
    auto& csr_write_masks = getCSRWriteMaskMap();

    // If no read mask or write mask, assume all bits are writable
    RegVal write_mask = mask(64);

    // To save space, we only specify write masks that differ
    // from the respective read masks in misc.hh
    // If there is no write mask, use the read mask to write
    // all "visible" aka readable bits. Else use mask(64).
    auto write_mask_it = csr_write_masks.find(csr);
    if (write_mask_it != csr_write_masks.end()) {
        write_mask = write_mask_it->second;
    } else {
        auto read_mask_it = csr_read_masks.find(csr);
        if (read_mask_it != csr_read_masks.end()) {
            write_mask = read_mask_it->second;
        }
    }

    auto writeDataMasked = writeData & write_mask;

    // CSRs are often aliases with different visibility
    // on the same physical register (MISCREG).
    // We must keep the values of the non-visible bits
    // intact for other privileges/contexts where
    // they are used.

    // Read all the CSR bits
    auto reg_data_all = backdoorReadCSRAllBits(xc, csr);

    // Only modify those in writeMask
    auto new_reg_data_all = (reg_data_all & ~write_mask)
                          | (writeData & write_mask);


    switch (csr) {
        case CSR_FCSR: {
            xc->setMiscReg(MISCREG_FFLAGS, bits(writeDataMasked, 4, 0));
            xc->setMiscReg(MISCREG_FRM, bits(writeDataMasked, 7, 5));
            break;
        }

        case CSR_HVIP: {
            // vssi bit is an alias, propagate to mip
            INTERRUPT mip = xc->readMiscReg(MISCREG_IP);
            mip.vssi = (new_reg_data_all & VSSI_MASK) >> 2;
            xc->setMiscReg(MISCREG_IP, mip);

            // turn off vssi for write to HVIP
            new_reg_data_all &= ~VSSI_MASK;

            // finally write hvip
            xc->setMiscReg(midx, new_reg_data_all);
            break;
        }
        // case CSR_MIP: case CSR_MIE:
        // case CSR_HIP: case CSR_HIE:
        // case CSR_SIP: case CSR_SIE:
        // case CSR_VSIP: case CSR_VSIE:
        // case CSR_MSTATUS: case CSR_SSTATUS:
        //     xc->setMiscReg(midx, new_reg_data_all);
        //     break;
        default:
            xc->setMiscReg(midx, new_reg_data_all);
            break;
    }
}

Addr
ISA::getFaultHandlerAddr(RegIndex idx, uint64_t cause, bool intr) const
{
    auto vec = tc->readMiscRegNoEffect(idx);
    Addr addr = mbits(vec, 63, 2);
    if (intr && bits(vec, 1, 0) == 1)
        addr += 4 * cause;
    return addr;
}

// V-bit utilities (H-extension)
bool virtualizationEnabled(ExecContext *xc) {
    return xc->readMiscReg(MISCREG_VIRT);
}
bool virtualizationEnabled(ThreadContext *tc) {
    return tc->readMiscReg(MISCREG_VIRT);
}

void setV(ExecContext *xc) {
    assert(!virtualizationEnabled(xc));
    xc->setMiscReg(MISCREG_VIRT, 1);
}
void setV(ThreadContext *tc) {
    assert(!virtualizationEnabled(tc));
    tc->setMiscReg(MISCREG_VIRT, 1);
}

void resetV(ExecContext *xc) {
    assert(virtualizationEnabled(xc));
    xc->setMiscReg(MISCREG_VIRT, 0);
}
void resetV(ThreadContext *tc) {
    assert(virtualizationEnabled(tc));
    tc->setMiscReg(MISCREG_VIRT, 0);
}

// FPU status update function
Fault
updateFPUStatus(ExecContext *xc, ExtMachInst machInst, bool set_dirty)
{
    MISA misa = xc->readMiscReg(MISCREG_ISA);
    STATUS status = xc->readMiscReg(MISCREG_STATUS);
    STATUS vsstatus = misa.rvh && virtualizationEnabled(xc) ?
        xc->readMiscReg(MISCREG_VSSTATUS) : 0;

    if (status.fs == FPUStatus::OFF ||
        (misa.rvh && virtualizationEnabled(xc) &&
         vsstatus.fs == FPUStatus::OFF))
        return std::make_shared<IllegalInstFault>("FPU is off", machInst);

    if (set_dirty) {
        status.fs = FPUStatus::DIRTY;
        xc->setMiscReg(MISCREG_STATUS, status);

        if (misa.rvh && virtualizationEnabled(xc)) {
            vsstatus.fs = FPUStatus::DIRTY;
            xc->setMiscReg(MISCREG_VSSTATUS, vsstatus);
        }
    }

    return NoFault;
}

// VPU status update function
Fault
updateVPUStatus(
    ExecContext *xc, ExtMachInst machInst, bool set_dirty, bool check_vill)
{
    MISA misa = xc->readMiscReg(MISCREG_ISA);
    STATUS status = xc->readMiscReg(MISCREG_STATUS);
    STATUS vsstatus = misa.rvh && virtualizationEnabled(xc) ?
        xc->readMiscReg(MISCREG_VSSTATUS) : 0;

    if (!misa.rvv || status.vs == VPUStatus::OFF ||
        (misa.rvh && virtualizationEnabled(xc) &&
         vsstatus.vs == VPUStatus::OFF)) {
        return std::make_shared<IllegalInstFault>(
            "RVV is disabled or VPU is off", machInst);
    }

    if (check_vill && machInst.vill) {
        return std::make_shared<IllegalInstFault>("VILL is set", machInst);
    }

    if (set_dirty) {
        status.vs = VPUStatus::DIRTY;
        xc->setMiscReg(MISCREG_STATUS, status);

        if (misa.rvh && virtualizationEnabled(xc)) {
            vsstatus.vs = VPUStatus::DIRTY;
            xc->setMiscReg(MISCREG_VSSTATUS, vsstatus);
        }
    }

    return NoFault;
}


} // namespace RiscvISA
} // namespace gem5

std::ostream &
operator<<(std::ostream &os, gem5::RiscvISA::PrivilegeMode pm)
{
    switch (pm) {
    case gem5::RiscvISA::PRV_U:
        return os << "PRV_U";
    case gem5::RiscvISA::PRV_S:
        return os << "PRV_S";
    case gem5::RiscvISA::PRV_M:
        return os << "PRV_M";
    default:
        return os << "PRV_<invalid>";
    }
}
