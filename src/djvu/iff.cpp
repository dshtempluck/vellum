#include "djvu/iff.h"

#include <cstring>

namespace vellum::djvu {

bool ChunkReader::Next(Chunk* out) {
  // A trailing pad byte can leave fewer than 8 bytes; that is the end.
  if (r_.remaining() < 8) return false;

  const size_t header_at = r_.pos();
  ByteSpan id = r_.take(4);
  const uint32_t len = r_.u32();
  if (len > r_.remaining())
    throw FormatError("IFF chunk length past end of container");

  Chunk c;
  std::memcpy(c.id, id.data(), 4);
  c.offset = header_at;

  if (c.is("FORM") || c.is("LIST") || c.is("PROP") || c.is("CAT ")) {
    if (len < 4) throw FormatError("IFF composite chunk too short");
    ByteSpan sec = r_.take(4);
    std::memcpy(c.form_id, sec.data(), 4);
    c.body = r_.take(len - 4);
  } else {
    c.body = r_.take(len);
  }

  if (len & 1) {
    // Pad byte. A file that ends exactly on the pad is still well formed.
    if (r_.remaining() >= 1) r_.u8();
  }

  *out = c;
  return true;
}

Chunk OpenDjVuFile(ByteSpan file) {
  if (file.size() < 12) throw FormatError("not a DjVu file: too short");
  if (std::memcmp(file.data(), "AT&T", 4) != 0)
    throw FormatError("not a DjVu file: missing AT&T signature");

  ChunkReader r(file.subspan(4, file.size() - 4));
  Chunk c;
  if (!r.Next(&c) || !c.is("FORM"))
    throw FormatError("not a DjVu file: no root FORM chunk");
  return c;
}

}  // namespace vellum::djvu
