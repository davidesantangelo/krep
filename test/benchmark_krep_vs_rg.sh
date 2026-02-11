#!/usr/bin/env bash
set -euo pipefail

DATASET_URL="https://burntsushi.net/stuff/subtitles2016-sample.en.gz"
DATASET_GZ="subtitles2016-sample.en.gz"
DATASET_TXT="subtitles2016-sample.en"
RUNS="${RUNS:-5}"
PATTERN="${1:-the}"
KREP_BIN="${KREP_BIN:-./krep}"

measure_cmd_avg() {
  local runs="$1"
  shift

  local sum="0"
  local real
  local i

  for ((i = 1; i <= runs; i++)); do
    real="$({ /usr/bin/time -p "$@" >/dev/null; } 2>&1 | awk '/^real / { print $2 }')"
    if [[ -z "${real}" ]]; then
      echo "Failed to measure command: $*" >&2
      return 1
    fi
    sum="$(awk -v a="${sum}" -v b="${real}" 'BEGIN { printf "%.6f", a + b }')"
  done

  awk -v s="${sum}" -v n="${runs}" 'BEGIN { printf "%.6f", s / n }'
}

if ! command -v rg >/dev/null 2>&1; then
  echo "ripgrep (rg) is not installed or not in PATH." >&2
  exit 1
fi

if [[ ! -x "${KREP_BIN}" ]]; then
  echo "krep binary not found or not executable at: ${KREP_BIN}" >&2
  echo "Run 'make' first, or set KREP_BIN to the binary path." >&2
  exit 1
fi

if [[ ! "${RUNS}" =~ ^[0-9]+$ ]] || [[ "${RUNS}" -le 0 ]]; then
  echo "RUNS must be a positive integer (current: ${RUNS})." >&2
  exit 1
fi

if [[ ! -f "${DATASET_GZ}" ]]; then
  echo "Downloading ${DATASET_GZ}..."
  curl -LO "${DATASET_URL}"
fi

if [[ ! -f "${DATASET_TXT}" ]]; then
  echo "Decompressing ${DATASET_GZ}..."
  gzip -dk "${DATASET_GZ}"
fi

if [[ ! -f "${DATASET_TXT}" ]]; then
  echo "Dataset file ${DATASET_TXT} is missing after download/decompression." >&2
  exit 1
fi

# Warm cache.
"${KREP_BIN}" -c -F -- "${PATTERN}" "${DATASET_TXT}" >/dev/null
rg -c -F -- "${PATTERN}" "${DATASET_TXT}" >/dev/null

krep_count="$("${KREP_BIN}" -c -F -- "${PATTERN}" "${DATASET_TXT}" | awk -F: '{print $NF}')"
rg_count="$(rg -c -F -- "${PATTERN}" "${DATASET_TXT}" | awk -F: '{print $NF}')"

if [[ "${krep_count}" != "${rg_count}" ]]; then
  echo "Count mismatch detected (krep=${krep_count}, rg=${rg_count})." >&2
  exit 1
fi

krep_avg="$(measure_cmd_avg "${RUNS}" "${KREP_BIN}" -c -F -- "${PATTERN}" "${DATASET_TXT}")"
rg_avg="$(measure_cmd_avg "${RUNS}" rg -c -F -- "${PATTERN}" "${DATASET_TXT}")"

speedup="$(awk -v k="${krep_avg}" -v r="${rg_avg}" 'BEGIN { if (k > 0) printf "%.2f", r / k; else print "inf" }')"

printf "\nBenchmark (cached)\n"
printf "Pattern: %s\n" "${PATTERN}"
printf "Dataset: %s\n" "${DATASET_TXT}"
printf "Runs: %s\n\n" "${RUNS}"
printf "%-10s %12s\n" "Tool" "Avg real (s)"
printf "%-10s %12s\n" "krep" "${krep_avg}"
printf "%-10s %12s\n" "ripgrep" "${rg_avg}"
printf "\nSpeedup (ripgrep/krep): %sx\n" "${speedup}"
printf "Validated count: %s\n" "${krep_count}"
