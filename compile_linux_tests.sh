#!/usr/bin/env bash
set -euo pipefail

project_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
mode=default
core_only=false

usage()
{
    cat <<'HELP'
Usage: ./compile_linux_tests.sh [--debug | --release | --both] [--core-only]

Build and run the Hydrology C++ tests with Ninja and CTest.

  (no option)  RelWithDebInfo build in build-linux-tests.
  --debug      Debug build and tests in build-linux-tests-debug.
  --release    Release build and tests in build-linux-tests-release.
  --both       Independently build and run Debug and Release test suites.
  --core-only  Build/test only the Qt-independent analysis library, without
               requiring Qt 6 to be installed. Otherwise, the REST executable
               is built too, checking its compile-time integration.
  --help       Show this help.

Use --core-only in a minimal development environment; omit it to verify the
actual Qt executable can compile before deploying.
HELP
}

while (( $# )); do
    case "$1" in
        --debug|--release|--both)
            if [[ "$mode" != default ]]; then
                echo "Choose only one build configuration." >&2
                exit 2
            fi
            mode="${1#--}"
            ;;
        --core-only) core_only=true ;;
        --help|-h) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

for tool in cmake c++ ninja ctest; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Missing build tool: $tool" >&2
        exit 1
    fi
done

run_suite()
{
    local build_directory="$1"
    local build_type="$2"
    local server_option=ON
    if [[ "$core_only" == true ]]; then
        server_option=OFF
    fi

    if [[ -f "$build_directory/CMakeCache.txt" ]]; then
        local cached_generator
        cached_generator="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$build_directory/CMakeCache.txt" | head -n 1)"
        if [[ -n "$cached_generator" && "$cached_generator" != Ninja ]]; then
            echo "Existing test build uses $cached_generator; recreating for Ninja."
            rm -rf -- "$build_directory"
        fi
    fi

    echo "=== Hydrology tests: $build_type (REST server build: $server_option) ==="
    cmake -S "$project_directory" -B "$build_directory" -G Ninja \
        -DCMAKE_BUILD_TYPE="$build_type" \
        -DBUILD_TESTING=ON \
        -DAOWIS_HYDROLOGY_BUILD_SERVER="$server_option" \
        -DAOWIS_HYDROLOGY_BUILD_SERVICE="$server_option"
    cmake --build "$build_directory" --parallel

    # CTest can otherwise exit successfully when zero tests are registered.
    local registered
    registered="$(ctest --test-dir "$build_directory" -N -L '^hydrology$')"
    if ! grep -Eq 'Total Tests: [1-9][0-9]*' <<< "$registered"; then
        echo "Error: no Hydrology tests registered in $build_directory" >&2
        echo "$registered" >&2
        return 1
    fi
    ctest --test-dir "$build_directory" --output-on-failure -L '^hydrology$'
}

case "$mode" in
    default) run_suite "$project_directory/build-linux-tests" RelWithDebInfo ;;
    debug) run_suite "$project_directory/build-linux-tests-debug" Debug ;;
    release) run_suite "$project_directory/build-linux-tests-release" Release ;;
    both)
        run_suite "$project_directory/build-linux-tests-debug" Debug
        run_suite "$project_directory/build-linux-tests-release" Release
        ;;
esac
