"""Local full-system SPMP workload and translation-fixture configuration.

Use run.py for exit-code, timeout, and output oracles.
"""

import argparse
from pathlib import Path

import m5
from m5.objects import (
    AddrRange,
    BadAddr,
    Cache,
    PMAChecker,
    RiscvAtomicSimpleCPU,
    RiscvBareMetal,
    RiscvISA,
    RiscvMinorCPU,
    RiscvSemihosting,
    RiscvTimingSimpleCPU,
    Root,
    SimpleMemory,
    SrcClockDomain,
    System,
    SystemXBar,
    VoltageDomain,
)


def arguments():
    """Parse local inputs without fetching or building any resource."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--xlen", type=int, choices=(32, 64), default=64)
    parser.add_argument(
        "--cpu", choices=("atomic", "timing", "minor"), default="atomic"
    )
    parser.add_argument("--feature", choices=("default", "on", "off"), default="on")
    parser.add_argument("--entries", type=int)
    parser.add_argument(
        "--profile", choices=("M", "MU", "MSU", "MHSU"), default="MSU"
    )
    parser.add_argument("--backend-xlen", type=int, choices=(32, 64))
    parser.add_argument("--rnmi", action="store_true")
    parser.add_argument(
        "--mode",
        choices=(
            "guest", "fixture", "fixture-seed", "fixture-restore",
            "fixture-shared-mismatch", "config",
        ),
        default="guest",
    )
    parser.add_argument("--switch-to", choices=("atomic", "timing", "minor"))
    parser.add_argument("--switch-feature", choices=("on", "off"))
    parser.add_argument("--switch-entries", type=int)
    parser.add_argument("--switch-xlen", type=int, choices=(32, 64))
    parser.add_argument("--restore", type=Path)
    parser.add_argument("--checkpoint", type=Path)
    parser.add_argument("--max-ticks", type=int, default=50_000_000_000)
    return parser.parse_args()


def make_cpu(kind, xlen, feature, count, profile, switched_out=False):
    """Construct an independent ISA/MMU pair for a real CPU or switch target."""
    classes = {
        "atomic": RiscvAtomicSimpleCPU,
        "timing": RiscvTimingSimpleCPU,
        "minor": RiscvMinorCPU,
    }
    cpu = classes[kind](cpu_id=0, switched_out=switched_out)
    isa = RiscvISA(
        riscv_type=f"RV{xlen}", privilege_mode_set=profile, enable_rvv=False
    )
    if feature != "default":
        isa.enable_Sspmp = feature == "on"
    cpu.isa = [isa]
    if count is not None:
        cpu.mmu.spmp.spmp_entries = count
    cpu.createThreads()
    cpu.createInterruptController()
    cpu.mmu.pma_checker = PMAChecker(
        uncacheable=[AddrRange(0x80004000, size=0x1000)],
        misaligned=[AddrRange(0x80006000, size=0x1000)],
    )
    return cpu


def check_advertisement(cpu):
    """Check actual parent-MMU count against the public ISA string."""
    isa = cpu.isa[0]
    text = isa.get_isa_string()
    implemented = isa.enable_Sspmp.value and cpu.mmu.spmp.spmp_entries.value > 0
    assert ("_Sspmp" in text) == implemented, text
    assert ("_Sscsrind" in text) == (isa.privilege_mode_set.value == "MSU"), text
    print("SPMP_ISA", cpu.path(), text, flush=True)


def successful_exit(limit):
    """Reject timeout, panic, m5_fail, and every unanticipated exit reason."""
    event = m5.simulate(limit)
    if event.getCause() != "m5_exit instruction encountered" or event.getCode() != 0:
        raise RuntimeError(
            f"SPMP unexpected exit: {event.getCause()} code={event.getCode()}"
        )


def main():
    """Run one explicit scenario; run.py supplies the external output oracle."""
    args = arguments()
    if not args.binary.is_file():
        raise FileNotFoundError(args.binary)
    system = System()
    system.clk_domain = SrcClockDomain(clock="1GHz", voltage_domain=VoltageDomain())
    system.cache_line_size = 64
    system.mem_ranges = [AddrRange(0x80000000, size="32MiB")]
    system.cpu = make_cpu(
        args.cpu, args.xlen, args.feature, args.entries, args.profile
    )
    system.cpu.isa[0].enable_Smrnmi = args.rnmi
    if args.backend_xlen is not None:
        system.cpu.mmu.spmp.riscv_type = f"RV{args.backend_xlen}"
    system.mem_mode = system.cpu.memory_mode()
    system.membus = SystemXBar()
    system.membus.badaddr_responder = BadAddr()
    system.membus.default = system.membus.badaddr_responder.pio
    system.system_port = system.membus.cpu_side_ports
    system.memory = SimpleMemory(range=system.mem_ranges[0], latency="10ns")
    system.memory.port = system.membus.mem_side_ports
    cache_args = dict(
        size="16KiB", assoc=2, tag_latency=1, data_latency=1,
        response_latency=1, mshrs=4, tgts_per_mshr=8,
    )
    system.cpu.addPrivateSplitL1Caches(Cache(**cache_args), Cache(**cache_args))
    system.cpu.connectBus(system.membus)
    system.workload = RiscvBareMetal(
        bootloader=str(args.binary.resolve()),
        semihosting=RiscvSemihosting(),
    )
    if args.switch_to:
        system.next_cpu = make_cpu(
            args.switch_to,
            args.switch_xlen or args.xlen,
            args.switch_feature or args.feature,
            args.switch_entries if args.switch_entries is not None else args.entries,
            args.profile,
            switched_out=True,
        )
    if args.mode.startswith("fixture"):
        from m5.objects import RiscvSPMPTest

        system.spmp_test = RiscvSPMPTest(cpu=system.cpu)
    root = Root(full_system=True, system=system)
    check_advertisement(system.cpu)
    if args.switch_to:
        check_advertisement(system.next_cpu)
    m5.instantiate(str(args.restore.resolve()) if args.restore else None)
    if args.mode == "config":
        print("SPMP_CONFIG_PASS", flush=True)
        return
    if args.mode == "fixture":
        system.spmp_test.run()
        print("SPMP_FIXTURE_PASS", flush=True)
        return
    if args.mode == "fixture-shared-mismatch":
        system.spmp_test.rejectSharedProfile()
        raise RuntimeError("SPMP shared-profile mismatch was accepted")
    if args.mode == "fixture-seed":
        if not args.checkpoint:
            raise ValueError("fixture-seed requires --checkpoint")
        system.spmp_test.seedCheckpoint()
        m5.checkpoint(str(args.checkpoint.resolve()))
        print("SPMP_CHECKPOINT_SAVED", flush=True)
        return
    if args.mode == "fixture-restore":
        if not args.restore:
            raise ValueError("fixture-restore requires --restore")
        system.spmp_test.verifyCheckpoint()
        print("SPMP_FIXTURE_RESTORE_PASS", flush=True)
        return
    if not args.restore:
        successful_exit(args.max_ticks)
        if args.checkpoint:
            m5.checkpoint(str(args.checkpoint.resolve()))
            print("SPMP_CHECKPOINT_SAVED", flush=True)
            return
        if args.switch_to:
            m5.switchCpus(system, [(system.cpu, system.next_cpu)])
            print("SPMP_SWITCH_COMPLETE", flush=True)
    successful_exit(args.max_ticks)
    print("SPMP_RUN_COMPLETE", flush=True)


if __name__ == "__m5_main__":
    main()
