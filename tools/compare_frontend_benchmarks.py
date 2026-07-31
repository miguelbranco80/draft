#!/usr/bin/env python3
"""Run and compare the isolated C++ and Draft frontend benchmarks.

Both child executables perform their own warmup, sampling, checksumming, and
median selection. This coordinator runs outside every timed region. It rejects
different phase sets, source sizes, token/node counts, iteration counts, or
checksums before displaying a ratio, so a fast result from different work is
never presented as a compiler improvement.
"""

from __future__ import annotations

import argparse
import csv
import io
import pathlib
import subprocess
import sys
from dataclasses import dataclass


@dataclass(frozen=True)
class Observation:
    """One stable TSV row emitted by a benchmark child."""

    implementation: str
    phase: str
    bytes: int
    tokens: int
    nodes: int
    iterations: int
    median_ns: int
    ns_per_byte_x1000: int
    ns_per_token_x1000: int
    checksum: int


def run_benchmark(
    executable: pathlib.Path,
    source: pathlib.Path,
    iterations: int,
    repository: pathlib.Path,
) -> dict[str, Observation]:
    """Run one child and decode its complete deterministic phase table."""

    completed = subprocess.run(
        [str(executable), str(source), str(iterations)],
        cwd=repository,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise RuntimeError(
            f"{executable.name} exited {completed.returncode}: {detail}"
        )

    rows: dict[str, Observation] = {}
    reader = csv.DictReader(io.StringIO(completed.stdout), delimiter="\t")
    expected_fields = [field.name for field in Observation.__dataclass_fields__.values()]
    if reader.fieldnames != expected_fields:
        raise RuntimeError(
            f"{executable.name} emitted an unexpected header: {reader.fieldnames}"
        )
    for encoded in reader:
        observation = Observation(
            implementation=encoded["implementation"],
            phase=encoded["phase"],
            bytes=int(encoded["bytes"]),
            tokens=int(encoded["tokens"]),
            nodes=int(encoded["nodes"]),
            iterations=int(encoded["iterations"]),
            median_ns=int(encoded["median_ns"]),
            ns_per_byte_x1000=int(encoded["ns_per_byte_x1000"]),
            ns_per_token_x1000=int(encoded["ns_per_token_x1000"]),
            checksum=int(encoded["checksum"]),
        )
        if observation.phase in rows:
            raise RuntimeError(
                f"{executable.name} emitted duplicate phase {observation.phase}"
            )
        rows[observation.phase] = observation
    if not rows:
        raise RuntimeError(f"{executable.name} emitted no observations")
    return rows


def require_matching_work(
    bootstrap: dict[str, Observation], draft: dict[str, Observation]
) -> None:
    """Reject a comparison unless both implementations performed equal work."""

    if list(bootstrap) != list(draft):
        raise RuntimeError(
            "benchmark phase order differs: "
            f"C++={list(bootstrap)}, Draft={list(draft)}"
        )
    for phase, cpp in bootstrap.items():
        other = draft[phase]
        cpp_identity = (
            cpp.bytes,
            cpp.tokens,
            cpp.nodes,
            cpp.iterations,
            cpp.checksum,
        )
        draft_identity = (
            other.bytes,
            other.tokens,
            other.nodes,
            other.iterations,
            other.checksum,
        )
        if cpp_identity != draft_identity:
            raise RuntimeError(
                f"phase {phase} performed different work: "
                f"C++={cpp_identity}, Draft={draft_identity}"
            )


def print_comparison(
    bootstrap: dict[str, Observation], draft: dict[str, Observation]
) -> None:
    """Display per-iteration latency and Draft/C++ ratios without subtraction."""

    print(
        f"{'phase':<22} {'C++ us/iter':>12} {'Draft us/iter':>14} "
        f"{'Draft/C++':>10} {'C++ ns/B':>10} {'Draft ns/B':>11}"
    )
    for phase, cpp in bootstrap.items():
        other = draft[phase]
        cpp_us = cpp.median_ns / cpp.iterations / 1_000.0
        draft_us = other.median_ns / other.iterations / 1_000.0
        ratio = other.median_ns / cpp.median_ns if cpp.median_ns else float("inf")
        cpp_ns_byte = cpp.ns_per_byte_x1000 / 1_000.0
        draft_ns_byte = other.ns_per_byte_x1000 / 1_000.0
        byte_columns = (
            f"{cpp_ns_byte:10.3f} {draft_ns_byte:11.3f}"
            if cpp.bytes
            else f"{'-':>10} {'-':>11}"
        )
        print(
            f"{phase:<22} {cpp_us:12.3f} {draft_us:14.3f} "
            f"{ratio:10.3f} {byte_columns}"
        )
    print("\nDraft/C++ > 1 means Draft is slower; < 1 means Draft is faster.")
    print("The clock row is reported, not subtracted from phase measurements.")


def parse_arguments(repository: pathlib.Path) -> argparse.Namespace:
    """Resolve explicit developer inputs while keeping useful release defaults."""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir",
        type=pathlib.Path,
        default=repository / "build-release",
        help="CMake build directory containing both benchmark executables",
    )
    parser.add_argument(
        "--source",
        type=pathlib.Path,
        default=repository / "compiler/syntax/parser.draft",
        help="valid Draft source used by both implementations",
    )
    parser.add_argument(
        "--iterations",
        type=int,
        default=100,
        help="phase calls per sample (1..1000000)",
    )
    arguments = parser.parse_args()
    if arguments.iterations < 1 or arguments.iterations > 1_000_000:
        parser.error("--iterations must be in [1, 1000000]")
    return arguments


def main() -> int:
    """Run both children, validate their work identity, and print ratios."""

    repository = pathlib.Path(__file__).resolve().parent.parent
    arguments = parse_arguments(repository)
    build_directory = arguments.build_dir.resolve()
    source = arguments.source.resolve()
    suffix = ".exe" if sys.platform == "win32" else ""
    bootstrap_executable = (
        build_directory / f"draft-bootstrap-frontend-benchmark{suffix}"
    )
    draft_executable = build_directory / f"draft-frontend-benchmark{suffix}"

    for path in (bootstrap_executable, draft_executable, source):
        if not path.is_file():
            raise RuntimeError(f"required benchmark input does not exist: {path}")

    bootstrap = run_benchmark(
        bootstrap_executable, source, arguments.iterations, repository
    )
    draft = run_benchmark(draft_executable, source, arguments.iterations, repository)
    require_matching_work(bootstrap, draft)
    print_comparison(bootstrap, draft)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
