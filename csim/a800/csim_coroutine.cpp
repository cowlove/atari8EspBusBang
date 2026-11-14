// CSIM coroutines: see include/csim_coroutine.h.

#include "csim_coroutine.h"
#include "csim.h"

#include <stdio.h>
#include <stdlib.h>
#include <vector>

CsimCoroutine *csimCurrentCoroutine = nullptr;

namespace {
// makecontext() passes only int arguments: the coroutine being started
// goes through here instead.
CsimCoroutine *starting = nullptr;
std::vector<void (*)()> background;
}  // namespace

CsimCoroutine::CsimCoroutine(const char *name, void (*entry)(), uint32_t yieldEvery,
                             size_t stackBytes)
    : name_(name), entry_(entry), yieldEvery_(yieldEvery ? yieldEvery : 1U),
      stackBytes_(stackBytes)
{
    if (yieldEvery_ & (yieldEvery_ - 1U)) {
        fprintf(stderr, "csim coroutine %s: yieldEvery %u is not a power of two\n", name,
                (unsigned)yieldEvery);
        abort();
    }
}

void CsimCoroutine::trampoline()
{
    CsimCoroutine *self = starting;
    starting = nullptr;
    self->entry_();
    self->finished_ = true;
    // Returning switches to resumer_ (uc_link); restore the bookkeeping
    // resume() would.
    csimCurrentCoroutine = self->resumerCoroutine_;
}

void CsimCoroutine::resume()
{
    if (finished_ || csimCurrentCoroutine == this) {
        return;
    }
    if (!started_) {
        started_ = true;
        getcontext(&self_);
        self_.uc_stack.ss_sp = malloc(stackBytes_);
        self_.uc_stack.ss_size = stackBytes_;
        self_.uc_link = &resumer_;
        starting = this;
        makecontext(&self_, trampoline, 0);
    }
    resumerCoroutine_ = csimCurrentCoroutine;
    csimCurrentCoroutine = this;
    swapcontext(&resumer_, &self_);
    csimCurrentCoroutine = resumerCoroutine_;
}

void CsimCoroutine::yield()
{
    if (csimCurrentCoroutine != this) {
        return;
    }
    csimCurrentCoroutine = resumerCoroutine_;
    swapcontext(&self_, &resumer_);
    // Resumed: resume() has set csimCurrentCoroutine to this again.
}

void csim_add_background(void (*run)())
{
    background.push_back(run);
}

void csim_wait_step(uint64_t until)
{
    if (csimCurrentCoroutine) {
        csimCurrentCoroutine->yield();
        return;
    }
    for (void (*run)() : background) {
        run();
    }
    const uint64_t now = CSIM_PEEK_CCOUNT64();
    const uint64_t tick = 16U * 132U;   // one 16-cycle CPU tick
    if (until > now) {
        CSIM_ADVANCE_CCOUNT(static_cast<uint32_t>(until - now < tick ? until - now : tick));
    }
}

void csim_wait_until(uint64_t until)
{
    while (CSIM_PEEK_CCOUNT64() < until) {
        csim_wait_step(until);
    }
}
