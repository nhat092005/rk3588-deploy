#!/usr/bin/env bash
#
# Run every C++ test of deploy/ on the board: <board root>/deploy/bin/test/*/test_*.
#
# Usage: bash <board root>/deploy/sh/env/run_tests.sh [model.rknn]
# test_npu_infer needs a model: it runs when the file exists and is skipped otherwise.
# Prints one line per test and a summary, exit code 1 when a test fails.

set -uo pipefail

BIN_DIR="$(cd "$(dirname "$0")/../../bin" && pwd)"
readonly BIN_DIR

main() {
  local model="${1:-}"
  local passed=0 failed=0 skipped=0
  local test_path name

  cd "${BIN_DIR}" || exit 1
  while read -r test_path; do
    name="$(basename "${test_path}")"
    if [[ "${name}" == "test_npu_infer" ]]; then
      if [[ ! -f "${model}" ]]; then
        echo "skip ${test_path} (no model)"
        skipped=$((skipped + 1))
        continue
      fi
      if "./${test_path}" "${model}" >/dev/null 2>&1; then
        echo "ok   ${test_path}"
        passed=$((passed + 1))
      else
        echo "FAIL ${test_path}"
        failed=$((failed + 1))
      fi
      continue
    fi
    if "./${test_path}" >/dev/null 2>&1; then
      echo "ok   ${test_path}"
      passed=$((passed + 1))
    else
      echo "FAIL ${test_path}"
      failed=$((failed + 1))
    fi
  done < <(find test -type f -name 'test_*' | sort)

  echo "C++ tests: ${passed} passed, ${failed} failed, ${skipped} skipped"
  [[ "${failed}" -eq 0 ]]
}

main "$@"
