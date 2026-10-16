// BZZ: the general-purpose byte compressor DjVu uses for its non-image
// chunks -- the document directory (DIRM), text layers (TXTz), the outline
// (NAVM) and annotations (ANTz).
//
// It is a Burrows-Wheeler transform followed by a move-to-front stage whose
// symbol ranks are entropy-coded with the ZP coder. Decoding inverts that:
// read ranks, undo the MTF, then undo the sort.
#pragma once

#include <cstdint>
#include <vector>

#include "core/bytes.h"

namespace vellum::djvu {

// Decompresses a complete BZZ stream.
std::vector<uint8_t> BzzDecompress(ByteSpan data);

}  // namespace vellum::djvu
