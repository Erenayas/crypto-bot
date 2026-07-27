#include "gzfile.hpp"

#include <zlib.h>

#include <cstring>
#include <stdexcept>

namespace hft {
namespace {
constexpr std::size_t kChunk = 1 << 16;  // 64 KiB
}

// ------------------------------------------------------------------ GzWriter

GzWriter::GzWriter(const std::string& path) {
    // "wb1" = write, binary, compression level 1. Level 1 rather than the
    // default 6 on purpose: this runs on the live data path, and market data
    // JSON is so repetitive that level 1 already gets ~90% of the ratio for a
    // fraction of the CPU. We are recording, not archiving.
    f_ = gzopen(path.c_str(), "wb1");
    if (!f_) throw std::runtime_error("cannot open recording for writing: " + path);
}

GzWriter::~GzWriter() {
    if (f_) gzclose(f_);
}

void GzWriter::write_line(std::string_view s) {
    if (gzwrite(f_, s.data(), static_cast<unsigned>(s.size())) == 0) {
        throw std::runtime_error("gzwrite failed");
    }
    if (gzwrite(f_, "\n", 1) == 0) {
        throw std::runtime_error("gzwrite failed");
    }
}

void GzWriter::flush() {
    // Z_SYNC_FLUSH ends the current deflate block so everything written so far
    // is recoverable from the file. Without it, killing the process loses the
    // tail of the recording -- which is exactly the part you wanted.
    gzflush(f_, Z_SYNC_FLUSH);
}

// -------------------------------------------------------------- GzLineReader

GzLineReader::GzLineReader(const std::string& path) {
    f_ = gzopen(path.c_str(), "rb");
    if (!f_) throw std::runtime_error("cannot open recording for reading: " + path);
}

GzLineReader::~GzLineReader() {
    if (f_) gzclose(f_);
}

bool GzLineReader::next(std::string_view& line) {
    for (;;) {
        const std::size_t avail = end_ - begin_;
        if (avail > 0) {
            const char* base = buf_.data();
            if (const void* nl = std::memchr(base + begin_, '\n', avail)) {
                const auto off = static_cast<std::size_t>(static_cast<const char*>(nl) - base);
                line   = std::string_view(base + begin_, off - begin_);
                begin_ = off + 1;
                return true;
            }
        }
        if (eof_) {
            // A final line with no trailing newline is still a line. This
            // happens whenever a recording was killed mid-write.
            if (avail > 0) {
                line   = std::string_view(buf_.data() + begin_, avail);
                begin_ = end_;
                return true;
            }
            return false;
        }
        fill();
    }
}

bool GzLineReader::fill() {
    // Slide unconsumed bytes to the front so the buffer only has to grow when a
    // single LINE is longer than it, not when the file is.
    if (begin_ > 0) {
        std::memmove(buf_.data(), buf_.data() + begin_, end_ - begin_);
        end_ -= begin_;
        begin_ = 0;
    }
    if (end_ == buf_.size()) {
        buf_.resize(buf_.empty() ? kChunk : buf_.size() * 2);
    }

    const int got = gzread(f_, buf_.data() + end_, static_cast<unsigned>(buf_.size() - end_));
    if (got < 0) {
        int         errnum = 0;
        const char* msg    = gzerror(f_, &errnum);
        // A recording killed mid-write ends in a partial deflate block. Every
        // byte up to the last Z_SYNC_FLUSH is still perfectly good data, so we
        // stop cleanly and flag it rather than throwing away the whole file.
        // Losing three hours of market data to a truncated last line would be
        // an absurd way to fail.
        if (errnum == Z_BUF_ERROR || errnum == Z_DATA_ERROR) {
            truncated_ = true;
            eof_       = true;
            return false;
        }
        throw std::runtime_error(std::string("gzread failed: ") + (msg ? msg : "unknown"));
    }
    if (got == 0) {
        eof_ = true;
        return false;
    }
    end_ += static_cast<std::size_t>(got);
    return true;
}

}  // namespace hft
