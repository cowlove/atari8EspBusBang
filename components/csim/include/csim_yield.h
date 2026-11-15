#ifndef A8BB_CSIM_YIELD_H
#define A8BB_CSIM_YIELD_H
/* Context-switch points for the simulator, one per side:
     CSIM_CORE0_YIELD()    the host app's core 0 code (main/)
     CSIM_6502_YIELD()     atari800, the 6502 side (csim/a800/atari800/src)
     CSIM_SIDECAR_YIELD()  the Sidecar app (sidecar/)
   Plain C: atari800 includes it. Empty unless CSIM is defined, so the IDF
   builds see nothing. */

#ifdef CSIM
/* TODO: the csim implementations. */
#define CSIM_CORE0_YIELD() ((void)0)
#define CSIM_6502_YIELD() ((void)0)
#define CSIM_SIDECAR_YIELD() ((void)0)
#else
#define CSIM_CORE0_YIELD() ((void)0)
#define CSIM_6502_YIELD() ((void)0)
#define CSIM_SIDECAR_YIELD() ((void)0)
#endif

#endif
