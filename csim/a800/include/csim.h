#pragma once

// every other function or macro used in ./main/*.cpp needs to be 
// implemented here.  Don't worry about any function body at first,
// just return 0 if it has a return value.  The first goal is 
// just compilation.
static inline uint32_t XTHAL_GET_CCOUNT() { 
	static uint32_t ccount = 0;
	return ccount++;
}
