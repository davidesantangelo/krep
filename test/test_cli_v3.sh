#!/usr/bin/env bash
set -euo pipefail

KREP_BIN="${KREP_BIN:-./krep}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/krep_cli_v3.XXXXXX")"
trap 'rm -rf "${TMP_DIR}"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

assert_contains() {
  local haystack="$1"
  local needle="$2"
  local label="$3"

  [[ "${haystack}" == *"${needle}"* ]] || fail "${label}: expected to contain '${needle}', got: ${haystack}"
}

assert_not_contains() {
  local haystack="$1"
  local needle="$2"
  local label="$3"

  [[ "${haystack}" != *"${needle}"* ]] || fail "${label}: expected not to contain '${needle}', got: ${haystack}"
}

assert_empty_file() {
  local path="$1"
  local label="$2"

  [[ ! -s "${path}" ]] || fail "${label}: expected empty file, got: $(cat "${path}")"
}

run_with_timeout() {
  local timeout_seconds="$1"
  shift
  local timeout_marker="${TMP_DIR}/command-timed-out"

  rm -f "${timeout_marker}"
  "$@" &
  local command_pid=$!

  (
    sleep "${timeout_seconds}"
    if kill -0 "${command_pid}" 2>/dev/null; then
      : >"${timeout_marker}"
      kill -TERM "${command_pid}" 2>/dev/null || true
      sleep 1
      kill -KILL "${command_pid}" 2>/dev/null || true
    fi
  ) &
  local watchdog_pid=$!
  local command_status=0

  wait "${command_pid}" || command_status=$?
  kill "${watchdog_pid}" 2>/dev/null || true
  wait "${watchdog_pid}" 2>/dev/null || true

  if [[ -f "${timeout_marker}" ]]; then
    return 124
  fi

  return "${command_status}"
}

mkdir -p "${TMP_DIR}/src" "${TMP_DIR}/vendor" "${TMP_DIR}/.hidden"

cat >"${TMP_DIR}/app.txt" <<'EOF'
alpha
beta needle
gamma
delta needle
EOF

cat >"${TMP_DIR}/src/main.c" <<'EOF'
int main(void) {
  /* TODO: wire feature */
  return 0;
}
EOF

cat >"${TMP_DIR}/src/clean.c" <<'EOF'
int clean(void) {
  return 0;
}
EOF

cat >"${TMP_DIR}/src/readme.md" <<'EOF'
TODO in docs
EOF

cat >"${TMP_DIR}/vendor/skip.c" <<'EOF'
/* TODO: vendored */
EOF

cat >"${TMP_DIR}/.hidden/secret.c" <<'EOF'
/* TODO: hidden */
EOF

line_output="$("${KREP_BIN}" -n needle "${TMP_DIR}/app.txt")"
assert_contains "${line_output}" "${TMP_DIR}/app.txt:2:beta needle" "-n line number output"
assert_contains "${line_output}" "${TMP_DIR}/app.txt:4:delta needle" "-n second line number output"

context_output="$("${KREP_BIN}" -C 1 needle "${TMP_DIR}/app.txt")"
assert_contains "${context_output}" "${TMP_DIR}/app.txt-1-alpha" "-C before context"
assert_contains "${context_output}" "${TMP_DIR}/app.txt:2:beta needle" "-C match line"
assert_contains "${context_output}" "${TMP_DIR}/app.txt-3-gamma" "-C after context"

json_output="$("${KREP_BIN}" --json needle "${TMP_DIR}/app.txt")"
assert_contains "${json_output}" '"type":"line"' "--json line type"
assert_contains "${json_output}" '"line_number":2' "--json line number"
assert_contains "${json_output}" '"text":"beta needle"' "--json text"

json_match_output="$("${KREP_BIN}" --json -o needle "${TMP_DIR}/app.txt")"
assert_contains "${json_match_output}" '"type":"match"' "--json -o match type"
assert_contains "${json_match_output}" '"match":"needle"' "--json -o match text"

glob_output="$("${KREP_BIN}" -r --glob '*.c' --exclude '*/vendor/*' -l TODO "${TMP_DIR}")"
assert_contains "${glob_output}" "${TMP_DIR}/src/main.c" "--glob includes C file"
assert_not_contains "${glob_output}" "readme.md" "--glob excludes non-C file"
assert_not_contains "${glob_output}" "vendor/skip.c" "--exclude skips vendor path"
assert_not_contains "${glob_output}" ".hidden/secret.c" "hidden directory skipped by default"

hidden_output="$("${KREP_BIN}" -r --hidden --glob '*.c' -l TODO "${TMP_DIR}")"
assert_contains "${hidden_output}" "${TMP_DIR}/.hidden/secret.c" "--hidden includes hidden directory"

