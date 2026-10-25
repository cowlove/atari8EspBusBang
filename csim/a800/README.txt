Let's start with just getting the .cpp files in ./main to compile
under ubuntu/gcc, with the exception of core1.cpp.  The first goal
is just a nop-stub of every external header file and function
called.  Initially, this step can serve as a quick code syntax and
verification tool.  "make -C csim/a800" will quickly indicate that 
the code is capable of being parsed and compiled. 

We can include every stubbed out function and macro in ./csim/a800/include/csim.h 
All the ./main/*.cpp files will need a conditional inclusion of 
#ifdef CSIM
#include "csim.h"
#endif

The Makefile compiles all included sources and links a shared link-check
artifact with unresolved-symbol checking enabled. This is not a runnable
simulator: the ESP-IDF and platform stubs are no-ops, and core1.cpp is
excluded. Do not execute or load the shared object; this stage only verifies
compile/link coverage.

