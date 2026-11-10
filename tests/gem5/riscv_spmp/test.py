"""Alternative testlib registration of six local SPMP guest scenarios.

The complete configuration/rejection/lifecycle matrix is in run.py. Build the
ELFs separately before using testlib; no network resource fixture is involved.
"""

import os

from testlib import *


binary_dir = os.environ.get("SPMP_BINARIES", joinpath(absdirpath(__file__), "build"))
for xlen in (32, 64):
    for cpu in ("atomic", "timing", "minor"):
        gem5_verify_config(
            name=f"riscv-spmp-rv{xlen}-{cpu}",
            verifiers=(
                verifier.MatchRegex("SPMP_WORKLOAD_PASS"),
                verifier.MatchRegex("SPMP_RUN_COMPLETE"),
                verifier.NoMatchRegex("SPMP_WORKLOAD_FAIL|panic:"),
            ),
            config=joinpath(absdirpath(__file__), "config.py"),
            config_args=[
                "--binary", joinpath(binary_dir, f"spmp-rv{xlen}-on-n16.elf"),
                "--xlen", str(xlen), "--cpu", cpu, "--feature", "on", "--entries", "16",
            ],
            valid_isas=(constants.riscv_tag,),
            length=constants.long_tag,
        )
