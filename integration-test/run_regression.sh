#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BIN_PATH="${REPO_ROOT}/bin/dynamatic"
REGRESSION_DIR="${SCRIPT_DIR}/regression"
FILTER_REGEX="${1:-}"

# Multi-transaction support:
# - Pass as 2nd positional argument, e.g.:
#     ./integration-test/run_regression.sh histogram 3
# - Or via env var TRANSACTIONS.
TRANSACTIONS_ARG="${2:-${TRANSACTIONS:-1}}"

# Under `set -u`, referencing an unset array is an error. Define EXCEPTION_DUTS
# as empty by default so is_exception_dut() is always safe.
EXCEPTION_DUTS=(${EXCEPTION_DUTS[@]+"${EXCEPTION_DUTS[@]}"})

# Exception list: DUTs in this list will be skipped.
# Matching is done on the DUT base directory name (e.g., "foo" for ".../foo").
EXCEPTION_DUTS=(
  complexdiv
  covariance
  covariance_float
  correlation_float
  dct
  external_integration
  test_constant_array
  test_internal_array
  cordic
  image_resize
  share_test_2
  test_memory_12
  test_memory_18
  mul_example
  while_loop_2
  test_bitint
)

is_exception_dut() {
  local dut_base="$1"
  local x
  for x in "${EXCEPTION_DUTS[@]}"; do
    if [[ "${dut_base}" == "${x}" ]]; then
      return 0
    fi
  done
  return 1
}


# Compile options: empty by default.
# Enable by exporting SCHEDCOV=1 or RVCOV=1.
COMPILE_OPTS=""
if [[ -n "${SCHEDCOV:-}" ]]; then
  COMPILE_OPTS="${COMPILE_OPTS} --sched-coverage"
fi
if [[ -n "${RVCOV:-}" ]]; then
  COMPILE_OPTS="${COMPILE_OPTS} --rv-coverage"
fi

mkdir -p "${REGRESSION_DIR}"
timestamp="$(date '+%Y%m%d-%H%M%S')"
RUN_DIR="${REGRESSION_DIR}/${timestamp}"
mkdir -p "${RUN_DIR}"

SCRIPT_LOG="${RUN_DIR}/${timestamp}.log"
exec > >(tee -a "${SCRIPT_LOG}") 2>&1

create_dyn_from_template() {
  local dut_name="$1"
  local source_rel_path="$2"
  local transactions="$3"
  local compile_line
  local temp_dyn
  temp_dyn="$(mktemp "${RUN_DIR}/${dut_name}.XXXXXX.dyn")"

  compile_line="compile${COMPILE_OPTS}"

  cat <<EOF >"${temp_dyn}"
set-src ${source_rel_path}
${compile_line}
write-hdl
simulate --transactions ${transactions}
exit
EOF

  echo "${temp_dyn}"
}

run_dut() {
  local dut_dir="$1"
  local dut_name="$2"
  local source_rel="$3"
  local transactions="$4"

  local temp_dyn
  temp_dyn="$(create_dyn_from_template "${dut_name}" "${source_rel}" "${transactions}")"

  local log_file="${RUN_DIR}/${dut_name}.log"
  local sim_report_path="${dut_dir}/out/sim/report.txt"

  echo "========================================"
  echo "[RUN ] ${dut_name}"
  echo "       src -> ${dut_dir}/${dut_name}.c"
  echo "       log -> ${log_file}"
  echo "       transactions -> ${transactions}"

  "${BIN_PATH}" --run "${temp_dyn}" >"${log_file}" 2>&1 || true

  local passed=1
  if grep -q "Simulation succeeded" "${log_file}"; then
    passed=0
    echo "[PASS] ${dut_name}"
  else
    echo "[FAIL] ${dut_name}"
  fi

  if [[ -f "${sim_report_path}" ]]; then
    {
      echo
      echo "===== SIMULATION REPORT (${sim_report_path}) ====="
      cat "${sim_report_path}"
    } >>"${log_file}"

    local covsum_lines
    covsum_lines=$(grep -i "covsum" "${sim_report_path}" || true)
    if [[ -n "${covsum_lines}" ]]; then
      echo "[COV ] ${dut_name} covsum logs:"
      while IFS= read -r line; do
        [[ -z "${line}" ]] && continue
        # Remove everything before the first occurrence of '[['
        line="${line#*[[}"
        line="[[${line}"
        echo "${line}"
      done <<< "${covsum_lines}"
    else
      echo "[COV ] ${dut_name}: no covsum logs found"
    fi
  else
    echo "[WARN] ${dut_name}: simulation report missing (${sim_report_path})" | tee -a "${log_file}"
  fi

  rm -f "${temp_dyn}"
  echo
  sync
  sleep 1

  return ${passed}
}

