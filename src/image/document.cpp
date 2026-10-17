#include "image/document.h"

#include <windows.h>

#include <shlwapi.h>
#include <wincodec.h>

#include <algorithm>
#include <cstring>

#include "core/bytes.h"

namespace vellum::image {
namespace {

// --- COM and WIC plumbing --------------------------------------------------

// Every thread that touches WIC needs COM initialised. The render worker is
// not the UI thread, so this is done per thread and left in place for the
// thread's lifetime.
void EnsureCom() {
  struct Guard {
    Guard() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
  };
  thread_local Guard guard;
}

// Minimal owning pointer; avoids pulling in WRL for five interfaces.
template <typename T>
class Com {
 public:
  Com() = default;
  ~Com() { reset(); }
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }

  T** put() {
    reset();
    return &p_;
  }
  T* get() const { return p_; }
  T* operator->() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }
  void reset() {
    if (p_) p_->Release();
    p_ = nullptr;
  }

 private:
  T* p_ = nullptr;
};

IWICImagingFactory* Factory() {
  EnsureCom();
  // One factory per thread: the interface is agile but caching avoids a
  // CoCreateInstance on every page.
  thread_local IWICImagingFactory* factory = [] {
    IWICImagingFactory* f = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&f));
    return f;
  }();
  return factory;
}

// --- EXIF orientation ------------------------------------------------------

// EXIF orientation codes describe how a camera held the sensor. 1 is
// upright; the rest are rotations and mirrorings that have to be undone
// before the picture looks right.
int ReadExifOrientation(IWICBitmapFrameDecode* frame) {
  Com<IWICMetadataQueryReader> reader;
  if (FAILED(frame->GetMetadataQueryReader(reader.put())) || !reader) return 1;

  // JPEG hangs the tag off its APP1 segment; TIFF keeps it in the IFD
  // directly. Both are tried, in that order.
  static const wchar_t* kPaths[] = {L"/app1/ifd/{ushort=274}",
                                    L"/ifd/{ushort=274}"};
  for (const wchar_t* path : kPaths) {
    PROPVARIANT v;
    PropVariantInit(&v);
    const bool ok = SUCCEEDED(reader->GetMetadataByName(path, &v));

    // A query can succeed and still carry nothing, so the value counts
    // only when the variant actually holds an integer. Treating an empty
    // result as "upright" would stop the search here and never reach the
    // path that does have the tag.
    int value = 0;
    if (ok) {
      if (v.vt == VT_UI2) value = v.uiVal;
      else if (v.vt == VT_UI4) value = static_cast<int>(v.ulVal);
      else if (v.vt == VT_I4) value = v.lVal;
      else if (v.vt == VT_I2) value = v.iVal;
    }
    PropVariantClear(&v);
    if (value >= 1 && value <= 8) return value;
  }
  return 1;
}

bool OrientationSwapsAxes(int exif) {
  return exif == 5 || exif == 6 || exif == 7 || exif == 8;
}

// Combines the EXIF correction with the viewer's own rotation into the
// single flip/rotate WIC can apply in one pass.
WICBitmapTransformOptions TransformFor(int exif, int view_rotation) {
  // EXIF code -> (quarter turns clockwise, mirrored horizontally)
  int quarters = 0;
  bool mirror = false;
  switch (exif) {
    case 2: mirror = true; break;
    case 3: quarters = 2; break;
    case 4: quarters = 2; mirror = true; break;
    case 5: quarters = 1; mirror = true; break;
    case 6: quarters = 1; break;
    case 7: quarters = 3; mirror = true; break;
    case 8: quarters = 3; break;
    default: break;
  }
  quarters = (quarters + (((view_rotation % 360) + 360) % 360) / 90) % 4;

  unsigned options = WICBitmapTransformRotate0;
  switch (quarters) {
    case 1: options = WICBitmapTransformRotate90; break;
    case 2: options = WICBitmapTransformRotate180; break;
    case 3: options = WICBitmapTransformRotate270; break;
    default: break;
  }
  if (mirror) options |= WICBitmapTransformFlipHorizontal;
  return static_cast<WICBitmapTransformOptions>(options);
}

// --- file identification ---------------------------------------------------

std::wstring LowerExtension(const std::wstring& path) {
  const size_t dot = path.find_last_of(L'.');
  if (dot == std::wstring::npos) return std::wstring();
  std::wstring ext = path.substr(dot);
  for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
  return ext;
}

const wchar_t* const kExtensions[] = {
    L".jpg", L".jpeg", L".jpe",  L".jfif", L".png",  L".bmp", L".dib",
    L".gif", L".tif",  L".tiff", L".webp", L".heic", L".heif", L".avif",
    L".ico", L".jxr",  L".wdp",
};

std::string FormatNameForExtension(const std::wstring& ext) {
  if (ext == L".jpg" || ext == L".jpeg" || ext == L".jpe" || ext == L".jfif")
    return "JPEG image";
  if (ext == L".png") return "PNG image";
  if (ext == L".bmp" || ext == L".dib") return "Bitmap image";
  if (ext == L".gif") return "GIF image";
  if (ext == L".tif" || ext == L".tiff") return "TIFF image";
  if (ext == L".webp") return "WebP image";
  if (ext == L".heic" || ext == L".heif") return "HEIF image";
  if (ext == L".avif") return "AVIF image";
  if (ext == L".ico") return "Icon";
  return "Image";
}

