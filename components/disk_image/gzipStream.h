#pragma once
// Streaming gunzip: compressed input in pieces (push()), the inflated bytes
// out through a callback, a 32 KB window instead of the whole output in
// memory. For main's OTA from the Sidecar's app.bin.gz (inflated into the
// OTA slot chunk by chunk) and the Sidecar's look at the start of that file
// (its app token). gzipImage.h is the whole-file inflater for disk images.
// The ROM's miniz (tinfl); state and window in PSRAM when there is some.
#include <stddef.h>
#include <stdint.h>

#include <functional>

class GzipStream {
public:
    // Gets each run of inflated bytes, in order. Return false to stop
    // (push() then returns false, and stopped() is true).
    using Sink = std::function<bool(const uint8_t *data, size_t length)>;

    explicit GzipStream(Sink sink);
    ~GzipStream();
    GzipStream(const GzipStream &) = delete;
    GzipStream &operator=(const GzipStream &) = delete;

    // Feed the next piece of the gzip file. False on an error or a stop.
    bool push(const uint8_t *data, size_t length);
    // All of the file given: true if the deflate stream ended and the
    // trailer (crc32 and length of the inflated data) matches.
    bool finish();

    bool stopped() const { return stopped_; }
    const char *error() const { return error_; }
    uint32_t outBytes() const { return outBytes_; }

private:
    enum class State : uint8_t { Header, Inflate, Done, Failed };   // Done: the deflate stream ended
    bool fail(const char *why);
    bool parseHeader();   // from header_: true once complete (or failed)
    bool inflate(const uint8_t *&data, size_t &length);

    Sink sink_;
    State state_ = State::Header;
    void *inflater_ = nullptr;   // tinfl_decompressor
    uint8_t *window_ = nullptr;  // TINFL_LZ_DICT_SIZE, wrapping
    size_t windowPos_ = 0;
    uint8_t header_[256];
    size_t headerLen_ = 0, headerNeed_ = 10;
    uint8_t tail_[8];        // the last bytes pushed: the trailer, at the end
    size_t tailLen_ = 0;
    uint32_t crc_ = 0, outBytes_ = 0;
    bool stopped_ = false;
    const char *error_ = nullptr;
};