if [[ ! -x "${BIN_PATH}" ]]; then
  echo "[ERROR] ${BIN_PATH} not found or not executable." >&2
  exit 1
fi

all_dirs=( $(find "${SCRIPT_DIR}" -mindepth 1 -maxdepth 2 -type d \
  \( -path "*/regression" -o -path "*/regression/*" -o -name out \) -prune -o -type d -print | sort) )
# Only keep leaf directories (not parents of any other DUT dir)
DUT_DIRS=()
for d in "${all_dirs[@]}"; do
  is_leaf=1
  for other in "${all_dirs[@]}"; do
    if [[ "$d" != "$other" && "$other" == "$d"/* ]]; then
      is_leaf=0
      break
    fi
  done
  if [[ $is_leaf -eq 1 ]]; then
    DUT_DIRS+=("$d")
  fi
done

if [[ ${#DUT_DIRS[@]} -eq 0 ]]; then
  echo "[ERROR] No DUT directories found under ${SCRIPT_DIR}." >&2
  exit 1
fi

PASS=0
FAIL=0
SKIP=0
FAILED_TARGETS=()
SKIPPED_TARGETS=()

for dut_dir in "${DUT_DIRS[@]}"; do
  # Get relative path from SCRIPT_DIR for dut_dir
  rel_dut_path="${dut_dir#${SCRIPT_DIR}/}"
  dut_name="$(basename "${dut_dir}")"

  if [[ -n "${FILTER_REGEX}" && ! "${rel_dut_path}" =~ ${FILTER_REGEX} ]]; then
    continue
  fi

  if is_exception_dut "${dut_name}"; then
    echo "========================================"
    echo "[SKIP] ${rel_dut_path}: in exception list"
    echo -e "\n"
    ((SKIP += 1))
    SKIPPED_TARGETS+=("${rel_dut_path}")
    continue
  fi

  source_path="${dut_dir}/${dut_name}.c"
  if [[ ! -f "${source_path}" ]]; then
    echo "========================================"
    echo "[SKIP] ${rel_dut_path}: missing ${dut_name}.c"
    echo -e "\n"
    ((SKIP += 1))
    SKIPPED_TARGETS+=("${rel_dut_path}")
    continue
  fi

  source_rel="${source_path#${REPO_ROOT}/}"

  if run_dut "${dut_dir}" "${dut_name}" "${source_rel}" "${TRANSACTIONS_ARG}"; then
    ((PASS += 1))
  else
    ((FAIL += 1))
    FAILED_TARGETS+=("${rel_dut_path}")
  fi
done

TOTAL=$((PASS + FAIL + SKIP))

echo "========================================"
echo "Regression summary:"
echo "  Total targets : ${TOTAL}"
echo "  Passed        : ${PASS}"
echo "  Failed        : ${FAIL}"
echo "  Skipped       : ${SKIP}"

if [[ ${FAIL} -ne 0 ]]; then
  echo "  Failed targets:"
  for target in "${FAILED_TARGETS[@]}"; do
    echo "    - ${target}"
  done
fi
if [[ ${SKIP} -ne 0 ]]; then
  echo "  Skipped targets:"
  for target in "${SKIPPED_TARGETS[@]}"; do
    echo "    - ${target}"
  done
fi

echo "Logs stored in: ${RUN_DIR}"

if [[ ${FAIL} -ne 0 ]]; then
  exit -1
fi
