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

echo "krep CLI v3 integration tests passed"
