#!/usr/bin/env python3

import argparse
import csv
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


_COV_HEADER_RE = re.compile(r"^\[COV\s*\]\s+(?P<dut>\S+)\s+covsum\s+logs:\s*$")
# Handles both:
#   [[Transaction 0]] CovSum=0x000001C5
#   # ** Note: [[Transaction 0]] CovSum=0x000001C5
_COV_LINE_RE = re.compile(
    r"^(?:#\s*\*\*\s*Note:\s*)?\[\[Transaction\s+(?P<tx>\d+)\]\]\s+CovSum=0x(?P<hex>[0-9A-Fa-f]+)\s*$"
)


@dataclass(frozen=True)
class TxCovSum:
    tx: int
    covsum: int


def parse_covsum_from_log(log_path: Path) -> Dict[str, List[TxCovSum]]:
    """Parse covsum points from a run_regression.sh aggregated log.

    Expected shape in log:
      [COV ] <dut> covsum logs:
      [[Transaction 0]] CovSum=0x...
      ...

        Returns:
            dut -> sorted list of TxCovSum(tx, covsum)
    """

    per_dut: Dict[str, Dict[int, int]] = {}
    current_dut: Optional[str] = None

    with log_path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")

            m = _COV_HEADER_RE.match(line)
            if m:
                current_dut = m.group("dut")
                per_dut.setdefault(current_dut, {})
                continue

            m = _COV_LINE_RE.match(line)
            if m and current_dut is not None:
                tx = int(m.group("tx"))
                covsum = int(m.group("hex"), 16)
                per_dut[current_dut][tx] = covsum
                continue

            # Any other line: ignore.

    result: Dict[str, List[TxCovSum]] = {}
    for dut, tx_map in per_dut.items():
        pts = [TxCovSum(tx=tx, covsum=covsum) for tx, covsum in tx_map.items()]
        pts.sort(key=lambda p: p.tx)
        result[dut] = pts

    return result


def write_csv(out_dir: Path, dut: str, pts: List[TxCovSum]) -> Path:
    out_path = out_dir / f"{dut}_covsum.csv"
    with out_path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["transaction", "covsum_dec", "covsum_hex"])
        for p in pts:
            w.writerow([p.tx, p.covsum, f"0x{p.covsum:08X}"])
    return out_path


def write_png(out_dir: Path, dut: str, pts: List[TxCovSum]) -> Optional[Path]:
    if not pts:
        return None

    xs = [p.tx for p in pts]
    ys = [p.covsum for p in pts]

    fig = plt.figure(figsize=(10, 4))
    ax = fig.add_subplot(1, 1, 1)
    ax.plot(xs, ys, linewidth=1)
    ax.set_title(f"{dut}: CovSum vs Round")
    ax.set_xlabel("Round")
    ax.set_ylabel("CovSum")
    ax.grid(True, which="both", linestyle=":", linewidth=0.5)

    out_path = out_dir / f"{dut}_covsum.png"
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    return out_path


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(
        description=(
            "Parse run_regression aggregated logs and plot CovSum vs transaction for each DUT. "
            "Always emits CSV; emits PNG if matplotlib is available."
        )
    )
    ap.add_argument("--log", required=True, help="Path to regression aggregated .log")
    ap.add_argument(
        "--out",
        required=True,
        help="Output directory for CSV/PNG artifacts (created if missing)",
    )
    args = ap.parse_args(argv)

    log_path = Path(args.log)
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    per_dut = parse_covsum_from_log(log_path)

    wrote_any = False
    for dut, pts in sorted(per_dut.items()):
        if not pts:
            continue
        write_csv(out_dir, dut, pts)
        write_png(out_dir, dut, pts)
        wrote_any = True

    if not wrote_any:
        sys.stderr.write(f"No covsum data found in log: {log_path}\n")
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
