#!/usr/bin/env bash
set -euo pipefail

# Package the worktree (including uncommitted/new non-ignored files) and all
# initialized Git submodule worktrees, recursively. Never package Git metadata,
# ignored build outputs, secrets, or local user settings.
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
submodule_count=0

# List the files of a Git worktree and then descend into its gitlinks. Git's
# parent ls-files returns only the gitlink, not the files inside the submodule.
# An uninitialized submodule is an error: silently producing a ZIP that cannot
# be built is worse than failing with an actionable message.
collect_worktree() {
    local root="$1" relative_root="$2"
    local listing gitlinks path archive_path entry mode child child_root git_root
    listing="$(mktemp "$tmp_dir/files.XXXXXX")"
    gitlinks="$(mktemp "$tmp_dir/gitlinks.XXXXXX")"
    git -C "$root" ls-files -z --cached --others --exclude-standard > "$listing"
    git -C "$root" ls-files -z --stage > "$gitlinks"

    while IFS= read -r -d '' path; do
        [[ -f "$root/$path" && ! -L "$root/$path" ]] || continue
        [[ "$root/$path" != "$output_path" ]] || continue
        # Ensure that no Git internals can leak into the handoff, even in
        # unusual untracked directories inside submodules.
        case "/$path/" in
            */.git/*) continue ;;
        esac
        # Benchmark entrypoints are documented as directly executable.
        case "$relative_root$path" in
            tools/benchmark_map_*.py)
                if [[ ! -x "$root/$path" ]]; then
                    printf 'Benchmark entrypoint must be executable: %s (run chmod +x %q)\n' \
                        "$path" "$root/$path" >&2
                    exit 1
                fi
                ;;
        esac
        [[ "$relative_root$path" != tools/zip_hydrology.zip ]] || continue
        case "/$path" in
            */.env|*/.env.*|*.pem|*.key|*.p12|*.pfx|*/id_rsa|*/id_ed25519) continue ;;
            */__pycache__/*|*.pyc|*.pyo|*.pyd) continue ;;
        esac
        archive_path="$project_name/$relative_root$path"
        if [[ "$archive_path" == *$'\n'* ]]; then
            printf 'Filename contains a newline: cannot package safely.\n' >&2
            exit 1
        fi
        paths+=("$archive_path")
    done < "$listing"

    while IFS= read -r -d '' entry; do
        mode="${entry%% *}"
        [[ "$mode" == 160000 ]] || continue
        child="${entry#*$'\t'}"
        child_root="$root/$child"
        if [[ "$child" == *$'\n'* || ! -e "$child_root/.git" ]]; then
            printf 'Git submodule is not initialized: %s\n' "$relative_root$child" >&2
            printf 'Run: git submodule update --init --recursive\n' >&2
            exit 1
        fi
        git_root="$(git -C "$child_root" rev-parse --show-toplevel 2>/dev/null || :)"
        if [[ "$git_root" != "$(cd -- "$child_root" && pwd -P)" ]]; then
            printf 'Invalid Git submodule worktree: %s\n' "$relative_root$child" >&2
            printf 'Run: git submodule update --init --recursive\n' >&2
            exit 1
        fi
        ((submodule_count += 1))
        collect_worktree "$child_root" "$relative_root$child/"
    done < "$gitlinks"
}

collect_worktree "$project_root" ""

if [[ ${#paths[@]} -eq 0 ]]; then
    printf 'No project files available to archive.\n' >&2
    exit 1
fi
(
    cd -- "$project_parent"
    printf '%s\n' "${paths[@]}" | LC_ALL=C sort -u | zip -q "$archive" -@
)
mv -f -- "$archive" "$output_path"
printf 'Created %s (%d files, %d initialized submodules)\n' \
    "$output_path" "${#paths[@]}" "$submodule_count"
