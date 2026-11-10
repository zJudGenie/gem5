# SPMP backend and translation fixture

`RiscvSPMPTest` is a test-only SimObject that inspects the real per-MMU backend
and invokes the RISC-V MMU entry points with controlled `Request` objects.
Its sources and build registration live in `fixture/`, included through
`EXTRAS=tests/gem5/riscv_spmp/fixture` with `USE_TEST_OBJECTS=y`.
Execution results are recorded in the container verification report linked from
the suite README. The fixture checks architectural fault causes and `tval` by invoking the returned
fault on the test hart, then restores the trap state so the cases remain
independent. It must run in full-system mode after `m5.instantiate()` and before
`m5.simulate()`.

`run()` covers these deterministic cases:

- Disabled Sspmp with PMP made permissive and enabled zero-entry Sspmp both
  allow the same aligned S-mode Bare load. Disabled aliases fault; zero-entry
  implemented aliases return zero and ignore writes. Selector `0`, `0xfff`,
  `0x100`, and `0x13f` read back, unsupported selector aliases fault, and
  reserved `sireg3`–`sireg6` fault.
- Backend canonical CSR values retain the complete U/SHARED fields. Reserved
  `W=1,R=0` and `SHARED=1,U=0` writes canonicalize to legal WARL values.
- TOR entry zero beginning at address zero, nonzero TOR first/last bytes and
  end-exclusive byte, empty equal/reversed TOR bounds, NA4 boundaries, NAPOT ranges,
  partial overlap, lowest-index overlapping-entry priority, and a request
  spanning adjacent entries that cannot jointly authorize it.
- RV32 and RV64 maximum NAPOT physical ranges accept the last representable
  byte, reject the physical limit, reject a range crossing the limit, reject an
  overflowing/out-of-range or zero-size request, and deny S-mode when no entry matches.
- The six legal packed X/W/R configurations are checked using explicit
  Figure 4 expected-permission rows. The cases cover S-only, U-only with SUM
  clear/set, shared S/U with SUM clear/set, shared U read-only for `R=W=1,X=0`,
  shared U execute-only for `R=W=X=1`, execute-only loads with MXR set, and
  M-mode bypass. LR uses Read permission; SC uses Write and fault cause 15; an
  AMO checks its read plus write requirements and reports cause 15.
- Atomic Bare loads and stores, architectural functional translation, timing
  immediate completion, `Request::PHYSICAL`, and physical finalization all hit
  the common active SPMP check. The timing callback records exactly one finish.
  A request using gem5's reserved functional-proxy requestor ID bypasses SPMP.
- SPMP faults 12/13/15 take priority over an empty PMP table; a load with a high
  physical-address bit on RV64 also reports SPMP first. An SPMP allow retains PMP
  access faults, PMA misalignment faults, and PMA uncacheable/strict-order flags.
  The PMA cases require the fixture configuration's deferred-alignment range
  and uncacheable region listed in `README.md`.
- MPRV data requests use MPP=S/U/M, fetch ignores MPRV, and both NMIE states are
  checked when Smrnmi is enabled. With Smrnmi disabled, NMIE does not suppress
  MPRV. Reconfiguration allow-to-deny-to-allow transitions are checked against
  repeated MMU requests, and reset retains address/permissions while clearing
  A/L and resetting `siselect`.
- On enabled RV32, attempting to write SATP bit 31 retains zero/Bare and the
  active empty table still denies S access. This checkout has no Sv32 walker;
  the fixture does not claim Sv32 paging coverage. Sv39 mapping and restrictive
  SPMP while paging is active are exercised by the bare-metal workload cases.

`seedCheckpoint()` prepares locked NA4, locked TOR, and shared NAPOT entries,
sets `siselect=0x102`, and leaves the CPU ready for checkpoint serialization.
`verifyCheckpoint()` checks restored selector/config/address values, lock
behavior, TOR first/last/end boundaries and actual MMU allow/deny translations.
`rejectSharedProfile()` must exit
with the expected fatal diagnostic when invoked after the MMU bound its normal
profile; this direct call tests the binding validator, not a multi-hart SMT
configuration.

The fixture itself does not execute guest instructions or prove that a denied
store leaves a memory sentinel unchanged. Those observations, guest CSR
privilege checks, trap PC records, actual Sv39 mappings, fence-after-cache-use,
CPU-model switching, and incompatible CPU-switch rejection are assigned to the
privileged workload suite in `workload/CASES.md`.
