#!/usr/bin/env bash
set -euo pipefail

# Package the entire current worktree, including uncommitted/new files, but
# excluding ignored builds, .git, secrets and user-specific settings.
if [[ $# -gt 1 ]]; then
    printf 'Usage: %s [output.zip]\n' "${0##*/}" >&2
    exit 2
fi
for program in git zip mktemp; do
    command -v "$program" >/dev/null 2>&1 || { printf 'Missing required command: %s\n' "$program" >&2; exit 1; }
done

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
project_root="$(cd -- "$script_dir/.." && pwd -P)"
project_name="$(basename -- "$project_root")"
project_parent="$(dirname -- "$project_root")"
if [[ "$project_name" != AOWIS-SERVER-HYDROLOGY ||
      "$(git -C "$project_root" rev-parse --show-toplevel 2>/dev/null)" != "$project_root" ]]; then
    printf 'This tool must run in the AOWIS-SERVER-HYDROLOGY Git worktree.\n' >&2
    exit 1
fi

output_arg="${1:-$project_parent/hydrology.zip}"
output_dir="$(dirname -- "$output_arg")"
[[ -d "$output_dir" ]] || { printf 'Output directory not found: %s\n' "$output_dir" >&2; exit 1; }
output_path="$(cd -- "$output_dir" && pwd -P)/$(basename -- "$output_arg")"
[[ "$output_path" == *.zip ]] || { printf 'Output must end with .zip\n' >&2; exit 1; }

tmp_dir="$(mktemp -d)"
trap 'rm -rf -- "$tmp_dir"' EXIT
archive="$tmp_dir/hydrology.zip"
paths=()
while IFS= read -r -d '' path; do
    [[ -f "$project_root/$path" && ! -L "$project_root/$path" ]] || continue
    [[ "$project_root/$path" != "$output_path" ]] || continue
    # Benchmark entrypoints are documented as directly executable. Preserve
    # that contract in worktree archives rather than silently distributing
    # a non-executable script (which will remain non-executable on extraction).
    case "$path" in
        tools/benchmark_map_*.py)
            if [[ ! -x "$project_root/$path" ]]; then
                printf 'Benchmark entrypoint must be executable: %s (run chmod +x %q)\n' \
                    "$path" "$project_root/$path" >&2
                exit 1
            fi
            ;;
    esac
    # Exclude the original misnamed Bash script if it remains after upgrade.
    [[ "$path" != tools/zip_hydrology.zip ]] || continue
    case "/$path" in
        */.env|*/.env.*|*.pem|*.key|*.p12|*.pfx|*/id_rsa|*/id_ed25519) continue ;;
        */__pycache__/*|*.pyc|*.pyo|*.pyd) continue ;;
    esac
    if [[ "$path" == *$'\n'* ]]; then
        printf 'Filename contains a newline: cannot package safely.\n' >&2
        exit 1
    fi
    paths+=("$project_name/$path")
done < <(git -C "$project_root" ls-files -z --cached --others --exclude-standard)

if [[ ${#paths[@]} -eq 0 ]]; then
    printf 'No project files available to archive.\n' >&2
    exit 1
fi
(
    cd -- "$project_parent"
    printf '%s\n' "${paths[@]}" | LC_ALL=C sort -u | zip -q "$archive" -@
)
mv -f -- "$archive" "$output_path"
printf 'Created %s (%d files)\n' "$output_path" "${#paths[@]}"
