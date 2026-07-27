#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// zlib's gzFile is a pointer to this opaque struct. Forward-declaring it keeps
// <zlib.h> out of our headers, the same discipline we apply to Boost.
struct gzFile_s;

namespace hft {

// Line-oriented gzip writer. One JSON object per line (JSONL), so a recording
// can be inspected with `zcat file.jsonl.gz | head`.
class GzWriter {
public:
    explicit GzWriter(const std::string& path);  // throws on failure
    ~GzWriter();
    GzWriter(const GzWriter&)            = delete;
    GzWriter& operator=(const GzWriter&) = delete;

    void write_line(std::string_view s);  // appends '\n'
    void flush();

private:
    gzFile_s* f_ = nullptr;
};

// Line-oriented gzip reader.
//
// Decompresses in chunks and hands out views into an internal buffer, so
// reading a multi-gigabyte recording never loads more than one chunk at a time.
class GzLineReader {
public:
    explicit GzLineReader(const std::string& path);  // throws on failure
    ~GzLineReader();
    GzLineReader(const GzLineReader&)            = delete;
    GzLineReader& operator=(const GzLineReader&) = delete;

    // Returns false at end of file. The view is valid only until the next
    // call -- the buffer is compacted and may reallocate underneath it.
    bool next(std::string_view& line);

    // True if the file ended in a partial deflate block, i.e. the recorder was
    // killed mid-write. The data read before that point is still valid.
    bool truncated() const { return truncated_; }

private:
    bool fill();

    gzFile_s*   f_ = nullptr;
    std::string buf_;
    std::size_t begin_     = 0;  // start of unconsumed data
    std::size_t end_       = 0;  // end of valid data
    bool        eof_       = false;
    bool        truncated_ = false;
};

}  // namespace hft
