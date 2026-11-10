from m5.objects.BaseCPU import BaseCPU
from m5.params import Param
from m5.SimObject import PyBindMethod, SimObject


class RiscvSPMPTest(SimObject):
    type = "RiscvSPMPTest"
    cxx_header = "spmp_test.hh"
    cxx_class = "gem5::RiscvISA::SPMPTest"

    cpu = Param.BaseCPU("CPU whose architectural context is under test")
    cxx_exports = [
        PyBindMethod("run"),
        PyBindMethod("seedCheckpoint"),
        PyBindMethod("verifyCheckpoint"),
        PyBindMethod("rejectSharedProfile"),
    ]
