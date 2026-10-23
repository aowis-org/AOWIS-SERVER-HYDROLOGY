#!/usr/bin/env bash
set -euo pipefail
# Install a reproducible, vendored copy of cpp-httplib. The copied source is
# included by zip_hydrology.sh in subsequent project handoffs.
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
target="$root/external/cpp-httplib"
if [[ -f "$target/httplib.h" ]]; then
    if grep -q '^#define CPPHTTPLIB_VERSION "0.58.0"' "$target/httplib.h"; then
        echo 'cpp-httplib v0.58.0 already initialized.'
        exit 0
    fi
    echo 'A different cpp-httplib version is already installed; refusing overwrite.' >&2
    exit 1
fi
command -v git >/dev/null || { echo 'git required' >&2; exit 1; }
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
git clone --quiet --depth 1 --branch v0.58.0     https://github.com/yhirose/cpp-httplib.git "$tmp/cpp-httplib"
commit="$(git -C "$tmp/cpp-httplib" rev-parse HEAD)"
if [[ "$commit" != 4f3f9ef* ]]; then
    echo "Unexpected cpp-httplib v0.58.0 commit: $commit" >&2
    exit 1
fi
mkdir -p -- "$target"
cp -- "$tmp/cpp-httplib/httplib.h" "$target/httplib.h"
cp -- "$tmp/cpp-httplib/LICENSE" "$target/LICENSE"
echo 'Installed cpp-httplib v0.58.0; include external/cpp-httplib in the next Git commit.'
