# csim: what the simulator shares across the code it runs: the host app
# (main/), the Sidecar app (sidecar/) and atari800 (csim/a800/atari800). On
# the IDF builds its macros compile to nothing. The csim projects themselves
# stay in csim/; csim/a800/Makefile adds include/ to every build there,
# atari800's included.
idf_component_register(INCLUDE_DIRS "include")
