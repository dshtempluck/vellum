// Minimal IFF/EA85 chunk reader for DjVu containers.
//
// A DjVu file is "AT&T" followed by one FORM chunk. Chunk headers are a
// four-character ID and a big-endian 32-bit length; odd-length chunks are
// followed by one pad byte. Composite chunks (FORM) spend their first four
// bytes on a secondary ID -- DJVU for a page, DJVI for a shared dictionary,
// DJVM for a multi-page document, THUM for thumbnails.
#pragma once

#include <string>
#include <vector>

#include "core/bytes.h"

namespace vellum::djvu {

struct Chunk {
  char id[5] = {};       // chunk ID, NUL-terminated for convenience
  char form_id[5] = {};  // secondary ID when this is a FORM, else empty
  ByteSpan body;         // chunk payload, excluding the secondary ID
  size_t offset = 0;     // file offset of the chunk header

  bool is(const char* want) const {
    return id[0] == want[0] && id[1] == want[1] && id[2] == want[2] &&
           id[3] == want[3];
  }
  bool is_form(const char* want) const {
    return is("FORM") && form_id[0] == want[0] && form_id[1] == want[1] &&
           form_id[2] == want[2] && form_id[3] == want[3];
  }
};

// Iterates the chunks directly inside one container body.
class ChunkReader {
 public:
  explicit ChunkReader(ByteSpan body) : r_(body), base_(body) {}

  // Reads the next chunk. Returns false at the end of the container.
  bool Next(Chunk* out);

 private:
  ByteReader r_;
  ByteSpan base_;
};

// Strips the "AT&T" magic and returns the outermost FORM chunk.
Chunk OpenDjVuFile(ByteSpan file);

}  // namespace vellum::djvu
