# a8log: LOG(), the project's printf() (include/a8log.h). Shared by the
# host app (main/), the Sidecar app (sidecar/) and their common components;
# csim compiles against the header directly (csim/a800/Makefile).
idf_component_register(INCLUDE_DIRS "include")
