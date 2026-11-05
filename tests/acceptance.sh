#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/.." && pwd)"
command_name="${1:-help}"
if (($#)); then shift; fi

python_cmd=(python3)

export PYTHONUTF8=1
if [[ -z "${QT_PLUGIN_PATH:-}" && -f "${repo_root}/out/build-test/qt-plugin-dir.txt" ]]; then
  QT_PLUGIN_PATH="$(<"${repo_root}/out/build-test/qt-plugin-dir.txt")"
  export QT_PLUGIN_PATH
fi
case "${command_name}" in
  generate)
    exec "${python_cmd[@]}" "${script_dir}/prepare_acceptance_data.py" "$@"
    ;;
  fault-copy|hold-file|realtime-events)
    exec "${python_cmd[@]}" "${script_dir}/acceptance_support.py" "${command_name}" "$@"
    ;;
  self-test)
    test_tmp="${repo_root}/out/build-test/acceptance-e2e/selftest-temp"
    mkdir -p "${test_tmp}"
    DBACKUP_ACCEPTANCE_TEST_TMP="${test_tmp}" \
      exec "${python_cmd[@]}" "${script_dir}/run_acceptance_selftest.py"
    ;;
  validate-catalog)
    exec "${python_cmd[@]}" "${script_dir}/validate_acceptance_catalog.py"
    ;;
  run)
    exec "${python_cmd[@]}" "${script_dir}/run_acceptance_case.py" "$@"
    ;;
  compare)
    exec "${python_cmd[@]}" "${script_dir}/acceptance_compare.py" "$@"
    ;;
  test-cli)
    cli="${repo_root}/out/build-test/BackupToolTestCli"
    if [[ ! -f "${cli}" ]]; then
      echo "Build the development test CLI first with: ./build.sh test" >&2
      exit 2
    fi
    exec "${cli}" "$@"
    ;;
  help|-h|--help)
    cat <<'HELP'
DBackup development acceptance commands (run from any directory in Linux):
  tests/acceptance.sh generate --root /tmp/DBackup-Acceptance [--profile standard|stress] [--reset]
  tests/acceptance.sh self-test
  tests/acceptance.sh validate-catalog
  tests/acceptance.sh run --case S1-PACK-001
  tests/acceptance.sh run --all
  tests/acceptance.sh test-cli --help
  tests/acceptance.sh fault-copy --root ROOT --kind archive-truncate --input FILE --output FILE
  tests/acceptance.sh hold-file --root ROOT --path FILE
  tests/acceptance.sh realtime-events --root ROOT

Other established commands are forwarded to tools/acceptance/acceptance.sh, including
compare, manifest, prepare-links, prepare-permissions, inspect-repository, and cleanup.
HELP
    ;;
  *)
    exec "${BASH}" "${repo_root}/tools/acceptance/acceptance.sh" "${command_name}" "$@"
    ;;
esac
