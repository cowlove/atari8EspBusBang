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

#if CSIM_COROUTINE_ASM
// csim_switch(save, load): push the callee-saved registers and the FP
// control state on the current stack, store its pointer in *save, load
// load, pop the same from there and return on that stack. (SysV x86-64:
// rbx, rbp, r12-r15, MXCSR's control bits, the x87 control word.)
extern "C" void csim_switch(void **save, void *load);
__asm__(R"(
    .text
    .globl csim_switch
    .type csim_switch, @function
csim_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $8, %rsp
    stmxcsr (%rsp)
    fnstcw 4(%rsp)
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    ldmxcsr (%rsp)
    fldcw 4(%rsp)
    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
    .size csim_switch, .-csim_switch
)");
#endif

void CsimCoroutine::trampoline()
{
    CsimCoroutine *self = starting;
    starting = nullptr;
    self->entry_();
    self->finished_ = true;
    csimCurrentCoroutine = self->resumerCoroutine_;
#if CSIM_COROUTINE_ASM
    void *dead;
    csim_switch(&dead, self->resumerSp_);   // never resumed again
    abort();
#else
    // Returning switches to resumer_ (uc_link).
#endif
}

void CsimCoroutine::resume()
{
    if (finished_ || csimCurrentCoroutine == this) {
        return;
    }
    if (!started_) {
        started_ = true;
        starting = this;
#if CSIM_COROUTINE_ASM
        // A frame csim_switch() can pop: FP control state, six registers
        // (zero), then the "return address", trampoline(), entered with the
        // stack as a call leaves it (rsp = 8 mod 16, a dummy return slot).
        uint8_t *stack = static_cast<uint8_t *>(malloc(stackBytes_));
        uintptr_t top = (reinterpret_cast<uintptr_t>(stack) + stackBytes_) & ~uintptr_t(15);
        uint64_t *sp = reinterpret_cast<uint64_t *>(top);
        *--sp = 0;                                            // trampoline()'s return slot
        *--sp = reinterpret_cast<uint64_t>(&trampoline);      // csim_switch()'s ret
        for (int reg = 0; reg < 6; ++reg) {
            *--sp = 0;                                        // rbp rbx r12-r15
        }
        uint32_t fp[2];
        __asm__ volatile("stmxcsr %0" : "=m"(fp[0]));
        __asm__ volatile("fnstcw %0" : "=m"(fp[1]));
        *--sp = uint64_t(fp[0]) | (uint64_t(fp[1] & 0xFFFFU) << 32);
        selfSp_ = sp;
#else
        getcontext(&self_);
        self_.uc_stack.ss_sp = malloc(stackBytes_);
        self_.uc_stack.ss_size = stackBytes_;
        self_.uc_link = &resumer_;
        makecontext(&self_, trampoline, 0);
#endif
    }
    resumerCoroutine_ = csimCurrentCoroutine;
    csimCurrentCoroutine = this;
#if CSIM_COROUTINE_ASM
    csim_switch(&resumerSp_, selfSp_);
#else
    swapcontext(&resumer_, &self_);
#endif
    csimCurrentCoroutine = resumerCoroutine_;
}

void CsimCoroutine::yield()
{
    if (csimCurrentCoroutine != this) {
        return;
    }
    csimCurrentCoroutine = resumerCoroutine_;
#if CSIM_COROUTINE_ASM
    csim_switch(&selfSp_, resumerSp_);
#else
    swapcontext(&self_, &resumer_);
#endif
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
