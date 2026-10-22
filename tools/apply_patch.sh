#!/usr/bin/env bash
set -euo pipefail

# A patch must be named for this repository, and every archive entry must
# remain inside AOWIS-SERVER-HYDROLOGY. Never unpack a foreign project patch.
if [[ $# -ne 1 ]]; then
    printf 'Usage: %s ~/Downloads/aowis-server-hydrology-<description>.zip\n' "${0##*/}" >&2
    exit 2
fi

patch_name="$(basename -- "$1")"
if [[ "$patch_name" != aowis-server-hydrology-*.zip ||
      "$patch_name" == aowis-server-hydrology-.zip ]]; then
    printf 'Refusing %s: expected aowis-server-hydrology-<description>.zip\n' "$patch_name" >&2
    exit 1
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
project_root="$(cd -- "$script_dir/.." && pwd -P)"
if [[ "$(basename -- "$project_root")" != AOWIS-SERVER-HYDROLOGY ]]; then
    printf 'Refusing to apply outside AOWIS-SERVER-HYDROLOGY.\n' >&2
    exit 1
fi

patch_path="$(realpath -e -- "$1")"
python3 - "$patch_path" "$project_root" <<'PY'
import os
from pathlib import Path
import stat
import sys
import tempfile
import zipfile

archive_path = Path(sys.argv[1])
root = Path(sys.argv[2])
project_name = root.name

# Complete validation before modifying the worktree; do not follow symlinked
# archive paths, or existing symlinks in the destination.
with zipfile.ZipFile(archive_path) as archive:
    entries = archive.infolist()
    if not entries:
        raise SystemExit('Refusing empty patch archive')
    files = []
    seen = set()
    for entry in entries:
        name = entry.filename
        parts = name.rstrip('/').split('/')
        if (not name or '\\' in name or '\x00' in name or
            len(parts) < 2 or parts[0] != project_name or
            any(part in ('', '.', '..') for part in parts) or
            '.git' in parts[1:] or
            name in seen):
            raise SystemExit(f'Invalid patch archive path: {name!r}')
        seen.add(name)
        mode = entry.external_attr >> 16
        kind = stat.S_IFMT(mode)
        if kind not in (0, stat.S_IFREG, stat.S_IFDIR):
            raise SystemExit(f'Refusing nonregular patch entry: {name!r}')
        if entry.is_dir() and kind == stat.S_IFREG:
            raise SystemExit(f'Invalid directory entry: {name!r}')
        if not entry.is_dir() and kind == stat.S_IFDIR:
            raise SystemExit(f'Invalid file entry: {name!r}')
        target = root.joinpath(*parts[1:])
        if root.is_symlink():
            raise SystemExit('Refusing symlinked project root')
        current = root
        for component in parts[1:]:
            current = current / component
            if current.is_symlink():
                raise SystemExit(f'Refusing symlinked destination: {current}')
        if entry.is_dir():
            if target.exists() and not target.is_dir():
                raise SystemExit(f'Directory conflicts with file: {target}')
        else:
            if target.exists() and not target.is_file():
                raise SystemExit(f'File conflicts with directory: {target}')
            files.append((entry, target, mode))

    touched = set()
    for entry in entries:
        if entry.is_dir():
            target = root.joinpath(*entry.filename.rstrip('/').split('/')[1:])
            target.mkdir(parents=True, exist_ok=True)
            touched.add(target)
    for entry, target, mode in files:
        target.parent.mkdir(parents=True, exist_ok=True)
        descriptor, temporary_name = tempfile.mkstemp(prefix='.hydrology-patch-', dir=target.parent)
        try:
            with os.fdopen(descriptor, 'wb') as output, archive.open(entry) as source:
                while True:
                    block = source.read(1024 * 1024)
                    if not block:
                        break
                    output.write(block)
            executable = bool(mode & 0o111)
            os.chmod(temporary_name, 0o755 if executable else 0o644)
            os.replace(temporary_name, target)
            os.utime(target, None)  # Use extraction time, never stale ZIP timestamps.
            touched.add(target.parent)
        finally:
            if os.path.exists(temporary_name):
                os.unlink(temporary_name)
    for directory in touched:
        os.utime(directory, None)
print(f'Applied {archive_path.name} ({len(files)} file(s)); refreshed timestamps.')
PY
