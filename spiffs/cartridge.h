#ifndef STRESS_CARTRIDGE_H
#define STRESS_CARTRIDGE_H

#include <stdint.h>

#include "atari_memory.h"

#define CART_CALLS_PER_REPORT 1800

#ifdef STRESS_MEGACART
#define CART_BANK_COUNT 8
#define CART_BANK(n) __attribute__((section(".bank" #n), noinline))
#define CART_FIXED __attribute__((section(".bank0_fixed"), noinline))

static uint8_t currentCartBank = 0;

static CART_FIXED void cartridge_banked_call(uint8_t bank, void (*function)(void)) {
    uint8_t previousBank = currentCartBank;
    currentCartBank = bank;
    ATARI_D500 = bank;
    function();
    currentCartBank = previousBank;
    ATARI_D500 = previousBank;
}

#define CART_CALL(bank, function) cartridge_banked_call((bank), (function))
#else
#include <banked-call.h>
#define CART_BANK_COUNT 16
#define CART_BANK(n) ATARIMAX_BANK(n)
#define CART_FIXED
#define CART_CALL(bank, function) atarimax_banked_call((bank), (function))
#endif

static volatile uint16_t cartBankCalls[CART_BANK_COUNT];

#define CART_DEFINE_BANK_PROBE(n) \
    CART_BANK(n) void bank##n##_probe(void) { ++cartBankCalls[n]; }

#define CART_CALL_BANK(n) CART_CALL(n, bank##n##_probe)

#endif
