#ifndef __TESTS_GEM5_RISCV_SPMP_FIXTURE_SPMP_TEST_HH__
#define __TESTS_GEM5_RISCV_SPMP_FIXTURE_SPMP_TEST_HH__

#include "params/RiscvSPMPTest.hh"
#include "sim/sim_object.hh"

namespace gem5
{

class BaseCPU;
namespace RiscvISA
{

class SPMPTest : public SimObject
{
  private:
    BaseCPU *cpu;

  public:
    PARAMS(RiscvSPMPTest);
    SPMPTest(const RiscvSPMPTestParams &p);

    /** Run the 5 C++-only Request and MMU invariant checks. */
    void run();

    /** Deliberately bind an incompatible context to exercise shared-MMU checks. */
    void rejectSharedProfile();
};

} // namespace RiscvISA
} // namespace gem5

#endif // __TESTS_GEM5_RISCV_SPMP_FIXTURE_SPMP_TEST_HH__
