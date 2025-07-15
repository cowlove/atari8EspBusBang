#!/bin/bash
gnuplot -e "set term dumb; plot '$1' u 1:3 w l ls 1, '$1' u 1:4 w l ls 1, '$2' u 1:3 w l ls 2, '$2' u 1:4 w l ls 3;"
