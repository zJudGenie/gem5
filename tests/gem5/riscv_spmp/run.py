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
    parser.add_argument(
        "--group",
        choices=(
            "all", "config", "fixture", "guest", "lifecycle", "extensions"
        ),
        default="all",
    )
    parser.add_argument("--timeout", type=int, default=180)
    return parser.parse_args()


def scenarios(args):
    """Yield concrete invocations and independent output/rejection oracles."""
    def case(
        name,
        xlen=64,
        cpu="atomic",
        feature="on",
        count=16,
        mode="guest",
        extra=(),
        reject=None,
        marker=None,
        binary=None,
        spmpen="off",
        delegation="off",
        pmp_entries=None,
    ):
        """Describe one process without executing it."""
        elf = args.binaries / (binary or f"test_csr-rv{xlen}.elf")
        command = [
            str(args.gem5.resolve()),
            "--outdir", str((args.outdir / name).resolve()),
            str(Path(__file__).with_name("config.py").resolve()),
            "--binary", str(elf.resolve()),
            "--xlen", str(xlen),
            "--cpu", cpu,
            "--feature", feature,
            "--mode", mode,
            "--spmpen", spmpen,
            "--delegation", delegation,
            *([] if count is None else ["--entries", str(count)]),
            *([] if pmp_entries is None else ["--pmp-entries", str(pmp_entries)]),
            *map(str, extra),
        ]
        return dict(
            name=name,
            command=command,
            rejection=reject,
            marker=marker or ("SPMP_WORKLOAD_PASS" if mode == "guest" else "SPMP_CONFIG_PASS"),
        )

    # 1. Config scenarios (7 cases: 1 per distinct fatal_if guard + positive default)
    if args.group in ("all", "config"):
        yield case("config-default", xlen=64, feature="default", count=None, mode="config")
        yield case(
            "reject-entries-negative", xlen=64, count=-1, mode="config",
            reject=r"SPMP supports between zero and 64 entries",
        )
        yield case(
            "reject-entries-overflow", xlen=64, count=65, mode="config",
            reject=r"SPMP supports between zero and 64 entries",
        )
        yield case(
            "reject-profile-non-msu", xlen=64, mode="config",
            extra=("--profile", "MU"),
            reject=r"Sspmp requires the non-virtualized MSU",
        )
        yield case(
            "reject-backend-xlen", xlen=64, mode="config",
            extra=("--backend-xlen", 32),
            reject=r"SPMP architecture must match the hart",
        )
        yield case(
            "reject-ext-without-spmp", xlen=64, feature="off", count=4,
            spmpen="on", mode="config",
            reject=r"Sspmpen and Smpmpdeleg require Sspmp",
        )
        yield case(
            "reject-switch-mismatch", xlen=64,
            extra=("--switch-to", "timing", "--switch-entries", 1),
            reject=r"Cannot switch CPUs with incompatible SPMP",
        )

    # 2. Fixture scenarios (3 cases)
    if args.group in ("all", "fixture"):
        yield case(
            "fixture-rv32", xlen=32, binary="test_csr-rv32.elf", mode="fixture",
            extra=("--rnmi",), marker="SPMP_FIXTURE_PASS",
        )
        yield case(
            "fixture-rv64", xlen=64, binary="test_csr-rv64.elf", mode="fixture",
            extra=("--rnmi",), marker="SPMP_FIXTURE_PASS",
        )
        yield case(
            "reject-fixture-shared", xlen=64, binary="test_csr-rv64.elf",
            mode="fixture-shared-mismatch",
            reject=r"RISC-V harts sharing an MMU must have matching SPMP",
        )

    # 3. Guest scenarios (14 cases: 7 per XLEN)
    if args.group in ("all", "guest"):
        for xlen in (32, 64):
            yield case(
                f"guest-csr-rv{xlen}", xlen=xlen, cpu="atomic", count=16,
                binary=f"test_csr-rv{xlen}.elf",
            )
            yield case(
                f"guest-matching-rv{xlen}", xlen=xlen, cpu="atomic", count=16,
                binary=f"test_matching-rv{xlen}.elf",
            )
            yield case(
                f"guest-permissions-rv{xlen}", xlen=xlen, cpu="timing", count=16,
                binary=f"test_permissions-rv{xlen}.elf",
            )
            yield case(
                f"guest-paging-fences-rv{xlen}", xlen=xlen, cpu="minor", count=16,
                binary=f"test_paging_fences-rv{xlen}.elf",
            )
            yield case(
                f"guest-disabled-rv{xlen}", xlen=xlen, feature="default", count=None,
                binary=f"test_csr-rv{xlen}.elf",
            )
            yield case(
                f"guest-n0-rv{xlen}", xlen=xlen, count=0,
                binary=f"test_csr-rv{xlen}.elf",
            )
            yield case(
                f"guest-n64-rv{xlen}", xlen=xlen, count=64,
                binary=f"test_csr-rv{xlen}.elf",
            )

    # 4. Extensions scenarios (6 cases: 3 per XLEN)
    if args.group in ("all", "extensions"):
        for xlen in (32, 64):
            yield case(
                f"ext-shared-rv{xlen}", xlen=xlen, count=0, spmpen="on",
                delegation="on", pmp_entries=64, binary=f"test_extensions-rv{xlen}.elf",
            )
            yield case(
                f"ext-spmpen-rv{xlen}", xlen=xlen, count=64, spmpen="on",
                delegation="off", binary=f"test_extensions-rv{xlen}.elf",
            )
            yield case(
                f"ext-delegation-only-rv{xlen}", xlen=xlen, count=16, spmpen="off",
                delegation="on", pmp_entries=64, binary=f"test_extensions-rv{xlen}.elf",
            )

    # 5. Lifecycle scenarios (7 cases: 3 per XLEN + 1 reject-restore-mismatch)
    if args.group in ("all", "lifecycle"):
        for xlen in (32, 64):
            yield case(
                f"switch-rv{xlen}-atomic-timing", xlen=xlen, cpu="atomic",
                binary=f"test_csr-rv{xlen}.elf", extra=("--switch-to", "timing"),
            )
            checkpoint = (args.outdir / f"guest-cpt-rv{xlen}").resolve()
            yield case(
                f"save-rv{xlen}", xlen=xlen, cpu="timing",
                binary=f"test_csr-rv{xlen}.elf",
                extra=("--checkpoint", checkpoint),
                marker="SPMP_CHECKPOINT_SAVED",
            )
            yield case(
                f"restore-rv{xlen}", xlen=xlen, cpu="timing",
                binary=f"test_csr-rv{xlen}.elf",
                extra=("--restore", checkpoint),
            )
        cpt64 = (args.outdir / "guest-cpt-rv64").resolve()
        yield case(
            "reject-restore-mismatch", xlen=64, count=1, mode="config",
            binary="test_csr-rv64.elf", extra=("--restore", cpt64),
            reject=r"differs from checkpoint SPMP table size",
        )


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
            result = subprocess.run(
                item["command"], stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, timeout=args.timeout,
            )
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
        (args.outdir / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    if not results or not all(item["passed"] for item in results):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
