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

    /** Run direct backend and translation checks and fail on the first error. */
    void run();

    /** Seed a locked, multi-entry configuration for checkpoint validation. */
    void seedCheckpoint();

    /** Verify checkpoint-restored selector, entries, and enforcement. */
    void verifyCheckpoint();

    /** Deliberately bind an incompatible context to exercise shared-MMU checks. */
    void rejectSharedProfile();
};

} // namespace RiscvISA
} // namespace gem5

#endif
