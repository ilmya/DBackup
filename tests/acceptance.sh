#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -W)"
repo_root="$(cd -- "${script_dir}/.." && pwd -W)"
command_name="${1:-help}"
if (($#)); then shift; fi

if command -v py.exe >/dev/null 2>&1; then
  python_cmd=(py.exe -3)
elif command -v python.exe >/dev/null 2>&1; then
  python_cmd=(python.exe)
elif command -v python3 >/dev/null 2>&1; then
  python_cmd=(python3)
else
  echo "Error: Python 3 was not found. Install Python 3 and add it to PATH." >&2
  exit 2
fi

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
  remote-e2e)
    cli="${repo_root}/out/build-test/BackupToolTestCli.exe"
    server="${repo_root}/out/build-test/DBackupServer.exe"
    openssl="$(command -v openssl.exe || command -v openssl || true)"
    if [[ ! -f "${cli}" || ! -f "${server}" ]]; then
      echo "Build the development test programs first with: cmd.exe /c build.bat test" >&2
      exit 2
    fi
    if [[ -z "${openssl}" ]]; then
      echo "OpenSSL was not found; add openssl.exe to PATH." >&2
      exit 2
    fi
    if command -v cygpath >/dev/null 2>&1; then
      openssl="$(cygpath -w "${openssl}")"
    fi
    exec "${python_cmd[@]}" "${script_dir}/acceptance_network_e2e.py" \
      --server-exe "${server}" --test-cli "${cli}" --openssl "${openssl}" "$@"
    ;;
  server-smoke)
    cli="${repo_root}/out/build-test/BackupToolTestCli.exe"
    if [[ ! -f "${cli}" ]]; then
      echo "Build the development test programs first with: cmd.exe /c build.bat test" >&2
      exit 2
    fi
    exec "${cli}" remote-client-test "$@"
    ;;
  test-cli)
    cli="${repo_root}/out/build-test/BackupToolTestCli.exe"
    if [[ ! -f "${cli}" ]]; then
      echo "Build the development test CLI first with: cmd.exe /c build.bat test" >&2
      exit 2
    fi
    exec "${cli}" "$@"
    ;;
  help|-h|--help)
    cat <<'HELP'
DBackup development acceptance commands (run from any directory in Git Bash):
  tests/acceptance.sh generate --root D:/DBackup-Acceptance [--profile standard|stress] [--reset]
  tests/acceptance.sh self-test
  tests/acceptance.sh validate-catalog
  tests/acceptance.sh run --case S4-REMOTE-001
  tests/acceptance.sh run --all
  tests/acceptance.sh test-cli --help
  tests/acceptance.sh fault-copy --root ROOT --kind archive-truncate --input FILE --output FILE
  tests/acceptance.sh hold-file --root ROOT --path FILE
  tests/acceptance.sh realtime-events --root ROOT
  tests/acceptance.sh remote-e2e --root ROOT
  tests/acceptance.sh server-smoke --root ROOT --server https://127.0.0.1:8443

Other established commands are forwarded to tools/acceptance/acceptance.sh, including
compare, manifest, prepare-links, prepare-permissions, inspect-repository, and cleanup.
HELP
    ;;
  *)
    exec "${BASH}" "${repo_root}/tools/acceptance/acceptance.sh" "${command_name}" "$@"
    ;;
esac