without_output="$("${KREP_BIN}" -r --glob '*.c' --exclude '*/vendor/*' -L TODO "${TMP_DIR}")"
assert_contains "${without_output}" "${TMP_DIR}/src/clean.c" "-L files without matches"
assert_not_contains "${without_output}" "${TMP_DIR}/src/main.c" "-L omits matching files"

quiet_out="${TMP_DIR}/quiet.out"
"${KREP_BIN}" -q needle "${TMP_DIR}/app.txt" >"${quiet_out}"
assert_empty_file "${quiet_out}" "-q output"

stats_out="${TMP_DIR}/stats.out"
stats_err="${TMP_DIR}/stats.err"
"${KREP_BIN}" --stats -c needle "${TMP_DIR}/app.txt" >"${stats_out}" 2>"${stats_err}"
assert_contains "$(cat "${stats_out}")" "${TMP_DIR}/app.txt:2" "--stats keeps normal stdout"
assert_contains "$(cat "${stats_err}")" "krep stats:" "--stats emits stderr summary"

zero_width_out="${TMP_DIR}/zero-width.out"
zero_width_err="${TMP_DIR}/zero-width.err"
zero_width_status=0
run_with_timeout 3 "${KREP_BIN}" -E '$' "${TMP_DIR}/app.txt" >"${zero_width_out}" 2>"${zero_width_err}" || zero_width_status=$?
[[ "${zero_width_status}" -ne 124 ]] || fail "zero-width regex at line end timed out"
[[ "${zero_width_status}" -eq 0 ]] || fail "zero-width regex at line end exited with ${zero_width_status}: $(cat "${zero_width_err}")"
assert_contains "$(cat "${zero_width_out}")" "${TMP_DIR}/app.txt:alpha" "zero-width regex prints the first line once"
assert_contains "$(cat "${zero_width_out}")" "${TMP_DIR}/app.txt:delta needle" "zero-width regex reaches the final line"

empty_regex_out="${TMP_DIR}/empty-regex.out"
empty_regex_err="${TMP_DIR}/empty-regex.err"
empty_regex_status=0
run_with_timeout 3 "${KREP_BIN}" -E '' "${TMP_DIR}/app.txt" >"${empty_regex_out}" 2>"${empty_regex_err}" || empty_regex_status=$?
[[ "${empty_regex_status}" -ne 124 ]] || fail "empty regex timed out"
if [[ "${empty_regex_status}" -eq 0 ]]; then
  assert_contains "$(cat "${empty_regex_out}")" "${TMP_DIR}/app.txt:alpha" "empty regex prints matching lines"
elif [[ "${empty_regex_status}" -eq 2 ]]; then
  assert_contains "$(cat "${empty_regex_err}")" "Regex compilation error" "platform regex engine rejects empty regex cleanly"
else
  fail "empty regex exited with unexpected status ${empty_regex_status}: $(cat "${empty_regex_err}")"
fi

# Regression tests for issue #44 heap buffer overflow / over-read fixes
cat >"${TMP_DIR}/dup_patterns.txt" <<'EOF'
AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
EOF
python3 -c 'print("A" * 64 * 10)' >"${TMP_DIR}/dup_text.txt"
dup_out="${TMP_DIR}/dup.out"
run_with_timeout 5 "${KREP_BIN}" -f "${TMP_DIR}/dup_patterns.txt" "${TMP_DIR}/dup_text.txt" >"${dup_out}"
[[ -s "${dup_out}" ]] || fail "duplicate pattern matching produced empty output"

cat >"${TMP_DIR}/len10_pattern.txt" <<'EOF'
0123456789
EOF
echo "prefix 0123456789 suffix" >"${TMP_DIR}/len10_text.txt"
len10_out="${TMP_DIR}/len10.out"
run_with_timeout 3 "${KREP_BIN}" -f "${TMP_DIR}/len10_pattern.txt" "${TMP_DIR}/len10_text.txt" >"${len10_out}"
assert_contains "$(cat "${len10_out}")" "0123456789" "short pattern loaded from file matches correctly"

mkdir -p "${TMP_DIR}/repo"
echo "ignored.txt" >"${TMP_DIR}/repo/.gitignore"
echo "secret" >"${TMP_DIR}/repo/ignored.txt"
echo "secret" >"${TMP_DIR}/repo/kept.txt"
gi_out="$("${KREP_BIN}" -r --gitignore secret "${TMP_DIR}/repo")"
assert_contains "${gi_out}" "kept.txt" "--gitignore keeps non-ignored file"
assert_not_contains "${gi_out}" "ignored.txt" "--gitignore skips ignored file"

echo "krep CLI v3 integration tests passed"
