from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject


class SPMP(SimObject):
    type = "SPMP"
    cxx_header = "arch/riscv/spmp.hh"
    cxx_class = "gem5::RiscvISA::SPMP"

    spmp_entries = Param.Int(
        16, "Standalone SPMP capacity (0-64); ignored under Smpmpdeleg"
    )
    delegation_enabled = Param.Bool(
        False, "Share PMP backing under Smpmpdeleg"
    )
    riscv_type = Param.RiscvType("RV64", "32 or 64 bits")
