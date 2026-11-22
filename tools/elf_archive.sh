#!/bin/bash
# Keep a copy of main's ELF by its SHA-256, so a core dump taken from an
# older build can still be decoded after build/ has moved on (crash_bundle.sh
# looks a core's app SHA up here when build/'s ELF isn't the one: a core 1
# fault found only on the next run's boot, say).
#
# Usage: elf_archive.sh [<elf>]   (default build/atari8EspBusBang.elf)
# Writes stash/elf/<sha256>.elf.gz unless it's there; keeps the newest 40
# (~3.5 MB each). Never fails the caller.
cd "$(dirname "$0")/.." || exit 0
elf="${1:-build/atari8EspBusBang.elf}"
dir=stash/elf
[[ -f "$elf" ]] || exit 0
mkdir -p "$dir" || exit 0
sha="$(sha256sum "$elf" | cut -d' ' -f1)"
dest="$dir/$sha.elf.gz"
if [[ ! -f "$dest" ]]; then
	gzip -1 -c -- "$elf" > "$dest.tmp" && mv -f -- "$dest.tmp" "$dest" || rm -f -- "$dest.tmp"
else
	touch -- "$dest"   # in use again: newest
fi
ls -1t "$dir"/*.elf.gz 2>/dev/null | tail -n +41 | xargs -r rm -f --
exit 0
