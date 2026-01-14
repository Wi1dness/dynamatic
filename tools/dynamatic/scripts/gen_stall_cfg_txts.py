#!/usr/bin/env python3

import argparse
import hashlib
import json
import random
from pathlib import Path


def read_ids_and_cap(json_path: Path) -> tuple[list[int], int | None]:
    try:
        with json_path.open("r", encoding="utf-8") as f:
            obj = json.load(f)
    except FileNotFoundError:
        return ([], None)
    except Exception:
        return ([], None)

    cap = obj.get("max_block_cycles")
    if isinstance(cap, int) and cap > 0:
        max_block_cycles: int | None = cap
    else:
        max_block_cycles = None

    pts = obj.get("stall_points")
    if not isinstance(pts, list):
        return ([], max_block_cycles)

    ids: set[int] = set()
    for elt in pts:
        if not isinstance(elt, dict):
            continue
        v = elt.get("id")
        if isinstance(v, int) and v >= 0:
            ids.add(v & 0xFFFFFFFF)
    return (sorted(ids), max_block_cycles)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Generate per-transaction stall cfg files (text hex) for VHDL TB."
    )
    ap.add_argument(
        "--output",
        required=True,
        help="Output directory (typically <sim>/INPUT_VECTORS)",
    )
    ap.add_argument("--json", required=True, help="Path to stall_points.json")
    ap.add_argument(
        "--kernel",
        required=True,
        help="Kernel name (used to seed RNG for reproducibility)",
    )
    ap.add_argument(
        "--rng-seed",
        default=None,
        help="Optional RNG seed override (string). Default: derived from --kernel.",
    )
    ap.add_argument("--transactions", type=int, default=1)
    ap.add_argument(
        "--base-min", type=int, default=0, help="Min base stall length (default: 0)"
    )
    ap.add_argument(
        "--base-max", type=int, default=16, help="Max base stall length (default: 16)"
    )
    ap.add_argument(
        "--threshold-min",
        type=lambda s: int(s, 0),
        default=0,
        help="Min threshold (uint32, default: 0)",
    )
    ap.add_argument(
        "--threshold-max",
        type=lambda s: int(s, 0),
        default=0xFFFFFFFF,
        help="Max threshold (uint32, default: 0xFFFFFFFF)",
    )
    args = ap.parse_args()

    out_dir = Path(args.output)
    out_dir.mkdir(parents=True, exist_ok=True)

    json_path = Path(args.json)
    ids, max_block_cycles = read_ids_and_cap(json_path)
    if not ids:
        # No JSON or no ids: emit a single non-matching id word so cfg_done can
        # still pulse.
        ids = [0xFFFFFFFF]

    base_min = max(0, int(args.base_min))
    base_max = max(base_min, int(args.base_max))

    # Cap base_max using the design-derived "max_block_cycles" (if present).
    # If base_min already exceeds the cap, keep the user's base_min/base_max
    # (i.e., don't force a clamp), since in that configuration the user is
    # explicitly requesting larger stalls.
    if max_block_cycles is not None:
        cap = max(0, int(max_block_cycles))
        if cap < base_min:
            base_max = base_min
        else:
            base_max = min(base_max, cap)

    thr_min = int(args.threshold_min) & 0xFFFFFFFF
    thr_max = int(args.threshold_max) & 0xFFFFFFFF
    if thr_min > thr_max:
        thr_min, thr_max = thr_max, thr_min

    txn_n = max(1, int(args.transactions))

    # Deterministic RNG seeding: by default derive from kernel name so cfg files
    # are reproducible across runs for the same kernel.
    seed_material = args.rng_seed if args.rng_seed is not None else str(args.kernel)
    seed_int = int.from_bytes(
        hashlib.sha256(seed_material.encode("utf-8")).digest()[:8], "little"
    )
    rng = random.Random(seed_int)

    for t in range(txn_n):
        p = out_dir / f"cfg_tx{t}.txt"
        with p.open("w", encoding="ascii") as f:
            for sid in ids:
                # Seed must be nonzero (otherwise the LFSR would get stuck).
                seed = rng.getrandbits(32) & 0xFFFFFFFF
                while seed == 0:
                    seed = rng.getrandbits(32) & 0xFFFFFFFF

                threshold = rng.randint(thr_min, thr_max) & 0xFFFFFFFF
                base = rng.randint(base_min, base_max) & 0xFFFFFFFF

                # 128-bit word, high->low 32-bit chunks: id, base, threshold, seed
                # Emit with a 0x prefix to match the HLS TB text conventions.
                word = f"0x{sid:08X}{base:08X}{threshold:08X}{seed:08X}"
                f.write(word + "\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
