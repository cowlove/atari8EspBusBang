#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'Usage: %s --repo PATH --output-dir PATH [--label NAME] -- COMMAND [ARG... ]\n' "$0" >&2
}

repo_path=""
output_dir=""
label="run"
while (($#)); do
    case "$1" in
        --repo)
            (($# >= 2)) || { usage; exit 2; }
            repo_path="$2"
            shift 2
            ;;
        --output-dir)
            (($# >= 2)) || { usage; exit 2; }
            output_dir="$2"
            shift 2
            ;;
        --label)
            (($# >= 2)) || { usage; exit 2; }
            label="$2"
            shift 2
            ;;
        --)
            shift
            break
            ;;
        *)
            usage
            exit 2
            ;;
    esac
done

if [[ -z "$repo_path" || -z "$output_dir" || $# -eq 0 ]]; then
    usage
    exit 2
fi

repo_root="$(git -C "$repo_path" rev-parse --show-toplevel)"
output_dir="$(mkdir -p "$output_dir" && cd "$output_dir" && pwd)"
case "$output_dir" in
    "$repo_root"/*)
        output_relative_path="${output_dir#"$repo_root"/}"
        if ! git -C "$repo_root" check-ignore -q -- "$output_relative_path/"; then
            printf 'Output directory must be outside the repository or Git-ignored: %s\n' \
                "$output_dir" >&2
            exit 2
        fi
        ;;
esac

timestamp="$(date +%Y%m%d.%H%M%S)"
legacy_prefix="$output_dir/$timestamp"
if [[ -e "$legacy_prefix.output" || -e "$legacy_prefix.git_diff" ]]; then
    printf 'A run already exists for timestamp %s; retry after the next second.\n' "$timestamp" >&2
    exit 2
fi

temporary_dir="$(mktemp -d)"
trap 'rm -rf "$temporary_dir"' EXIT
git -C "$repo_root" status --branch --untracked-files=all > "$temporary_dir/git-status.txt"
git -C "$repo_root" diff HEAD --binary --no-ext-diff > "$temporary_dir/git-diff.patch"
git -C "$repo_root" ls-files --others --exclude-standard -z > "$temporary_dir/untracked-paths.list"
LC_ALL=C sort -z "$temporary_dir/untracked-paths.list" -o "$temporary_dir/untracked-paths.list"
untracked_manifest="$temporary_dir/untracked-hashes.txt"
while IFS= read -r -d '' path; do
    if [[ -L "$repo_root/$path" ]]; then
        digest="$(readlink -z -- "$repo_root/$path" | sha256sum | cut -d' ' -f1)"
    elif [[ -f "$repo_root/$path" ]]; then
        digest="$(sha256sum -- "$repo_root/$path" | cut -d' ' -f1)"
    else
        digest="non-regular-file"
    fi
    printf '%q %s\n' "$path" "$digest" >> "$untracked_manifest"
done < "$temporary_dir/untracked-paths.list"
touch "$untracked_manifest"

diff_hash="$(sha256sum "$temporary_dir/git-diff.patch" | cut -c1-12)"
version="$(git -C "$repo_root" describe --abbrev=12 --dirty --always)"
safe_version="${version//[^[:alnum:]_.-]/_}"
safe_label="${label//[^[:alnum:]_.-]/_}"
artifact_prefix="$output_dir/$timestamp.$safe_version.$diff_hash"
marker_path="$artifact_prefix"
diff_path="$artifact_prefix.git_diff"
status_path="$artifact_prefix.git_status"
version_path="$artifact_prefix.git_version"
untracked_path="$artifact_prefix.untracked_hashes"
untracked_list_path="$artifact_prefix.untracked_paths"
command_path="$artifact_prefix.cmd"
metadata_path="$artifact_prefix.metadata"
console_path="$artifact_prefix.output"

mv "$temporary_dir/git-diff.patch" "$diff_path"
mv "$temporary_dir/git-status.txt" "$status_path"
mv "$untracked_manifest" "$untracked_path"
tr '\0' '\n' < "$temporary_dir/untracked-paths.list" > "$untracked_list_path"
printf '%s\n' "$version" > "$version_path"
: > "$marker_path"
ln -s "$(basename "$console_path")" "$legacy_prefix.output"

{
    printf 'working_directory=%q\n' "$PWD"
    printf 'argv='
    redact_next=0
    for argument in "$@"; do
        if ((redact_next)); then
            printf '%q ' '<redacted>'
            redact_next=0
            continue
        fi
        if [[ "$argument" =~ ^(--?)(password|token|secret|api[-_]?key|access[-_]?key|authorization|credential)$ ]]; then
            printf '%q ' "$argument"
            redact_next=1
        elif [[ "$argument" =~ ^(--?)(password|token|secret|api[-_]?key|access[-_]?key|authorization|credential)=(.*)$ ]]; then
            printf '%q ' "${BASH_REMATCH[1]}${BASH_REMATCH[2]}=<redacted>"
        elif [[ "$argument" =~ ^[A-Za-z_][A-Za-z0-9_]*(PASSWORD|TOKEN|SECRET|API_KEY|ACCESS_KEY)= ]]; then
            printf '%q ' "${argument%%=*}=<redacted>"
        else
            printf '%q ' "$argument"
        fi
    done
    printf '\n'
} > "$command_path"

{
    printf 'timestamp=%s\n' "$timestamp"
    printf 'started_utc=%s\n' "$(date -u --iso-8601=ns)"
    printf 'repository=%s\n' "$repo_root"
    printf 'commit=%s\n' "$(git -C "$repo_root" rev-parse HEAD)"
    printf 'version=%s\n' "$version"
    printf 'branch=%s\n' "$(git -C "$repo_root" branch --show-current)"
    printf 'diff_sha256=%s\n' "$(sha256sum "$diff_path" | cut -d' ' -f1)"
    printf 'diff_path=%s\n' "$diff_path"
    printf 'status_path=%s\n' "$status_path"
    printf 'untracked_hash_manifest=%s\n' "$untracked_path"
    printf 'command_path=%s\n' "$command_path"
    printf 'console_path=%s\n' "$console_path"
    printf 'command_environment=RUN_CAPTURE_ACTIVE=1\n'
} > "$metadata_path"

set +e
RUN_CAPTURE_ACTIVE=1 "$@" 2>&1 | tee "$console_path"
pipeline_status=("${PIPESTATUS[@]}")
set -e
command_status="${pipeline_status[0]}"
tee_status="${pipeline_status[1]}"
printf 'finished_utc=%s\ncommand_exit_status=%s\ntee_exit_status=%s\n' \
    "$(date -u --iso-8601=ns)" "$command_status" "$tee_status" >> "$metadata_path"
exit "$command_status"
