#!/usr/bin/env bash
set -euo pipefail

# Run once inside the real Hydrology Git worktree after unpacking this patch.
# This creates an actual Git submodule gitlink and .gitmodules entry, which
# a changes-only ZIP archive cannot represent by itself.
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd -- "$root"
if [[ "$(git rev-parse --show-toplevel 2>/dev/null || :)" != "$root" ]]; then
    echo 'Run this script from a checked-out AOWIS-SERVER-HYDROLOGY Git repository.' >&2
    exit 2
fi
if [[ -e external/GeographicLib && ! -d external/GeographicLib/.git &&
      ! -f external/GeographicLib/.git ]]; then
    echo 'external/GeographicLib exists but is not a Git checkout. Refusing to replace it.' >&2
    exit 2
fi
if [[ ! -e external/GeographicLib ]]; then
    git submodule add --depth 1 https://github.com/geographiclib/geographiclib.git external/GeographicLib
else
    git submodule update --init external/GeographicLib
fi
# Match the GeographicLib 2.7 source bundled with the current GUI snapshot.
git -C external/GeographicLib fetch --depth 1 origin tag v2.7
git -C external/GeographicLib checkout --detach v2.7
git add .gitmodules external/GeographicLib
printf 'GeographicLib submodule registered at %s\n' "$(git -C external/GeographicLib rev-parse --short HEAD)"
printf 'Commit .gitmodules and the external/GeographicLib gitlink with your changes.\n'
