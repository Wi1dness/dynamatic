#!/usr/bin/env bash
set -euo pipefail

# Duplicate transaction 0 vector data into transactions 1..(N-1) for all
# transactional .dat files under INPUT_VECTORS/ and C_OUT/.
#
# This is useful for quickly running multi-transaction simulation without
# generating distinct vectors for each transaction.
#
# Usage:
#   duplicate-tx0-vectors.sh <SIM_DIR> <TRANSACTIONS>
# Example:
#   duplicate-tx0-vectors.sh integration-test/histogram/out/sim 3

if [[ $# -ne 2 ]]; then
  echo "Usage: $0 <SIM_DIR> <TRANSACTIONS>" >&2
  exit 2
fi

SIM_DIR="$1"
TXN="$2"

if [[ ! -d "$SIM_DIR" ]]; then
  echo "ERROR: directory not found: $SIM_DIR" >&2
  exit 1
fi

if ! [[ "$TXN" =~ ^[0-9]+$ ]] || [[ "$TXN" -lt 1 ]]; then
  echo "ERROR: TRANSACTIONS must be an integer >= 1" >&2
  exit 2
fi

if [[ "$TXN" -eq 1 ]]; then
  exit 0
fi

# Extract the [[transaction]] 0 payload from a .dat file (keeps original lines).
# Assumes HLS TB format:
#   [[[runtime]]]
#   [[transaction]] 0
#   ...
#   [[/transaction]]
#   [[transaction]] 1
#   ...
#   [[[ /runtime]]]
extract_tx0_payload() {
  local file="$1"
  awk '
    $1=="[[transaction]]" && $2=="0" {in0=1; next}
    $1=="[[/transaction]]" && in0==1 {exit}
    in0==1 {print}
  ' "$file"
}

rewrite_dir() {
  local dir="$1"
  [[ -d "$dir" ]] || return 0

  for f in "$dir"/*.dat; do
    [[ -f "$f" ]] || continue

    if ! grep -q "\[\[transaction\]\] 0" "$f"; then
      # Not a transactional file, skip.
      continue
    fi

    payload="$(extract_tx0_payload "$f")"

    tmp="${f}.tmp"
    {
      echo "[[[runtime]]]"
      for ((i=0; i<TXN; i++)); do
        echo "[[transaction]] $i"
        if [[ -n "$payload" ]]; then
          printf '%s\n' "$payload"
        fi
        echo "[[/transaction]]"
      done
      echo "[[[/runtime]]]"
    } > "$tmp"

    mv "$tmp" "$f"

  done
}

rewrite_dir "$SIM_DIR/INPUT_VECTORS"
rewrite_dir "$SIM_DIR/C_OUT"
