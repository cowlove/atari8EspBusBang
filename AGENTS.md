# Repository Guidance

## Firmware Smoke Tests

- Prefer `tools/boot.sh` for routine hardware build/flash/boot tests rather than invoking `idf.py flash` directly. It records timestamped serial output under `stash/` along with the Git version and a working-tree diff, providing a useful history of boot behavior.
- If the project-root IDF build cache is stale or points at an unavailable toolchain/source path, run `idf.py fullclean` from the project root, then continue with `tools/boot.sh`. `fullclean` removes generated build state only; it leaves source files and boot logs intact.
- This reproducible history is worth the inconvenience that `boot.sh` may leave serial-monitor processes running. After each run, check for stale monitors and terminate them before starting another capture.
- The script snapshots `git diff HEAD` and `git describe`, so the diff includes both staged and unstaged changes to tracked files. Untracked file contents are not included; account for those separately when they matter to a capture.

## Hardware Test Pool

- We maintain test hardware on `miner6.local`, with working directories `~/src/atari8EspBusBang.N` where `N` is currently `1` through `6`.
- Shorthand such as “git push and fast forward miner6 machine 3” means push the current branch, then SSH to `miner6.local` and run `git pull` in `~/src/atari8EspBusBang.3`. Apply the same mapping to similar machine-number references.
