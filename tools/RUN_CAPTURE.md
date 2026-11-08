# Generic run capture

`run_capture.sh` wraps an arbitrary command in a Git-aware run record. It requires a Git working tree and an output directory outside that tree or ignored by Git. Canonical files are flat and share this prefix: `YYYYMMDD.HHMMSS.VERSION.DIFFHASH.LABEL`, where `VERSION` contains the abbreviated commit and dirty state, `DIFFHASH` is a 12-character SHA-256 prefix, and `LABEL` is `--label` (default `run`; anything but letters, digits, `_`, `.` and `-` becomes `_`). Suffixes identify artifacts: `.output`, `.git_diff`, `.git_version`, `.git_status`, `.cmd`, `.metadata`, `.untracked_hashes`, and `.untracked_paths`. The bare prefix is the historical extensionless marker. A `LABEL.output.latest` symlink in the output directory points to that label's newest `.output`. It is updated as each run starts, so during a run it is the live log. It is not a separate log, and its trailing `.latest` keeps it out of `*.output` globs.

The `.git_diff` artifact is the complete binary-safe tracked diff against `HEAD`, including staged and unstaged changes. `.git_status` captures `git status --branch --untracked-files=all`. Untracked file contents are not copied; `.untracked_paths` and `.untracked_hashes` record their paths and content hashes instead. `.cmd` records the working directory and shell-escaped argv. Common secret-like command-line option values are redacted in the recorded command, while the child receives the original arguments. `.output` contains merged stdout/stderr, and `.metadata` records commit, branch, full diff SHA-256, timestamps, and exit codes.

Usage:

```sh
tools/run_capture.sh \
  --repo /path/to/project \
  --output-dir /path/to/project/stash \
  --label bench \
  -- command arg1 arg2
```

The wrapper returns the child command's exit status and tees its console output. Project wrappers may set `RUN_CAPTURE_TOOL` to override their local copy. `RUN_CAPTURE_ACTIVE=1` is set for the child so wrappers can re-exec through the logger without recursion. Names have one-second timestamp granularity; a second run with the same label in the same output directory and same second fails rather than overwriting the first.