std::string NarrowPath(const std::wstring& w) {
  if (w.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                    static_cast<int>(w.size()), nullptr, 0,
                                    nullptr, nullptr);
  std::string out(static_cast<size_t>(std::max(0, n)), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                      out.data(), n, nullptr, nullptr);
  return out;
}

}  // namespace

bool HasImageExtension(const std::wstring& path) {
  const std::wstring ext = LowerExtension(path);
  for (const wchar_t* e : kExtensions)
    if (ext == e) return true;
  return false;
}

bool LooksLikeImage(const uint8_t* d, size_t n) {
  if (n < 12) return false;
  if (d[0] == 0xff && d[1] == 0xd8 && d[2] == 0xff) return true;      // JPEG
  if (!std::memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return true;           // PNG
  if (d[0] == 'B' && d[1] == 'M') return true;                        // BMP
  if (!std::memcmp(d, "GIF87a", 6) || !std::memcmp(d, "GIF89a", 6))
    return true;                                                      // GIF
  if (!std::memcmp(d, "II\x2a\x00", 4) || !std::memcmp(d, "MM\x00\x2a", 4))
    return true;                                                      // TIFF
  if (!std::memcmp(d, "RIFF", 4) && !std::memcmp(d + 8, "WEBP", 4))
    return true;                                                      // WebP
  // ISO base media: HEIF/AVIF carry their brand in the ftyp box.
  if (!std::memcmp(d + 4, "ftyp", 4)) {
    const char* brand = reinterpret_cast<const char*>(d + 8);
    if (!std::memcmp(brand, "heic", 4) || !std::memcmp(brand, "heif", 4) ||
        !std::memcmp(brand, "heix", 4) || !std::memcmp(brand, "mif1", 4) ||
        !std::memcmp(brand, "avif", 4))
      return true;
  }
  return false;
}

std::unique_ptr<ImageDocument> ImageDocument::Open(const std::wstring& path) {
  EnsureCom();
  if (!Factory()) throw FormatError("the imaging component is unavailable");

  auto doc = std::unique_ptr<ImageDocument>(new ImageDocument());

  const size_t slash = path.find_last_of(L"\\/");
  doc->folder_ =
      (slash == std::wstring::npos) ? L"." : path.substr(0, slash);
  const std::wstring opened =
      (slash == std::wstring::npos) ? path : path.substr(slash + 1);

  // Collect every image sitting beside this one.
  WIN32_FIND_DATAW find{};
  const std::wstring pattern = doc->folder_ + L"\\*";
  HANDLE h = FindFirstFileW(pattern.c_str(), &find);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      if (!HasImageExtension(find.cFileName)) continue;
      doc->files_.emplace_back(find.cFileName);
      // A folder of tens of thousands of images would make measuring every
      // page pointless work; stop at a sane ceiling.
      if (doc->files_.size() >= 5000) break;
    } while (FindNextFileW(h, &find));
    FindClose(h);
  }

  // The file that was asked for always opens, even if the scan missed it
  // (an unusual extension, a folder we could not enumerate).
  if (std::find(doc->files_.begin(), doc->files_.end(), opened) ==
      doc->files_.end())
    doc->files_.push_back(opened);

  // Sort the way Explorer does, so img2 comes before img10.
  std::sort(doc->files_.begin(), doc->files_.end(),
            [](const std::wstring& a, const std::wstring& b) {
              return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
            });

  const auto it = std::find(doc->files_.begin(), doc->files_.end(), opened);
  doc->initial_ = static_cast<int>(
      (it == doc->files_.end()) ? 0 : std::distance(doc->files_.begin(), it));

  const size_t n = doc->files_.size();
  doc->geometry_.resize(n);
  doc->exif_.assign(n, 1);
  doc->measured_.assign(n, false);

  // The requested file must actually decode, or this is not an image
  // document and the caller should hear about it.
  doc->LoadGeometry(doc->initial_);
  if (doc->geometry_[doc->initial_].width_pt <= 0)
    throw FormatError("this image could not be decoded");

  return doc;
}

