#!/bin/bash
gnuplot -e "
    set term dumb; 
    set logscale y;
    unset key;
    plot '$1' u 1:2 w l ls 1, '$2' u 1:2 w l ls 2;
"
