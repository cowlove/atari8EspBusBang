#pragma once
// -DHOT_CACHE_TRAP=1 (a diagnostic): both caches suspended whenever core 0
// is hot, resumed at every way back to cold (the detach in
// ScopedInterruptEnable, core1Stalled(), core0Loop()'s return). Core 1
// never needs the cache, and core 0's hot code shouldn't either
// (tools/verify_core0_hot_calls.py), but the checks see only static
// addresses: code or data reached through a pointer (a heap buffer in
// PSRAM, a flash vtable or const table) gets past them, normally harmless
// as cache hits. Suspended, any such access by core 0 raises the cache
// error interrupt (level 4: through portDISABLE_INTERRUPTS()) and panics
// with its PC and address ("Cache disabled but cached memory region
// accessed"); core 1, without IDF's panic support, just wedges in the ROM,
// and core 0's watchdog exit (-10, its deliberate panic) dumps core: the
// core dump shows where. Suspending keeps
// the dirty lines, so nothing is lost, and nothing is written back while
// hot either. The ROM calls touch the cache's registers only at the
// transitions, with the bus detached (or core 1 not yet serving).
// One exemption: a PSRAM bank swap-in (ExtBankPool::getBank(), with the
// 6502 halted) copies to and from PSRAM by design; the caches are resumed
// for its copies only, and suspended again only if they were (getBank() also
// runs cold, at the memory probe's setup, where a suspend would kill the
// flash code it returns to). The calls are noinline IRAM functions (main.cpp), declared to
// tools/verify_core0_hot_calls.py as cold calls (top-level CMakeLists.txt,
// HOT_CACHE_TRAP builds only): they touch the cache's registers, so they
// may run only at the transitions.
#if defined(HOT_CACHE_TRAP) && HOT_CACHE_TRAP && !defined(CSIM)
#define A8BB_HOT_CACHE_TRAP 1
void hotCacheSuspend();
bool hotCacheResume();   // true: they were suspended (and are now resumed)
#else
#define A8BB_HOT_CACHE_TRAP 0
static inline void hotCacheSuspend() {}
static inline bool hotCacheResume() { return false; }
#endif
