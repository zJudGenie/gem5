# SPMP chapter 2 suite

The original chapter 2 implementation stopped after static review. The subsequent
authorized Podman build and execution stage is recorded in the
[container verification report](../../../../SPMP_CONTAINER_VERIFICATION.md).
Its environment, actual results and runtime fixes supersede that earlier
unexecuted status.

The suite uses local ELF files, real privileged M/S/U execution and a test-only
SimObject for otherwise inaccessible request boundaries. It needs no kernel,
disk, downloaded resource, assembler extension-name support, or opaque binary.
Numeric CSRs implement the indirect window. The supported profile is MSU, H off.

## Structure and build order

```text
riscv_spmp/
    fixture/       # Test-only C++ SimObject and SConscript
    workload/      # Privileged C/assembly guests and linker script
    config.py      # Full-system simulation configuration
    run.py         # Complete 94-scenario matrix and output oracles
    test.py        # Alternative testlib registration of six guest scenarios
```

Prepare the verification container separately, with the dependencies required by
this gem5 checkout (C++ compiler, Python development headers, SCons, zlib, m4,
Kconfig tooling and existing build dependencies) and a RISC-V bare-metal GCC/binutils
toolchain. `riscv64-unknown-elf-gcc` must support both `rv32imac_zicsr/ilp32` and
`rv64imac_zicsr/lp64`. No libc is needed. `CROSS=` selects another compiler prefix.
The tests use C/assembly, ELF loading, semihosting output, and m5 pseudo-ops.

From the **gem5 submodule directory**, execute in this order:

```sh
export PYTHONDONTWRITEBYTECODE=1
scons defconfig build/SPMP build_opts/RISCV EXTRAS=tests/gem5/riscv_spmp/fixture
scons setconfig build/SPMP USE_TEST_OBJECTS=y EXTRAS=tests/gem5/riscv_spmp/fixture
scons build/SPMP/gem5.opt EXTRAS=tests/gem5/riscv_spmp/fixture -j1
make -C tests/gem5/riscv_spmp/workload OUT=../build CROSS=riscv64-unknown-elf- all
python3 tests/gem5/riscv_spmp/run.py --gem5 build/SPMP/gem5.opt --binaries tests/gem5/riscv_spmp/build --outdir tests/gem5/riscv_spmp/out/manual-run --group all
```

These commands assume sufficient RAM. The original 5 GiB build needed additional
compiler/linker memory settings, described in the historical verification report.
The environment setting keeps Python bytecode out of the source directories.
`EXTRAS` registers `fixture/SConscript`; together with `USE_TEST_OBJECTS=y` it
includes `RiscvSPMPTest`. No fixture files or registration live in
`src/arch/riscv/`. For a fresh production build omit the extra directory and
leave test objects disabled. EXTRAS is remembered per build directory; pass
`EXTRAS=` explicitly when clearing it from an existing build.
The Makefile places ELFs and linker maps under `OUT`; gem5
build products stay under `build/SPMP`. Simulator output, logs, command records,
checkpoints and `results.json` go under the supplied `--outdir`. Do not put
generated artifacts in the source directory or reuse an output directory from a
different build/configuration when evaluating results.

The runner's independent groups are `config`, `fixture`, `guest`, and
`lifecycle`. For a focused later run, replace `--group all` above with one group.
The runner does not build or download anything. Its default wall-clock timeout
is 180 seconds per process; `--timeout` changes that bound. Each guest simulation
phase is also bounded to 50 billion ticks; an expiry fails.

An individual invocation, useful when diagnosing a later failure, is:

```sh
build/SPMP/gem5.opt --outdir tests/gem5/riscv_spmp/out/manual tests/gem5/riscv_spmp/config.py --binary tests/gem5/riscv_spmp/build/spmp-rv64-on-n16.elf --xlen 64 --cpu minor --feature on --entries 16
```

Use the runner for final oracles: a zero process exit alone does not prove guest
success. Testlib's `test.py` also registers the six RV32/RV64 ×
AtomicSimpleCPU/TimingSimpleCPU/MinorCPU enabled workloads. It expects the ELFs in
`tests/gem5/riscv_spmp/build`; `SPMP_BINARIES` can override that local directory.
The full rejection/checkpoint/switch matrix is in `run.py`, not testlib discovery.

## Concrete cases and expected observations

The runner schedules 36 configuration, 14 direct fixture, 18 guest and 26
lifecycle processes. Each process checks multiple assertions. Backend/guest
coverage deliberately overlaps: direct requests isolate range and translation
rules, while guests prove instruction decoding, precise trap delivery and memory
effects. XLEN, CPU and entry-count repetitions exercise different configurations.
A source audit found no exact duplicate scenario within the full runner matrix.
This does not mean the assertions are disjoint: lifecycle runs execute the
common guest phase again to establish state before switching/checkpointing.
The six enabled 16-entry guests also appear in `test.py` as an alternative testlib
entry point; invoking both runners repeats those six, not additional coverage.

