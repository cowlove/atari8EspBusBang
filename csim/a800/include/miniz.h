#include "csim.h"
typedef unsigned int mz_uint32;
#define TINFL_LZ_DICT_SIZE 32768
#define MZ_CRC32_INIT 0
#define TINFL_FLAG_HAS_MORE_INPUT 1
#define TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF 2
typedef struct { int unused; } tinfl_decompressor;
typedef enum {
    TINFL_STATUS_BAD_PARAM = -3,
    TINFL_STATUS_FAILED = -1,
    TINFL_STATUS_DONE = 0,
    TINFL_STATUS_NEEDS_MORE_INPUT = 1,
    TINFL_STATUS_HAS_MORE_OUTPUT = 2
} tinfl_status;
static inline void tinfl_init(tinfl_decompressor *decompressor) { (void)decompressor; }
static inline tinfl_status tinfl_decompress(tinfl_decompressor *decompressor,
    const unsigned char *input, size_t *input_size, const unsigned char *output_start,
    unsigned char *output_next, size_t *output_size, mz_uint32 flags) {
    (void)decompressor; (void)input; (void)input_size; (void)output_start;
    (void)output_next; (void)output_size; (void)flags;
    return TINFL_STATUS_FAILED;
}
static inline uint32_t mz_crc32(uint32_t crc, const unsigned char *buf, size_t len) { (void)buf; (void)len; return crc; }
