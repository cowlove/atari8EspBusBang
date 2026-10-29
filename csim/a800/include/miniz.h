#include "csim.h"
#include <zlib.h>
typedef unsigned int mz_uint32;
#define TINFL_LZ_DICT_SIZE 32768
#define MZ_CRC32_INIT 0
#define TINFL_FLAG_HAS_MORE_INPUT 1
#define TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF 2
typedef struct { z_stream stream; int initialized; } tinfl_decompressor;
typedef enum {
    TINFL_STATUS_BAD_PARAM = -3,
    TINFL_STATUS_FAILED = -1,
    TINFL_STATUS_DONE = 0,
    TINFL_STATUS_NEEDS_MORE_INPUT = 1,
    TINFL_STATUS_HAS_MORE_OUTPUT = 2
} tinfl_status;
static inline void tinfl_init(tinfl_decompressor *decompressor) {
    memset(decompressor, 0, sizeof(*decompressor));
    decompressor->initialized = inflateInit2(&decompressor->stream, -MAX_WBITS) == Z_OK;
}
static inline tinfl_status tinfl_decompress(tinfl_decompressor *decompressor,
    const unsigned char *input, size_t *input_size, const unsigned char *output_start,
    unsigned char *output_next, size_t *output_size, mz_uint32 flags) {
    (void)output_start; (void)flags;
    if (!decompressor->initialized) return TINFL_STATUS_FAILED;
    z_stream *stream = &decompressor->stream;
    stream->next_in = const_cast<Bytef *>(input);
    stream->avail_in = static_cast<uInt>(*input_size);
    stream->next_out = output_next;
    stream->avail_out = static_cast<uInt>(*output_size);
    uInt inputBefore = stream->avail_in;
    uInt outputBefore = stream->avail_out;
    int result = inflate(stream, Z_NO_FLUSH);
    *input_size = inputBefore - stream->avail_in;
    *output_size = outputBefore - stream->avail_out;
    if (result == Z_STREAM_END) {
        inflateEnd(stream);
        decompressor->initialized = 0;
        return TINFL_STATUS_DONE;
    }
    if (result != Z_OK && result != Z_BUF_ERROR) {
        inflateEnd(stream);
        decompressor->initialized = 0;
        return TINFL_STATUS_FAILED;
    }
    if (stream->avail_out == 0) return TINFL_STATUS_HAS_MORE_OUTPUT;
    return TINFL_STATUS_NEEDS_MORE_INPUT;
}
static inline uint32_t mz_crc32(uint32_t crc, const unsigned char *buf, size_t len) { return static_cast<uint32_t>(crc32(crc, buf, static_cast<uInt>(len))); }