Read [workload/CASES.md](workload/CASES.md) for guest case IDs and
[FIXTURE_CASES.md](FIXTURE_CASES.md) for direct fixture inputs/oracles.
The [implementation review](../../../../SPMP_CHAPTER2_REVIEW.md) records
requirement-by-requirement and access-path coverage.

| Group | Prepared scenarios and required observations |
| --- | --- |
| Activation/profile | `config-rv*-default` leaves feature/count unassigned; enabled counts 0/1/16/64 on both XLENs; negative counts −1/65, profiles M/MU/MHSU, backend XLEN mismatch. ISA strings asserted against actual parent-MMU count. Configuration rejection requires a nonzero exit and the specific diagnostic |
| CSR programming | Guest instruction-level selector/readback/RMW/suppression/lock/privilege probes; fixture checks disabled-feature allocation illegality versus enabled zero-count zero/ignored state |
| Range/matching | Fixture uses explicit address/size cases at zero, exact boundaries, top of physical space, overflow, overlapping and adjacent entries; complete request checked |
| Permission matrix | Explicit named-bit Figure 4 table for S/U/shared and SUM, M bypass, WARL, SUM/MXR writability, execute-only loads. No paging-style MXR inference |
| Effective privilege | Guest and fixture MPRV/MPP probes; extra `fixture-rv*-rnmi` enables Smrnmi so NMIE=0/1 behavior is separately observable; instruction privilege independent of MPRV |
| Memory operations | Guest fetch/load/store/LR/SC/AMO trap records and denied-store/AMO sentinels; fixture observes whole-request boundaries and full sizes |
| Fault priority | Guest aligned SPMP/PMP denial; fixture checks actual `PMAChecker` deferred PA misalignment at 0x80004001 (allowed-misalignment range is 0x80006000–0x80006fff), SPMP/PMP causes, fault values and PMA flags |
| Paging/paths | Bare atomic/timing/functional/physical request fixtures and privileged CPU workloads; RV64 valid Sv39 mappings, repeated hits/misses and timing completion; RV32 Sv32 limitation below |
| Reconfiguration/fences | Guest repeats allow→deny→allow accesses/fetches with `SFENCE.VMA x0,x0` after previous accesses/cache population |
| Reset/checkpoint | Fixture checks reset invariants. `fixture-save/restore-rv*` preserves multi-entry locked TOR+selector and checks translations. `save/restore-rv*` resumes actual timing guest after checkpoint and checks state/behavior. Restore feature/count/XLEN mismatch processes must reject |
| CPU takeover | `switch-rv*-atomic-timing`, `timing-minor`, `minor-timing`: actual guest config/locks before first exit, state and permissions checked after switching. Feature/count/XLEN mismatches must reject. Direct shared-profile binding mismatch checks the MMU validator |

## Interpreting results

- Guest emits `SPMP_PHASE_READY` before its first `m5_exit`. The configuration
  then resumes, saves a checkpoint, or switches CPU as requested. After resume,
  the guest checks state and actual allow/deny behavior, emits
  `SPMP_WORKLOAD_PASS`, and exits again. `SPMP_WORKLOAD_FAIL`, unexpected traps,
  panic, timeout, missing final marker, and nonzero exit are failures.
- Checkpoint creation expects `SPMP_CHECKPOINT_SAVED` rather than a final guest
  pass. Restore is a distinct process using `--restore`; it must reach final
  guest pass or the fixture's `SPMP_FIXTURE_RESTORE_PASS`.
- Rejection cases are configuration/lifecycle failures by design. The runner
  accepts only a positive nonzero exit with the named SPMP diagnostic. A signal,
  arbitrary panic, or architectural guest page fault is not a substitute.
- Fixtures fail immediately on an incorrect cause, fault value, range decision,
  callback count or state invariant. Their pass marker is distinct from actual
  privileged guest execution. Per-case applicability is reported in their source
  documentation; zero entries do not run meaningful nonzero-entry deny tests.
- CPU/XLEN combinations are explicitly scheduled for all three CPU models. The
  source contains no restriction excluding RV32 MinorCPU in MSU. A later build
  or execution failure must be investigated, not relabelled as a skipped pass.

## Explicit limits

Sv32 is not implemented in this checkout's SATP layout/walker. Enabled RV32
supports Bare only; attempted Sv32 selection must read back zero and must not
disable SPMP. **The suite cannot prove valid Sv32 paging and does not mark that
coverage as passed.** RV64 Sv39 is the prepared paged-execution proof.

The non-H profile excludes VS/VU, Shbare, HLV/HLVX/HSV, chapters 3–4 and the
machine indirect window. SE, DMA, physical loader/debugger accesses, and KVM do
not establish architectural S/U enforcement. Cross-line atomics are already
unsupported by the CPU implementation. CMO/vector/prefetch source paths are
audited in the implementation report; instruction-level test limitations are
listed explicitly in the guest case document. No claim of exhaustive testing of
unrelated instruction extensions or every possible CPU decomposition is made.

Actual execution results and remaining coverage limits are recorded in the
container verification report linked above.