void ImageDocument::LoadGeometry(int page) const {
  // Caller must hold mu_, or be in Open before the document is shared.
  if (page < 0 || page >= static_cast<int>(files_.size())) return;
  if (measured_[page]) return;
  measured_[page] = true;

  IWICImagingFactory* factory = Factory();
  if (!factory) return;

  const std::wstring full = folder_ + L"\\" + files_[page];
  Com<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromFilename(
          full.c_str(), nullptr, GENERIC_READ,
          WICDecodeMetadataCacheOnDemand, decoder.put())) ||
      !decoder)
    return;

  Com<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, frame.put())) || !frame) return;

  UINT w = 0, h = 0;
  if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) return;

  const int exif = ReadExifOrientation(frame.get());
  exif_[page] = exif;
  if (OrientationSwapsAxes(exif)) std::swap(w, h);

  double dpi_x = 96, dpi_y = 96;
  if (FAILED(frame->GetResolution(&dpi_x, &dpi_y)) || dpi_x < 1 ||
      dpi_y < 1 || dpi_x > 10000 || dpi_y > 10000) {
    dpi_x = dpi_y = 96;
  }

  // Report the size in points so that "actual size" shows the picture at
  // its true physical dimensions, exactly as it does for a PDF page.
  PageGeometry g;
  g.width_pt = w * 72.0 / dpi_x;
  g.height_pt = h * 72.0 / dpi_y;
  g.rotation = 0;  // orientation is baked into the reported size
  geometry_[page] = g;
}

PageGeometry ImageDocument::geometry(int page) const {
  std::lock_guard<std::mutex> lock(mu_);
  LoadGeometry(page);
  if (page < 0 || page >= static_cast<int>(geometry_.size()))
    return PageGeometry();
  PageGeometry g = geometry_[page];
  if (g.width_pt <= 0) {
    // An image that will not decode still needs a box to sit in, so the
    // rest of the folder stays usable.
    g.width_pt = 640 * 72.0 / 96.0;
    g.height_pt = 480 * 72.0 / 96.0;
  }
  return g;
}

RenderedPage ImageDocument::Render(int page, Size size, int rotation) {
  RenderedPage out;
  out.page = page;
  out.size = size;
  if (size.empty() || page < 0 || page >= static_cast<int>(files_.size())) {
    out.error = "page out of range";
    return out;
  }

  int exif = 1;
  std::wstring full;
  {
    std::lock_guard<std::mutex> lock(mu_);
    LoadGeometry(page);
    exif = exif_[page];
    full = folder_ + L"\\" + files_[page];
  }

  out.pixmap.Init(size.w, size.h);
  out.pixmap.Fill(0xff, 0xff, 0xff);

  IWICImagingFactory* factory = Factory();
  if (!factory) {
    out.error = "the imaging component is unavailable";
    return out;
  }

  Com<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromFilename(
          full.c_str(), nullptr, GENERIC_READ,
          WICDecodeMetadataCacheOnDemand, decoder.put())) ||
      !decoder) {
    out.error = "this image could not be opened";
    return out;
  }

  Com<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, frame.put())) || !frame) {
    out.error = "this image has no decodable frame";
    return out;
  }

  // Convert first so that everything downstream works in one format, and
  // so that transparency lands on white rather than on uninitialised
  // memory.
  Com<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(converter.put())) ||
      FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat24bppBGR,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
    out.error = "this image format is not supported";
    return out;
  }

  IWICBitmapSource* source = converter.get();

  // Orientation and view rotation in one step.
  Com<IWICBitmapFlipRotator> rotator;
  const WICBitmapTransformOptions transform = TransformFor(exif, rotation);
  if (transform != WICBitmapTransformRotate0) {
    if (SUCCEEDED(factory->CreateBitmapFlipRotator(rotator.put())) &&
        SUCCEEDED(rotator->Initialize(source, transform)))
      source = rotator.get();
  }

  // Resample to the requested size. Fant is WIC's good downscaler; for
  // upscaling it behaves like a linear filter, which is what we want.
  Com<IWICBitmapScaler> scaler;
  UINT sw = 0, sh = 0;
  source->GetSize(&sw, &sh);
  if (sw != static_cast<UINT>(size.w) || sh != static_cast<UINT>(size.h)) {
    if (SUCCEEDED(factory->CreateBitmapScaler(scaler.put())) &&
        SUCCEEDED(scaler->Initialize(source, size.w, size.h,
                                     WICBitmapInterpolationModeFant)))
      source = scaler.get();
  }

  const WICRect rect{0, 0, size.w, size.h};
  const HRESULT hr = source->CopyPixels(
      &rect, static_cast<UINT>(out.pixmap.stride()),
      static_cast<UINT>(out.pixmap.stride()) * size.h, out.pixmap.row(0));
  if (FAILED(hr)) {
    out.error = "this image could not be decoded";
    return out;
  }

  out.complete = true;
  return out;
}

std::string ImageDocument::format_name() const {
  if (files_.empty()) return "Image";
  // One file can be named precisely. A folder holds a mixture, and the
  // status bar already shows the current file's name, so describing the
  // collection avoids claiming a JPEG is a PNG after navigating.
  if (files_.size() == 1)
    return FormatNameForExtension(LowerExtension(files_[0]));
  return "Images (" + std::to_string(files_.size()) + " in folder)";
}

std::string ImageDocument::title() const {
  const size_t slash = folder_.find_last_of(L"\\/");
  const std::wstring name =
      (slash == std::wstring::npos) ? folder_ : folder_.substr(slash + 1);
  return NarrowPath(name);
}

std::string ImageDocument::page_label(int page) const {
  if (page < 0 || page >= static_cast<int>(files_.size()))
    return std::string();
  return NarrowPath(files_[page]);
}

}  // namespace vellum::image
