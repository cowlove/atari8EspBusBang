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

temporary_dir="$(mktemp -d)"
trap 'rm -rf "$temporary_dir"' EXIT
# "normal" collapses wholly untracked directories (e.g. old log archives) to one line.
git -C "$repo_root" status --branch --untracked-files=normal > "$temporary_dir/git-status.txt"
git -C "$repo_root" diff HEAD --binary --no-ext-diff > "$temporary_dir/git-diff.patch"
git -C "$repo_root" ls-files --others --exclude-standard -z > "$temporary_dir/all-untracked-paths.list"
untracked_total="$(tr -cd '\0' < "$temporary_dir/all-untracked-paths.list" | wc -c)"
# Only inventory recently modified untracked files; long-running hosts can hold
# millions of stale untracked logs. Set to "all" to inventory everything.
untracked_max_age_days="${RUN_CAPTURE_UNTRACKED_MAX_AGE_DAYS:-7}"
if [[ "$untracked_max_age_days" == all ]]; then
    mv "$temporary_dir/all-untracked-paths.list" "$temporary_dir/untracked-paths.list"
else
    [[ "$untracked_max_age_days" =~ ^[0-9]+$ ]] || {
        printf 'RUN_CAPTURE_UNTRACKED_MAX_AGE_DAYS must be a whole number of days or "all": %s\n' \
            "$untracked_max_age_days" >&2
        exit 2
    }
    untracked_cutoff_epoch=$(( $(date +%s) - untracked_max_age_days * 86400 ))
    # Prefix ./ so paths starting with "-" are not parsed as find options.
    (cd "$repo_root" && sed -z 's|^|./|' "$temporary_dir/all-untracked-paths.list" |
        xargs -0 -r sh -c 'cutoff="$1"; shift; exec find "$@" -maxdepth 0 -newermt "@$cutoff" -print0' \
            find-recent "$untracked_cutoff_epoch" |
        sed -z 's|^\./||') > "$temporary_dir/untracked-paths.list"
fi
LC_ALL=C sort -z "$temporary_dir/untracked-paths.list" -o "$temporary_dir/untracked-paths.list"
untracked_inventoried="$(tr -cd '\0' < "$temporary_dir/untracked-paths.list" | wc -c)"
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
# The label goes last, just before each artifact's suffix, so runs with
# different labels (say mainapp and sidecar) can share a directory and a second.
artifact_prefix="$output_dir/$timestamp.$safe_version.$diff_hash.$safe_label"
if [[ -e "$artifact_prefix" || -e "$artifact_prefix.output" ]]; then
    printf 'A %s run already exists for timestamp %s; retry after the next second.\n' \
        "$safe_label" "$timestamp" >&2
    exit 2
fi
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
# <label>.output.latest always points at this label's newest console log,
# the current one while a run is going. The trailing suffix keeps it out of
# the tool scripts' *.output globs. Make the link under a temporary name and
# rename it over the old one, so readers never find it missing.
latest_link="$output_dir/$safe_label.output.latest"
ln -sfn -- "$(basename "$console_path")" "$latest_link.tmp.$$"
mv -fT -- "$latest_link.tmp.$$" "$latest_link"

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
    printf 'untracked_max_age_days=%s\n' "$untracked_max_age_days"
    printf 'untracked_files_total=%s\n' "$untracked_total"
    printf 'untracked_files_inventoried=%s\n' "$untracked_inventoried"
    printf 'command_path=%s\n' "$command_path"
    printf 'console_path=%s\n' "$console_path"
    printf 'command_environment=RUN_CAPTURE_ACTIVE=1 RUN_CAPTURE_PREFIX=%s\n' "$artifact_prefix"
} > "$metadata_path"

set +e
# RUN_CAPTURE_PREFIX lets the command find its own artifacts (e.g. the
# console output at $RUN_CAPTURE_PREFIX.output).
RUN_CAPTURE_ACTIVE=1 RUN_CAPTURE_PREFIX="$artifact_prefix" "$@" 2>&1 | tee "$console_path"
pipeline_status=("${PIPESTATUS[@]}")
set -e
command_status="${pipeline_status[0]}"
tee_status="${pipeline_status[1]}"
printf 'finished_utc=%s\ncommand_exit_status=%s\ntee_exit_status=%s\n' \
    "$(date -u --iso-8601=ns)" "$command_status" "$tee_status" >> "$metadata_path"
exit "$command_status"
