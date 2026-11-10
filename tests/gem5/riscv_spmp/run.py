#!/usr/bin/env python3
"""Execute the authored SPMP suite against explicitly supplied local artifacts.

This runner never builds gem5 or guest binaries and never downloads resources.
"""

import argparse
import json
from pathlib import Path
import re
import subprocess


def arguments():
    """Require explicit simulator, local ELF, and output locations."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gem5", type=Path, required=True)
    parser.add_argument("--binaries", type=Path, required=True)
    parser.add_argument("--outdir", type=Path, required=True)
    parser.add_argument("--group", choices=("all", "config", "fixture", "guest", "lifecycle"), default="all")
    parser.add_argument("--timeout", type=int, default=180)
    return parser.parse_args()


def scenarios(args):
    """Yield concrete invocations and independent output/rejection oracles."""
    def case(name, xlen=64, cpu="atomic", feature="on", count=16,
             mode="guest", extra=(), reject=None, marker=None, binary_feature=None):
        """Describe one process without executing it."""
        enabled = feature == "on" if binary_feature is None else binary_feature
        elf = args.binaries / f"spmp-rv{xlen}-{'on' if enabled else 'off'}-n{count if count in (0, 1, 16, 64) else 16}.elf"
        command = [str(args.gem5.resolve()), "--outdir", str((args.outdir / name).resolve()),
                   str(Path(__file__).with_name("config.py").resolve()),
                   "--binary", str(elf.resolve()), "--xlen", str(xlen), "--cpu", cpu,
                   "--feature", feature, "--mode", mode,
                   *([] if count is None else ["--entries", str(count)]),
                   *map(str, extra)]
        return dict(name=name, command=command, rejection=reject,
                    marker=marker or ("SPMP_WORKLOAD_PASS" if mode == "guest" else "SPMP_CONFIG_PASS"))

    if args.group in ("all", "config"):
        for xlen in (32, 64):
            # 'default' deliberately leaves enable_Sspmp unassigned.
            yield case(f"config-rv{xlen}-default", xlen, feature="default", count=None, mode="config", binary_feature=False)
            for count in (0, 1, 16, 64):
                yield case(f"config-rv{xlen}-n{count}", xlen, count=count, mode="config")
                yield case(f"config-rv{xlen}-off-n{count}", xlen, feature="off",
                           count=count, mode="config")
            for count in (-1, 65):
                yield case(f"reject-rv{xlen}-n{count}", xlen, count=count, mode="config",
                           reject=r"SPMP supports between zero and 64 entries")
            for profile in ("M", "MU", "MHSU"):
                for count in (0, 16):
                    yield case(f"reject-rv{xlen}-{profile}-n{count}", xlen,
                               count=count, mode="config",
                               extra=("--profile", profile),
                               reject=r"Sspmp requires the non-virtualized MSU")
            yield case(f"reject-rv{xlen}-backend-xlen", xlen, mode="config",
                       extra=("--backend-xlen", 64 if xlen == 32 else 32),
                       reject=r"SPMP architecture must match the hart")

    if args.group in ("all", "fixture"):
        for xlen in (32, 64):
            for count in (0, 1, 16, 64):
                yield case(f"fixture-rv{xlen}-n{count}", xlen, count=count, mode="fixture", marker="SPMP_FIXTURE_PASS")
            yield case(f"fixture-rv{xlen}-disabled", xlen, feature="default", mode="fixture",
                       marker="SPMP_FIXTURE_PASS", binary_feature=False)
            yield case(f"fixture-rv{xlen}-rnmi", xlen, mode="fixture",
                       extra=("--rnmi",), marker="SPMP_FIXTURE_PASS")
            yield case(f"reject-fixture-rv{xlen}-shared", xlen, mode="fixture-shared-mismatch",
                       reject=r"RISC-V harts sharing an MMU must have matching SPMP")

    if args.group in ("all", "guest"):
        for xlen in (32, 64):
            for cpu in ("atomic", "timing", "minor"):
                yield case(f"guest-rv{xlen}-{cpu}", xlen, cpu)
                yield case(f"guest-rv{xlen}-{cpu}-disabled", xlen, cpu, feature="default", binary_feature=False)
            for count in (0, 1, 64):
                yield case(f"guest-rv{xlen}-n{count}", xlen, count=count)

    if args.group in ("all", "lifecycle"):
        for xlen in (32, 64):
            for old, new in (("atomic", "timing"), ("timing", "minor"), ("minor", "timing")):
                yield case(f"switch-rv{xlen}-{old}-{new}", xlen, old, extra=("--switch-to", new))
            for what, value in (("feature", "off"), ("entries", 1), ("xlen", 64 if xlen == 32 else 32)):
                yield case(f"reject-switch-rv{xlen}-{what}", xlen,
                           extra=("--switch-to", "timing", f"--switch-{what}", value),
                           reject=r"Cannot switch CPUs with incompatible SPMP")
            checkpoint = (args.outdir / f"guest-cpt-rv{xlen}").resolve()
            yield case(f"save-rv{xlen}", xlen, "timing", extra=("--checkpoint", checkpoint), marker="SPMP_CHECKPOINT_SAVED")
            yield case(f"restore-rv{xlen}", xlen, "timing", extra=("--restore", checkpoint))
            yield case(f"reject-restore-rv{xlen}-xlen", 64 if xlen == 32 else 32,
                       "timing", mode="config", extra=("--restore", checkpoint),
                       reject=r"SPMP feature, XLEN, or privilege profile differs")
            for feature, count, reason in (
                ("off", 16, r"SPMP feature, XLEN, or privilege profile differs"),
                ("on", 1, r"differs from checkpoint SPMP table size"),
            ):
                yield case(f"reject-restore-rv{xlen}-{feature}-n{count}", xlen, "timing", feature, count,
                           mode="config", extra=("--restore", checkpoint), reject=reason,
                           binary_feature=feature == "on")
            fixture_cpt = (args.outdir / f"fixture-cpt-rv{xlen}").resolve()
            yield case(f"fixture-save-rv{xlen}", xlen, mode="fixture-seed",
                       extra=("--checkpoint", fixture_cpt), marker="SPMP_CHECKPOINT_SAVED")
            yield case(f"fixture-restore-rv{xlen}", xlen, mode="fixture-restore",
                       extra=("--restore", fixture_cpt), marker="SPMP_FIXTURE_RESTORE_PASS")


def main():
    """Run bounded processes; panic, timeout, missing markers, or signals fail."""
    args = arguments()
    args.outdir = args.outdir.resolve()
    args.outdir.mkdir(parents=True, exist_ok=True)
    results = []
    for item in scenarios(args):
        directory = args.outdir / item["name"]
        directory.mkdir(parents=True, exist_ok=True)
        try:
            result = subprocess.run(item["command"], stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, timeout=args.timeout)
            output = result.stdout
            rejection = item["rejection"]
            success = (
                result.returncode > 0 and re.search(rejection, output) is not None
                if rejection else result.returncode == 0 and item["marker"] in output
            )
            success = bool(success and "panic:" not in output and "SPMP_WORKLOAD_FAIL" not in output)
            record = dict(name=item["name"], passed=success, exit_code=result.returncode)
        except subprocess.TimeoutExpired as error:
            output = error.stdout or b""
            if isinstance(output, bytes):
                output = output.decode(errors="replace")
            record = dict(name=item["name"], passed=False, error="wall-clock timeout")
        (directory / "run.log").write_text(output)
        (directory / "command.json").write_text(json.dumps(item, indent=2) + "\n")
        results.append(record)
        print(f"{'PASS' if record['passed'] else 'FAIL'} {item['name']}", flush=True)
        # Preserve results if a later process is interrupted.
        (args.outdir / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    if not results or not all(item["passed"] for item in results):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
