#!/bin/bash
# Run a shell command line in each test-pool checkout (~/src/atari8EspBusBang.1-6),
# each in its own subshell, e.g.
#   tools/testpool_forEach.sh "git checkout master && git pull && git submodule update --init"
for n in {1..6}; do
    d=~/src/atari8EspBusBang.$n
    echo "== $d"
    ( cd "$d" && eval "$*" ) || echo "== $d: failed ($?)"
done
