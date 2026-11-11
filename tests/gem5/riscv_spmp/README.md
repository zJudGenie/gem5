# SPMP suite

The test suite validates RISC-V Sspmp (Supervisor Physical Memory Protection), Sspmpen,
and Smpmpdeleg implementation in gem5.

The suite combines modular bare-metal guest ELFs with a focused C++ translation fixture
testing internal `Request`/MMU boundary invariants.

## Structure

```text
riscv_spmp/
    fixture/       # C++ SimObject for internal MMU/Request boundary tests
    workload/      # Modular privileged bare-metal C/assembly guest workloads
    config.py      # gem5 simulation configuration script
    run.py         # Test runner and oracle validation matrix (~37 scenarios)
    test.py        # testlib discovery integration for guest workloads
```
## SPMP backend and translation fixture

`RiscvSPMPTest` is a test-only SimObject that tests the internal C++ `Request`/MMU
invariants that guest instructions cannot synthesize.
Its sources live in `fixture/`, registered through `EXTRAS=tests/gem5/riscv_spmp/fixture`
with `USE_TEST_OBJECTS=y`. The fixture runs in full-system mode after `m5.instantiate()`
and before `m5.simulate()`.

### C++ Invariants Checked by `run()`

1. **Synthetic invalid `Request` bounds**: `size == 0` and 64-bit `MaxAddr - 3` size `8`
   (`paddr + size` overflow) rejection directly in `SPMP::spmpCheck()`.
2. **Functional debugger bypass**: `Request::funcRequestorId` bypasses sPMP in
   `translateFunctional`, while normal architectural `translateFunctional`, `finalizePhysical`,
   and `translateTiming` enforce sPMP.
3. **Direct `PMAChecker` integration at `0x80004000`**: sPMP fault takes priority over
   deferred `MMU::WordAlign` misalignment fault; when sPMP allows, `LOAD_ADDR_MISALIGNED` and
   `isUncacheable()` / `isStrictlyOrdered()` flags are preserved.
4. **Smrnmi effective privilege**: With `enableSmrnmi()`, `NMIE = 0` disables `MPRV`
   translation modification while `NMIE = 1` enforces `MPRV` effective privilege.
5. **Shared-MMU profile mismatch rejection (`rejectSharedProfile()`)**: Deliberately binds
   an incompatible profile to verify the MMU profile binding validator rejects mismatches.

## Workloads and Binaries

The suite uses 5 focused, deduplicated guest workloads built by `workload/Makefile`:
- `test_csr-rv{32,64}.elf`: CSR legality, indirect window indexing, WARL canonicalization.
- `test_matching-rv{32,64}.elf`: Address range matching (TOR, NA4, NAPOT) and priority.
- `test_permissions-rv{32,64}.elf`: Permission matrix (Figure 4), SUM control, LR/SC/AMO faults.
- `test_paging_fences-rv{32,64}.elf`: Sv39 paging walks with sPMP, `SFENCE.VMA` ordering.
- `test_extensions-rv{32,64}.elf`: Sspmpen enable bits and Smpmpdeleg dynamic partitioning.

## Build and Execution

From the **gem5 submodule directory**, use the container (see top-level README.md)

```sh
export PYTHONDONTWRITEBYTECODE=1
scons defconfig build/SPMP build_opts/RISCV EXTRAS=tests/gem5/riscv_spmp/fixture
scons setconfig build/SPMP USE_TEST_OBJECTS=y EXTRAS=tests/gem5/riscv_spmp/fixture
scons build/SPMP/gem5.opt EXTRAS=tests/gem5/riscv_spmp/fixture -j$(nproc)
make -C tests/gem5/riscv_spmp/workload OUT=../build CROSS=riscv64-unknown-elf- all
python3 tests/gem5/riscv_spmp/run.py --gem5 build/SPMP/gem5.opt --binaries tests/gem5/riscv_spmp/build --outdir tests/gem5/riscv_spmp/out/run --group all
```

## Runner Test Groups

The runner matrix (~37 scenarios) is organized into 5 groups:
- `config`: Configuration validation and negative rejection tests (bounds, invalid profiles, XLEN mismatch).
- `fixture`: C++ internal MMU invariant checks (synthetic bounds, functional debugger bypass, PMAChecker integration, RNMI effect).
- `guest`: Full guest execution across CPU models (`atomic`, `timing`, `minor`) and entry counts (0, 16, 64).
- `extensions`: Sspmpen standalone, Smpmpdeleg standalone, and combined shared-MMU guests.
- `lifecycle`: CPU switching (`atomic` -> `timing`), checkpoint save, restore, and table-size mismatch rejection.
