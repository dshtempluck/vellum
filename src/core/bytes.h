// Byte-level primitives shared by the format back ends.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace vellum {

// A non-owning view over bytes. Reads past the end are a hard error rather
// than undefined behaviour: document files are untrusted input.
class ByteSpan {
 public:
  ByteSpan() = default;
  ByteSpan(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  uint8_t operator[](size_t i) const {
    if (i >= size_) throw std::out_of_range("ByteSpan index");
    return data_[i];
  }

  ByteSpan subspan(size_t off, size_t len) const {
    if (off > size_ || len > size_ - off)
      throw std::out_of_range("ByteSpan subspan");
    return ByteSpan(data_ + off, len);
  }

 private:
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
};

// Sequential big-endian reader. DjVu (like IFF generally) is big-endian.
class ByteReader {
 public:
  explicit ByteReader(ByteSpan s) : s_(s) {}

  size_t pos() const { return pos_; }
  size_t remaining() const { return s_.size() - pos_; }
  bool eof() const { return pos_ >= s_.size(); }
  void seek(size_t p) {
    if (p > s_.size()) throw std::out_of_range("ByteReader seek");
    pos_ = p;
  }

  uint8_t u8() {
    if (pos_ >= s_.size()) throw std::out_of_range("ByteReader u8");
    return s_.data()[pos_++];
  }
  uint16_t u16() { return static_cast<uint16_t>(u8() << 8 | u8()); }
  uint32_t u24() { return static_cast<uint32_t>(u8() << 16 | u8() << 8 | u8()); }
  uint32_t u32() {
    uint32_t v = u8();
    v = v << 8 | u8();
    v = v << 8 | u8();
    return v << 8 | u8();
  }

  // Reads a NUL-terminated string, consuming the terminator.
  std::string cstr() {
    std::string out;
    while (true) {
      uint8_t c = u8();
      if (c == 0) break;
      out.push_back(static_cast<char>(c));
    }
    return out;
  }

  ByteSpan take(size_t n) {
    ByteSpan r = s_.subspan(pos_, n);
    pos_ += n;
    return r;
  }

 private:
  ByteSpan s_;
  size_t pos_ = 0;
};

// Thrown for malformed input. Callers degrade gracefully: a page that fails
// to decode is reported as such instead of taking down the viewer.
class FormatError : public std::runtime_error {
 public:
  explicit FormatError(const std::string& what)
      : std::runtime_error(what) {}
};

std::vector<uint8_t> ReadWholeFile(const std::wstring& path);

}  // namespace vellum
