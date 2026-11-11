"""Registration of local SPMP guest scenarios using modular test ELFs.

The complete configuration/rejection/lifecycle matrix is in run.py. Build the
ELFs separately before using testlib; no network resource fixture is involved.
"""

import os

from testlib import *


binary_dir = os.environ.get("SPMP_BINARIES", joinpath(absdirpath(__file__), "build"))
workloads = [
    ("csr", "test_csr", "atomic", 16),
    ("matching", "test_matching", "atomic", 16),
    ("permissions", "test_permissions", "timing", 16),
    ("paging-fences", "test_paging_fences", "minor", 16),
    ("extensions", "test_extensions", "atomic", 64),
]

for xlen in (32, 64):
    for name, binary_prefix, cpu, entries in workloads:
        extra_args = (
            ["--spmpen", "on", "--delegation", "on", "--pmp-entries", "64"]
            if name == "extensions"
            else []
        )
        gem5_verify_config(
            name=f"riscv-spmp-rv{xlen}-{name}",
            verifiers=(
                verifier.MatchRegex("SPMP_WORKLOAD_PASS"),
                verifier.MatchRegex("SPMP_RUN_COMPLETE"),
                verifier.NoMatchRegex("SPMP_WORKLOAD_FAIL|panic:"),
            ),
            config=joinpath(absdirpath(__file__), "config.py"),
            config_args=[
                "--binary", joinpath(binary_dir, f"{binary_prefix}-rv{xlen}.elf"),
                "--xlen", str(xlen), "--cpu", cpu, "--feature", "on",
                "--entries", str(entries), *extra_args,
            ],
            valid_isas=(constants.riscv_tag,),
            length=constants.long_tag,
        )
