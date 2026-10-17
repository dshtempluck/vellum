// Test helpers: locating the sample corpus, temporary directories, and
// synthetic documents built in memory.
//
// Most of the suite runs with no external files at all: BMP, PDF and IFF
// containers are simple enough to construct here, which keeps those tests
// hermetic. The DjVu codec tests need real encoded streams, so they use the
// fetched corpus and skip with a clear message when it is absent.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vellum::test {

// The repository's samples/ directory, whether or not it has been fetched.
std::wstring SampleDir();

// Full path to a sample, e.g. Sample(L"djvu3spec.djvu").
std::wstring Sample(const wchar_t* name);

// True when the DjVu corpus has been fetched. Codec tests check this and
// skip rather than fail, so a fresh clone still runs the rest.
bool HasCorpus();

// Reads a whole file; empty when it does not exist.
std::vector<uint8_t> ReadFile(const std::wstring& path);

// A directory that deletes itself and its contents when it goes out of
// scope, for tests that must touch the filesystem.
class TempDir {
 public:
  TempDir();
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::wstring& path() const { return path_; }

  // Writes `bytes` to `name` inside the directory and returns its path.
  std::wstring Write(const wchar_t* name, const std::vector<uint8_t>& bytes);

 private:
  std::wstring path_;
};

// --- synthetic documents ---------------------------------------------------

// A 24-bit uncompressed BMP of a single colour. BMP is simple enough to
// emit by hand, which gives the image backend something to decode without
// committing a binary fixture or depending on an encoder.
std::vector<uint8_t> MakeBmp(int width, int height, uint8_t r, uint8_t g,
                             uint8_t b);

// A one-page PDF with the given page box, optionally painting the whole
// page a solid colour so a render can be checked against known pixels.
std::vector<uint8_t> MakePdf(double width_pt = 612, double height_pt = 792,
                             bool fill_red = false);

// A PDF with an outline: two top-level bookmarks, the first with a child,
// plus a document title. Exercises the bookmark walk, the destination
// lookup and the UTF-16 metadata conversion.
std::vector<uint8_t> MakePdfWithOutline();

// A minimal baseline grayscale TIFF carrying an Orientation tag. TIFF is
// the simplest container that can hold one, which makes it the practical
// way to exercise the EXIF-orientation handling without an encoder.
std::vector<uint8_t> MakeTiff(int width, int height, int orientation);

// Builds an IFF chunk: a four-character id, then a big-endian length, then
// the body, padded to an even length.
std::vector<uint8_t> MakeChunk(const char id[5],
                               const std::vector<uint8_t>& body);

// Wraps `body` in FORM:<form_id>, prefixed with the DjVu "AT&T" magic when
// `with_magic` is set.
std::vector<uint8_t> MakeForm(const char form_id[5],
                              const std::vector<uint8_t>& body,
                              bool with_magic = true);

// Appends a big-endian integer, for building chunk bodies by hand.
void PutU16(std::vector<uint8_t>* out, uint32_t v);
void PutU24(std::vector<uint8_t>* out, uint32_t v);
void PutU32(std::vector<uint8_t>* out, uint32_t v);

}  // namespace vellum::test
