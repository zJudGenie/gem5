# Privileged workload cases

Execution results are recorded in the container verification report linked from
the suite README.

`start.S` starts in M, disables delegation/interrupts, preserves an M stack in
`run_probe`, and enters individual S/U probes with `mret`. Probes never modify
SP. MPRV probes jump directly while remaining in M, with MPP selecting data
privilege. The handler clears MPRV before accessing its log, records
`{cause,tval,mepc,case_id}`, compares all three trap fields, and returns to the
saved M frame. An unexpected trap outside a frame terminates through m5_fail.
Case IDs increment in deterministic source order for each compiled profile;
failure diagnostics also print the case name and actual/expected trap fields.

A successful memory probe must reach its exact ecall label with cause 8, 9 or
11 and tval zero. A denied operation must trap at the labelled memory
instruction (or the aligned target PC for a fetch), with causes 12/13/15 and the
specified address. Every intentional negative fetch entrypoint is aligned to
the configured 64-byte cache line. Timeout, panic, an unexpected trap or missing
pass marker fails; none is treated as a protection success.

## Inputs and oracles

The labels below identify the concrete source function/probe and its diagnostic
prefix, rather than assuming the same numeric case count for every profile.

| Cases / source | Concrete input | Expected observation |
| --- | --- | --- |
| Activation: main | Feature off, any authored count; empty table, permissive PMP | Selector remains generic; sireg access traps cause 2 at `probe_invalid_alias_fault`, tval 0x151022f3. U load/fetch/store reach their ecalls |
| Activation: main | Feature on, count 0 | Address/config aliases zero; writes ignored; U load/fetch/store reach ecalls |
| Activation: main | Feature on, nonzero count, reset entries all OFF | U fetch at `probe_u_load` faults 12 with PC=tval=probe address |
| Activation RV32 | Write SATP=0x80000000 | Readback zero; subsequent nonzero-table empty Bare fetch still faults 12. No Sv32 execution claim |
| Status: `test_status_bits` | Set and clear sstatus SUM/MXR through CSR writes on both XLENs | Both bits read back as written, original state restored |
| CSR: `test_csr_programming` | siselect 0, 0xfff, 0x100, 0x13f; alternate selectors | Exact retained selector, canonical address/config readback including U/SHARED; configured entry switching |
| CSR bounds | Entry 63 at count64; position63/count boundary at smaller counts | Implemented entry stores address; unimplemented aliases read zero and ignore writes |
| CSR RMW/suppression | CSRRS with rs1=x0; CSRRS/CSRRC X bit; CSRRW rd=x0 | Suppressed write leaves state; RMW uses complete backend config; read-suppressed write has correct readback |
| CSR errors | Select 0x140 then access sireg; access sireg3; U access siselect | Cause2 at exact CSR PC, tval respective instruction words 0x151022f3, 0x153022f3, 0x150022f3 |
| CSR WARL/locks | Reserved SHARED/U or R/W; lock high entry; lock TOR successor14 with predecessor13 (counts16/64) | Canonical legal configs; own config/address writes ignored; predecessor address write ignored |
| Permissions: `test_permissions` | U RW region; S SUM=0/1 | U loads/stores succeed; S load faults13 with SUM0 and succeeds with SUM1 |
| MXR | U X-only data; S SUM=1, MXR=1 | S load still faults13; SPMP does not import paging MXR rules |
| Shared R+W | SHARED=U=1, R=W=1,X=0 | U load succeeds, U store faults15; S store succeeds with SUM0 |
| Shared RWX | SHARED=U=1,R=W=X=1, executable target at0x80006000 | U target-page ecall executes; U data load/store fault13/15, including load from the executing page |
| Legal/reserved R/W/X loop | Packed values0..7, named R/W/X bit expectations | U data permissions follow canonical R and W; code X separately permits fetch; S never executes U-only code even with SUM1. Complete S/U/shared Figure4 matrix is independently tabulated in the fixture |
| Actual LR/SC/AMO | U RW data; LR/SC pair; AMOADD.W | Successful SC changes target to0x13579bdf and records status0; AMO increments0x51a7c0de to0x51a7c0df; the pair proves LR has load permission |
| Denied stores/atomics | U read-only data; then no data permissions for LR | Store/SC/AMO fault15 at their own labelled instructions, LR faults13. Single denied store/AMO leave0x51a7c0de sentinel unchanged |
| Fetch | U code rule X cleared after prior accesses | Fault12, exact aligned probe PC/tval |
| TOR/NA4 | TOR `[target,target+8)`, predecessor OFF; NA4 `[target,target+4)` | Exact word and last byte allowed; halfword at+7/+3 faults13 for partial overlap, +8 outside TOR faults13. PMA's configured range permits these misaligned requests so SPMP is observable |
| MPRV | M execution, MPRV0 with MPP U/S/M; MPRV1 with MPP U/S/M | Off ignores MPP; effective M succeeds. Effective U/S data denied13 under restrictive table; code remains M fetch. NMIE=0/1 with Smrnmi is isolated in direct fixtures |
| Split: `test_split_store` | SW0x11223344 at0x8000603e, crossing64-byte line; writable page, higher-priority read-only NA4 at0x80006040 | Fault15 at store PC. Whole request may fault at603e with no effect, or second fragment at6040; denied bytes and guards stay0xaa, prior allowed two bytes may remain0xaa or become44,33. Prints which request form was observed. It never treats partial effects as proof of a denied single request changing memory |
| Fences: `test_fences` | Warm fetch/data, change data R allow→deny→allow; code X allow→deny→allow; full SFENCE after each change | Subsequent access alternates ecall→13→ecall and fetch ecall→12→ecall; no stale cache/protection state can hide the update |
| Priority: `test_fault_priority` | SPMP and PMP both deny aligned data; later both deny a whole code page | Causes13/15/12, exact memory/fetch PC and tval; PMP baseline permits handler/code where needed. Concrete PMA priority/flags are isolated in fixture |
| Paging: `test_paging` RV64 | Sv39 1GiB identity leaf at root[2], S-only RWX/A/D, restrictive SPMP | S load/fetch succeed on first/repeated access and after SFENCE-induced misses; instruction/data TLBs and timing walker completion remain active |
| Paging PMP/PTE faults | PMP denies target page while permitting code/PTE; then reserved R=0,W=1 leaf | Load access fault5 at data instruction; after TLB flush invalid leaf yields fetch page fault12. Returning SATP to Bare restores SPMP load fault13 |
| Lifecycle: before first exit | Counts16/64: code/data U rules, OFF TOR predecessor3, locked read-only TOR4, second locked NAPOT5, S fallback6; selector0x102 | `SPMP_PHASE_READY` then m5_exit after nontrivial state is installed |
| Lifecycle: after resume/switch/restore | Same raw state, without replaying SPMP writes | Selector checked before changing it; raw addresses/config/locks retained; TOR predecessor/config writes ignored; actual U locked-region load succeeds, store faults15 and sentinel0x7150cafe unchanged. PMP is re-established explicitly because its existing takeover is outside this change |

