from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject


class TimberV(SimObject):
    type = "TimberV"
    cxx_header = "arch/riscv/timberv.hh"
    cxx_class = "gem5::RiscvISA::TimberV"
