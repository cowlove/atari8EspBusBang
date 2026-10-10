#pragma once 
#ifndef TEST_HW_ID
#error testing to make sure TEST_HW_ID defined
#define TEST_HW_ID 0
#endif
static constexpr DRAM_ATTR int SWEEP=78;

#if TEST_HW_ID == 0 
#define LEADIN_NOPS 4
#define RD_EXIT_TS 80
#define WR_EXIT_TS 80

#elif TEST_HW_ID == 1
#define LEADIN_NOPS 4
#define RD_EXIT_TS 100
#define WR_EXIT_TS 80

#elif TEST_HW_ID == 2
#define LEADIN_NOPS 4
#define RD_EXIT_TS 100
#define WR_EXIT_TS 80

#elif TEST_HW_ID == 3 
#define LEADIN_NOPS 4 
#define RD_EXIT_TS 80 
#define WR_EXIT_TS 72 

#elif TEST_HW_ID == 4
#define LEADIN_NOPS 4
#define RD_EXIT_TS 80
#define WR_EXIT_TS 72 

#elif TEST_HW_ID == 5
#define LEADIN_NOPS 0
#define RD_EXIT_TS 105
#define WR_EXIT_TS 85

#elif TEST_HW_ID == 6
#define LEADIN_NOPS 0
#define RD_EXIT_TS 105
#define WR_EXIT_TS 85

#endif

