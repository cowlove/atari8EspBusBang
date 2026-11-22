#!/bin/bash
# Clear main's build-time -D settings this run doesn't pass, from an existing
# build's CMake cache, so none carries over from an earlier run (a cache
# variable stays set until removed), without deleting the build: an
# incremental build instead of a full one.
#
#   tools/clear_build_defines.sh <build dir> [the run's idf.py args...]
#
# The names are the ones main/CMakeLists.txt reads: its option()s, its
# CACHE variables and its foreach(define_name ...) list; also the top-level
# CMakeLists.txt's option()s (SIDECAR and its kin). A name passed as
# -DNAME=... is left alone. No cache yet: nothing to do.
set -u
build=$1
shift
cache="$build/CMakeCache.txt"
[[ -f "$cache" ]] || exit 0
here=$(cd "$(dirname "$0")/.." && pwd)
lists="$here/main/CMakeLists.txt"
# Each name with its default: an option()'s is OFF, a CACHE variable's is
# its set() value; the foreach list's have none (any value is a -D).
declare -A default
while read -r name; do default[$name]=OFF; done < <(grep -hoE '^option\([A-Z_0-9]+' "$lists" "$here/CMakeLists.txt" | sed 's/option(//')
while read -r name value; do default[$name]=$value; done < <(
    sed -nE 's/^set\(([A-Z_0-9]+) "([^"]*)" CACHE .*/\1 \2/p' "$lists")
names=$(
    {
        printf '%s\n' "${!default[@]}"
        # The foreach list, which spans lines up to its closing parenthesis.
        sed -n '/foreach(define_name /,/)/p' "$lists" | sed 's/foreach(define_name//; s/)//' | tr -s ' \t' '\n'
    } | grep -E '^[A-Z_0-9]+$' | sort -u
)
passed=" "
for arg in "$@"; do
    [[ "$arg" == -D*=* ]] && passed+="$(sed -E 's/^-D([A-Za-z_0-9]+)=.*/\1/' <<< "$arg") "
done
unset_args=()
for name in $names; do
    [[ "$passed" == *" $name "* ]] && continue
    line=$(grep -E "^${name}:" "$cache") || continue
    value=${line#*=}
    if [[ -v "default[$name]" ]]; then
        # At its default: nothing to clear (clearing would only reconfigure).
        case "${default[$name]}:${value^^}" in
            OFF:OFF|OFF:0|OFF:FALSE|OFF:NO|OFF:) continue ;;
        esac
        [[ "$value" == "${default[$name]}" ]] && continue
    fi
    unset_args+=(-U "$name")
done
if ((${#unset_args[@]})); then
    printf 'clear_build_defines: clearing %s\n' "$(printf '%s ' "${unset_args[@]}" | sed 's/-U //g')"
    cmake "${unset_args[@]}" "$build" > /dev/null 2>&1
fi
