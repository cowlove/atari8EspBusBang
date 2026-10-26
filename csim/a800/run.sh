#!/bin/bash
#
make -C /tmp/atari800-video-hybrid/
make TEST_SEC=-1 ATARI800_BUILD=/tmp/atari800-video-hybrid hybrid
/tmp/atari800-video-hybrid/src/atari800-csim -xl -nopatchall

