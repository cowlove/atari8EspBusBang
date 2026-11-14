// nanoprintf (main/nanoprintf.h, https://github.com/charlesnicholson/nanoprintf,
// 0BSD/Unlicense, vendored 2026-11-14): the formatter LOGHOT() runs with
// core 1 hot (a8log_hot(), main/a8log.cpp). This object is in internal RAM
// (link.lf: a8logFmt (noflash)): code, and the constants it reads. About
// 1.8 KB at -Os in this configuration (measured 2026-11-14: 1771 bytes;
// 1013 without precision, ll/z/hh and '#'). Its only calls are ROM ones
// (__ashldi3, __lshrdi3). No floats: %f prints nothing useful.
#include "sdkconfig.h"
#ifdef CSIM
#include "csim.h"
#endif

#if defined(CONFIG_A8BB_LOG_VIA_SIDECAR) && CONFIG_A8BB_LOG_VIA_SIDECAR
#define NANOPRINTF_USE_FIELD_WIDTH_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_PRECISION_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_FLOAT_FORMAT_SPECIFIERS 0
#define NANOPRINTF_USE_LARGE_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_SMALL_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_BINARY_FORMAT_SPECIFIERS 0
#define NANOPRINTF_USE_WRITEBACK_FORMAT_SPECIFIERS 0
#define NANOPRINTF_USE_ALT_FORM_FLAG 1
#define NANOPRINTF_IMPLEMENTATION
#include "nanoprintf.h"
#endif
