#!/usr/bin/env bash
set -euo pipefail

project_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
build_directory="$project_directory/build-linux"
install_dependencies=false
run_after_build=true
application_arguments=()

usage()
{
    cat <<'HELP'
Usage: ./compile_linux.sh [--install-dependencies] [--no-run] [-- <server arguments>]

Build the Qt 6 REST executable in build-linux using Ninja, then run it unless
--no-run was selected. Pass command-line options to the server after --.

  --install-dependencies   Install CMake, Ninja, C++ and Qt 6 development
                           packages on Debian-family systems (requires root/sudo).
  --no-run                 Build but do not start the server.
  --help                   Show this help.

Environment:
  AOWIS_BUILD_TYPE         CMake build type; defaults to RelWithDebInfo.

Examples:
  ./compile_linux.sh --no-run
  ./compile_linux.sh -- --print-default-config
  ./compile_linux.sh -- --config ./hydrology.ini --check-config
HELP
}

is_debian_family()
{
    if [[ ! -r /etc/os-release ]]; then
        return 1
    fi
    local ID="" ID_LIKE=""
    # shellcheck disable=SC1091
    source /etc/os-release
    [[ "${ID:-}" == "debian" || " ${ID_LIKE:-} " == *" debian "* ]]
}

install_debian_dependencies()
{
    if ! is_debian_family || ! command -v apt-get >/dev/null 2>&1; then
        echo "Error: --install-dependencies requires Debian or a Debian-based system with apt-get." >&2
        exit 1
    fi
    local packages=(build-essential cmake ninja-build qt6-base-dev qt6-httpserver-dev libgdal-dev)
    if (( EUID == 0 )); then
        apt-get update
        apt-get install -y "${packages[@]}"
    else
        if ! command -v sudo >/dev/null 2>&1; then
            echo "Error: sudo or root privileges are required to install dependencies." >&2
            exit 1
        fi
        sudo apt-get update
        sudo apt-get install -y "${packages[@]}"
    fi
}

while (( $# )); do
    case "$1" in
        --install-dependencies) install_dependencies=true ;;
        --no-run) run_after_build=false ;;
        --help|-h) usage; exit 0 ;;
        --)
            shift
            application_arguments+=("$@")
            break
            ;;
        *)
            echo "Unknown build option: $1 (use -- before server arguments)" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

if [[ "$install_dependencies" == true ]]; then
    install_debian_dependencies
fi
for tool in cmake c++ ninja; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Missing build tool: $tool" >&2
        echo "Install the dependencies or run ./compile_linux.sh --install-dependencies --no-run." >&2
        exit 1
    fi
done

if [[ -f "$build_directory/CMakeCache.txt" ]]; then
    cached_generator="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$build_directory/CMakeCache.txt" | head -n 1)"
    if [[ -n "$cached_generator" && "$cached_generator" != Ninja ]]; then
        echo "Existing build directory uses $cached_generator; recreating for Ninja."
        rm -rf -- "$build_directory"
    fi
fi

cmake -S "$project_directory" -B "$build_directory" -G Ninja \
    -DCMAKE_BUILD_TYPE="${AOWIS_BUILD_TYPE:-RelWithDebInfo}" \
    -DAOWIS_HYDROLOGY_BUILD_SERVER=ON \
    -DAOWIS_HYDROLOGY_BUILD_SERVICE=ON
cmake --build "$build_directory" --parallel

executable="$build_directory/aowis-server-hydrology"
if [[ ! -x "$executable" ]]; then
    echo "Build succeeded but $executable was not created." >&2
    exit 1
fi
if [[ "$run_after_build" == false ]]; then
    echo "Build completed: $executable"
    exit 0
fi
exec "$executable" "${application_arguments[@]}"
