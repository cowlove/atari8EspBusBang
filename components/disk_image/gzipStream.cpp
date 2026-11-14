// See gzipStream.h.
#include "gzipStream.h"

#include "esp_heap_caps.h"
#include "miniz.h"

#include <algorithm>
#include <stdlib.h>
#include <string.h>

namespace {
void *allocate(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}
}  // namespace

GzipStream::GzipStream(Sink sink) : sink_(std::move(sink))
{
    inflater_ = allocate(sizeof(tinfl_decompressor));
    window_ = static_cast<uint8_t *>(allocate(TINFL_LZ_DICT_SIZE));
    if (!inflater_ || !window_) {
        fail("out of memory");
        return;
    }
    tinfl_init(static_cast<tinfl_decompressor *>(inflater_));
    crc_ = MZ_CRC32_INIT;
}

GzipStream::~GzipStream()
{
    heap_caps_free(inflater_);
    heap_caps_free(window_);
}

bool GzipStream::fail(const char *why)
{
    if (state_ != State::Failed) {
        error_ = why;
        state_ = State::Failed;
    }
    return false;
}

// The gzip header (RFC 1952): 10 fixed bytes, then the optional fields its
// flags name. Grows headerNeed_ until it's all in header_.
bool GzipStream::parseHeader()
{
    if (headerLen_ < 10) return false;
    if (header_[0] != 0x1f || header_[1] != 0x8b) return fail("not gzip");
    if (header_[2] != 8) return fail("not deflate");
    const uint8_t flags = header_[3];
    if (flags & 0xe0) return fail("reserved header flags");
    size_t at = 10;
    if (flags & 0x04) {   // FEXTRA
        if (headerLen_ < at + 2) return (headerNeed_ = at + 2), false;
        at += 2 + (header_[at] | (header_[at + 1] << 8));
    }
    for (const uint8_t f : {uint8_t(0x08), uint8_t(0x10)}) {   // FNAME, FCOMMENT: NUL-terminated
        if (!(flags & f)) continue;
        while (true) {
            if (headerLen_ <= at) return (headerNeed_ = at + 1), false;
            if (header_[at++] == 0) break;
        }
    }
    if (flags & 0x02) at += 2;   // FHCRC
    if (at > sizeof(header_)) return fail("header too long");
    if (headerLen_ < at) return (headerNeed_ = at), false;
    headerNeed_ = at;
    return true;
}

bool GzipStream::inflate(const uint8_t *&data, size_t &length)
{
    auto *d = static_cast<tinfl_decompressor *>(inflater_);
    for (;;) {
        size_t in = length;
        size_t out = TINFL_LZ_DICT_SIZE - windowPos_;
        const tinfl_status status =
            tinfl_decompress(d, data, &in, window_, window_ + windowPos_, &out, TINFL_FLAG_HAS_MORE_INPUT);
        data += in;
        length -= in;
        if (out) {
            crc_ = static_cast<uint32_t>(mz_crc32(crc_, window_ + windowPos_, out));
            outBytes_ += out;
            const bool more = sink_(window_ + windowPos_, out);
            windowPos_ = (windowPos_ + out) & (TINFL_LZ_DICT_SIZE - 1);
            if (!more) {
                stopped_ = true;
                return fail("stopped");
            }
        }
        if (status == TINFL_STATUS_DONE) {
            state_ = State::Done;
            return true;
        }
        if (status < TINFL_STATUS_DONE) return fail("bad deflate data");
        if (status == TINFL_STATUS_NEEDS_MORE_INPUT && length == 0) return true;
        // TINFL_STATUS_HAS_MORE_OUTPUT (the window filled), or input left: again.
    }
}

bool GzipStream::push(const uint8_t *data, size_t length)
{
    // The trailer is the file's last 8 bytes. tinfl can't be trusted to
    // leave them: its fast path reads 4 bytes ahead into its bit buffer, so
    // the end of the deflate stream may already have eaten some of them,
    // depending on where the pieces split. So keep the last 8 bytes seen.
    if (length >= sizeof(tail_)) {
        memcpy(tail_, data + length - sizeof(tail_), sizeof(tail_));
        tailLen_ = sizeof(tail_);
    } else {
        const size_t keep = std::min(tailLen_, sizeof(tail_) - length);
        memmove(tail_, tail_ + tailLen_ - keep, keep);
        memcpy(tail_ + keep, data, length);
        tailLen_ = keep + length;
    }
    while (length) {
        switch (state_) {
        case State::Header: {
            const size_t take = std::min(length, headerNeed_ - headerLen_);
            if (headerLen_ + take > sizeof(header_)) return fail("header too long");
            memcpy(header_ + headerLen_, data, take);
            headerLen_ += take;
            data += take;
            length -= take;
            if (headerLen_ == headerNeed_ && parseHeader()) {
                state_ = State::Inflate;
            } else if (state_ == State::Failed) {
                return false;
            }
            break;
        }
        case State::Inflate:
            if (!inflate(data, length)) return false;
            break;
        case State::Done:
            return true;   // the trailer (kept in tail_), or anything after it
        case State::Failed:
            return false;
        }
    }
    return state_ != State::Failed;
}

bool GzipStream::finish()
{
    if (state_ == State::Failed) return false;
    if (state_ != State::Done || tailLen_ < sizeof(tail_)) return fail("truncated");
    // Assumes nothing follows the trailer (one member, no padding).
    uint32_t crc, size;
    memcpy(&crc, tail_, 4);
    memcpy(&size, tail_ + 4, 4);
    if (crc != crc_) return fail("crc32 mismatch");
    if (size != outBytes_) return fail("length mismatch");
    return true;
}
