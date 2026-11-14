#ifndef A8BB_CSIM_COROUTINE_H
#define A8BB_CSIM_COROUTINE_H

// CSIM: cooperative coroutines for the code that runs beside atari800's CPU
// loop on the hardware: the host's core 0 (bus-cycle mode) and the Sidecar
// app. Each gets its own stack (POSIX ucontext) and runs its real loop; it
// yields back to whoever resumed it whenever it waits: a clock read every
// so often (csim.h) or a link wait (i2slink_csim.cpp). atari800's loop,
// on the process's own stack, is the scheduler: each CPU tick advances the
// clock and resumes them in turn (csim_stubs.cpp). Code on one stack never
// re-enters code suspended on another, so neither side needs guards
// against being called back from inside its own waits.

#include <stddef.h>
#include <stdint.h>
#include <ucontext.h>

class CsimCoroutine {
public:
    // yieldEvery: yield on every Nth clock read from this coroutine (a
    // power of two): how much of its loop it runs per CPU tick.
    CsimCoroutine(const char *name, void (*entry)(), uint32_t yieldEvery,
                  size_t stackBytes = 1U << 20);

    // Run it until it yields (or its entry returns). From the scheduler or
    // another coroutine; not from itself.
    void resume();
    // Back to the resumer. A no-op unless called on this coroutine's stack.
    void yield();

    bool started() const { return started_; }
    bool finished() const { return finished_; }
    const char *name() const { return name_; }

    // A clock read on this coroutine's stack: yield on every yieldEvery-th.
    void clockRead()
    {
        if ((++clockReads_ & (yieldEvery_ - 1U)) == 0U) {
            yield();
        }
    }

private:
    static void trampoline();

    const char *name_;
    void (*entry_)();
    uint32_t yieldEvery_;
    uint32_t clockReads_ = 0;
    size_t stackBytes_;
    bool started_ = false;
    bool finished_ = false;
    ucontext_t self_;
    ucontext_t resumer_;
    CsimCoroutine *resumerCoroutine_ = nullptr;
};

// The coroutine whose stack is running, or nullptr on the scheduler's.
extern CsimCoroutine *csimCurrentCoroutine;

// A clock read (csim.h's XTHAL_GET_CCOUNT(), esp_timer_get_time()): on a
// coroutine, yield every so often, so loops that wait on time let the rest
// of the simulation run.
static inline void csim_coroutine_clock_read()
{
    if (csimCurrentCoroutine) {
        csimCurrentCoroutine->clockRead();
    }
}

// One step of a wait for simulated time `until` (CCOUNT), or for something
// another side will do (a link read with nothing new): on a coroutine,
// yield (the clock moves while it's suspended); on the scheduler's stack,
// where nothing else would run, resume the background coroutines (the
// Sidecar) and advance the clock a CPU tick's worth, or to `until` if
// that's sooner. Callers loop on their own condition.
void csim_wait_step(uint64_t until = UINT64_MAX);
// Wait until the clock reaches `until`, a step at a time.
void csim_wait_until(uint64_t until);

// Background coroutines, run by csim_wait_step() on the scheduler's stack
// (setup()'s link probe, default mode's in-line core 0 work). Registered by
// their owners.
void csim_add_background(void (*run)());

#endif
