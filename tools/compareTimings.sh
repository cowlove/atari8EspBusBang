#!/bin/bash
gnuplot -e "
    set term dumb ansi256; 
    set logscale y;
    unset key;
    set boxwidth 0.2;
    set style fill solid 1.0;
    set offsets graph 0.1, graph 0.1, graph 0.1, graph 0.1;
    plot '$1' u 1:2 w boxes, '$2' u 1:2 w boxes;
"
