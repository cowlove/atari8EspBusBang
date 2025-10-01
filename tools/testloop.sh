#!/bin/bash
cd "$(dirname $0)/.."
while sleep .1; do ./tools/boot.sh; ./tools/sdx.sh; done

