// ETL settings for this project (components/etl/etl is the ETL submodule).
#pragma once

// Target builds compile ETL's runtime checks out: ETL is used in code that
// runs while core 1 drives the bus, which can't reach an error handler in
// flash. csim keeps them, as assert()s, so overflows show up in tests.
#ifndef CSIM
#define ETL_NO_CHECKS
#endif

// No exceptions (IDF builds without them).
#undef ETL_THROW_EXCEPTIONS