The dedicated fixture additionally covers address zero, equal/reversed TOR,
physical limits, maximum NAPOT, overflowing/zero-size requests, first-index
overlap priority, adjacent rules, architectural functional translation, physical
finalization, deferred PMA alignment/flags, reset and backend checkpoint ranges.
Those inputs need direct request control and are not replaced with opaque guest
workloads.

## Authored profiles and applicability

`make all` authors both XLENs for feature on/off and counts0/1/16/64. The primary
execution matrix is counts16, both XLENs and AtomicSimpleCPU, TimingSimpleCPU,
MinorCPU; the runner also schedules enabled0/1/64 and disabled16 guest profiles.
All are local ELFs and remain **unbuilt/unrun** at this stopping point.

- Disabled/zero profiles execute the inactive-access and alias cases; nonzero
  permission/matching cases are inapplicable to their table. They still use the
  two-exit protocol.
- Count1 executes active empty-table denial and CSR/bounds/lock cases. The
  multi-entry guest cases need at least3 entries; split needs4, lifecycle needs8.
  Direct count1 fixture tests still cover ranges and the permission matrix.
- Counts16/64 execute all applicable guest cases. Count64 also exercises actual
  entry63 programming. RV64 executes Sv39; RV32 checks forced Bare and continued
  enforcement. A nonexistent Sv32 walker is not silently marked passed.

The Makefile selects `rv32imac_zicsr/ilp32` or `rv64imac_zicsr/lp64`, uses
freestanding headers only, and writes ELFs/maps under `OUT`. The linker pins
probe, data, split, locked and page-table sections and asserts they do not overlap;
the source fixes a64-byte cache line in the supplied configuration. Startup
clears BSS. No libraries, kernel or disk image are required.

## Explicit remaining coverage limits

There are no instruction-level vector, CMO or prefetch workloads here; their
translation/issue paths were reviewed statically. The suite tests scalar32-bit
loads/stores and atomics on both XLENs, one halfword boundary crossing and one
supported split store. It does not enumerate FSD-on-RV32 or every vector/segment
decomposition. Cross-line atomics are unsupported by the existing CPU code.
Shared-MMU mismatch is tested by direct profile binding, not an SMT execution
workload. Legacy metadata-free checkpoint handling is reviewed statically;
authored restore rejection processes cover current feature/count/XLEN mismatches.

These are coverage limits, not passing results. Compilation, assembler support,
trap execution, LR/SC forward progress, actual cache behavior, checkpoint drain
and CPU takeover remain unverified until the separate containerized stage.
