#include "djvu/jb2.h"

#include <algorithm>
#include <cstring>

namespace vellum::djvu {
namespace {

// Record types, in the order the bitstream numbers them.
enum RecordType {
  kStartOfData = 0,
  kNewMark = 1,
  kNewMarkLibraryOnly = 2,
  kNewMarkImageOnly = 3,
  kMatchedRefine = 4,
  kMatchedRefineLibraryOnly = 5,
  kMatchedRefineImageOnly = 6,
  kMatchedCopy = 7,
  kNonMarkData = 8,
  kRequiredDictOrReset = 9,
  kPreservedComment = 10,
  kEndOfData = 11,
};

// The range the format uses to mean "any number".
constexpr int kNumHigh = 262142;
constexpr int kNumLow = -262143;

constexpr int kCellChunk = 20000;

// Ink bounding box of a library shape, in the shape's own coordinates.
struct LibRect {
  int left = 0, right = 0, top = 0, bottom = 0;

  void Compute(const Bitmap& bm) {
    const int w = bm.cols();
    const int h = bm.rows();
    int r = w - 1;
    for (; r >= 0; --r) {
      bool ink = false;
      for (int y = 0; y < h && !ink; ++y) ink = bm.row(y)[r] != 0;
      if (ink) break;
    }
    int t = h - 1;
    for (; t >= 0; --t) {
      bool ink = false;
      const uint8_t* p = bm.row(t);
      for (int x = 0; x < w && !ink; ++x) ink = p[x] != 0;
      if (ink) break;
    }
    int l = 0;
    for (; l < r; ++l) {
      bool ink = false;
      for (int y = 0; y < h && !ink; ++y) ink = bm.row(y)[l] != 0;
      if (ink) break;
    }
    int b = 0;
    for (; b < t; ++b) {
      bool ink = false;
      const uint8_t* p = bm.row(b);
      for (int x = 0; x < w && !ink; ++x) ink = p[x] != 0;
      if (ink) break;
    }
    left = l;
    right = r;
    top = t;
    bottom = b;
  }
};

// Pixel context for coding a shape from scratch: two fully decoded rows
// above plus the part of the current row already decoded.
inline int DirectContext(const uint8_t* up2, const uint8_t* up1,
                         const uint8_t* up0, int col) {
  return (up2[col - 1] << 9) | (up2[col] << 8) | (up2[col + 1] << 7) |
         (up1[col - 2] << 6) | (up1[col - 1] << 5) | (up1[col] << 4) |
         (up1[col + 1] << 3) | (up1[col + 2] << 2) | (up0[col - 2] << 1) |
         (up0[col - 1] << 0);
}

// Slides the direct context one column right, folding in the bit just
// decoded. The mask drops the three bits that fall out of the window.
inline int ShiftDirectContext(int ctx, int next, const uint8_t* up2,
                              const uint8_t* up1, int col) {
  return ((ctx << 1) & 0x37a) | (up1[col + 2] << 2) | (up2[col + 1] << 7) |
         (next << 0);
}

// Pixel context for refining an existing shape: neighbours in the bitmap
// being decoded plus a 3x3 window of the shape being refined.
inline int CrossContext(const uint8_t* up1, const uint8_t* up0,
                        const uint8_t* xup1, const uint8_t* xup0,
                        const uint8_t* xdn1, int col) {
  return (up1[col - 1] << 10) | (up1[col] << 9) | (up1[col + 1] << 8) |
         (up0[col - 1] << 7) | (xup1[col] << 6) | (xup0[col - 1] << 5) |
         (xup0[col] << 4) | (xup0[col + 1] << 3) | (xdn1[col - 1] << 2) |
         (xdn1[col] << 1) | (xdn1[col + 1] << 0);
}

inline int ShiftCrossContext(int ctx, int n, const uint8_t* up1,
                             const uint8_t* xup1, const uint8_t* xup0,
                             const uint8_t* xdn1, int col) {
  return ((ctx << 1) & 0x636) | (up1[col + 1] << 8) | (xup1[col] << 6) |
         (xup0[col + 1] << 3) | (xdn1[col + 1] << 0) | (n << 7);
}

// Decodes one JB2 chunk. A dictionary chunk yields shapes only; a page
// chunk yields shapes plus the blits that place them.
class Jb2Decoder {
 public:
  explicit Jb2Decoder(ByteSpan data) : zp_(data) {
    // Cell 0 is a dummy, so that a zero context means "not yet allocated".
    bitcells_.assign(kCellChunk, 0);
    leftcell_.assign(kCellChunk, 0);
    rightcell_.assign(kCellChunk, 0);
    cur_ncell_ = 1;
  }

  void Run(Jb2Dict* dict, Jb2Image* image);

 private:
  using NumContext = uint32_t;

  int CodeNum(int low, int high, NumContext* pctx);
  int CodeBit(BitContext& ctx) { return zp_.Decode(ctx); }

  void ResetNumCoder();
  void InitLibrary(const Jb2Dict& dict);
  int AddLibrary(int shapeno, const Jb2Shape& shape);
  int CodeMatchIndex(int* shapeno);

  void CodeImageSizeCommon(bool is_image);
  std::string CodeComment();
  void CodeAbsoluteMarkSize(Bitmap& bm, int border);
  void CodeRelativeMarkSize(Bitmap& bm, int cw, int ch, int border);
  void CodeRelativeLocation(Jb2Blit* blit, int rows, int cols);
  void CodeAbsoluteLocation(Jb2Blit* blit, int rows, int cols);
  void CodeBitmapDirectly(Bitmap& bm);
  void CodeBitmapByCrossCoding(Bitmap& bm, Bitmap cbm, int libno);
  void CodeInheritedShapeCount();

  void FillShortList(int v) {
    short_list_[0] = short_list_[1] = short_list_[2] = v;
    short_list_pos_ = 0;
  }
  int UpdateShortList(int v);

  const Jb2Dict& target() const { return image_ ? *image_ : *dict_; }
  Jb2Dict& mutable_target() { return image_ ? *image_ : *dict_; }

  ZpDecoder zp_;

  // Number-coder cell pool: each coding decision gets an adaptive cell,
  // chained into a binary tree so contexts specialise by value prefix.
  std::vector<BitContext> bitcells_;
  std::vector<NumContext> leftcell_;
  std::vector<NumContext> rightcell_;
  int cur_ncell_ = 1;

  // One number context per coded quantity.
  NumContext dist_comment_byte_ = 0;
  NumContext dist_comment_length_ = 0;
  NumContext dist_record_type_ = 0;
  NumContext dist_match_index_ = 0;
  BitContext dist_refinement_flag_ = 0;
  NumContext abs_loc_x_ = 0;
  NumContext abs_loc_y_ = 0;
  NumContext abs_size_x_ = 0;
  NumContext abs_size_y_ = 0;
  NumContext image_size_dist_ = 0;
  NumContext inherited_shape_count_dist_ = 0;
  BitContext offset_type_dist_ = 0;
  NumContext rel_loc_x_current_ = 0;
  NumContext rel_loc_x_last_ = 0;
  NumContext rel_loc_y_current_ = 0;
  NumContext rel_loc_y_last_ = 0;
  NumContext rel_size_x_ = 0;
  NumContext rel_size_y_ = 0;

  // Pixel contexts: 10 bits of neighbourhood for direct coding, 11 for
  // refinement.
  BitContext bitdist_[1024] = {};
  BitContext cbitdist_[2048] = {};

  // Library: the shapes currently available for matching, and their ink
  // bounding boxes (used to centre refinements and relative placements).
  std::vector<int> lib2shape_;
  std::vector<int> shape2lib_;
  std::vector<LibRect> libinfo_;

  // Placement state driving relative coordinate coding.
  int image_columns_ = 0;
  int image_rows_ = 0;
  int last_left_ = 0;
  int last_right_ = 0;
  int last_bottom_ = 0;
  int last_row_left_ = 0;
  int last_row_bottom_ = 0;
  int short_list_[3] = {};
  int short_list_pos_ = 0;

  bool got_start_record_ = false;
  bool refinement_ = false;
  Jb2Dict* dict_ = nullptr;
  Jb2Image* image_ = nullptr;
};

int Jb2Decoder::CodeNum(int low, int high, NumContext* pctx) {
  if (!pctx || static_cast<int>(*pctx) >= cur_ncell_)
    throw FormatError("JB2 bad number context");

  bool negative = false;
  int cutoff = 0;
  int phase = 1;
  unsigned range = 0xffffffffu;

  while (range != 1) {
    // Allocate this decision's cell the first time it is reached.
    if (*pctx == 0) {
      if (cur_ncell_ >= static_cast<int>(bitcells_.size())) {
        // Growing the pool moves it, so re-seat pctx afterwards.
        const ptrdiff_t off_left = pctx - leftcell_.data();
        const ptrdiff_t off_right = pctx - rightcell_.data();
        const bool in_left =
            off_left >= 0 &&
            off_left < static_cast<ptrdiff_t>(leftcell_.size());
        const bool in_right =
            off_right >= 0 &&
            off_right < static_cast<ptrdiff_t>(rightcell_.size());
        const size_t grown = bitcells_.size() + kCellChunk;
        bitcells_.resize(grown, 0);
        leftcell_.resize(grown, 0);
        rightcell_.resize(grown, 0);
        if (in_left)
          pctx = leftcell_.data() + off_left;
        else if (in_right)
          pctx = rightcell_.data() + off_right;
      }
      *pctx = static_cast<NumContext>(cur_ncell_++);
      bitcells_[*pctx] = 0;
      leftcell_[*pctx] = rightcell_[*pctx] = 0;
    }

    // A bit is only spent when the declared range leaves the answer open.
    const bool decision =
        (low >= cutoff) ||
        ((high >= cutoff) && CodeBit(bitcells_[*pctx]) != 0);

    const NumContext cell = *pctx;
    pctx = decision ? &rightcell_[cell] : &leftcell_[cell];

    switch (phase) {
      case 1:
        // Sign. A negative value is decoded as the mirrored range.
        negative = !decision;
        if (negative) {
          const int temp = -low - 1;
          low = -high - 1;
          high = temp;
        }
        phase = 2;
        cutoff = 1;
        break;

      case 2:
        // Climb powers of two until the value's bracket is found.
        if (!decision) {
          phase = 3;
          range = static_cast<unsigned>((cutoff + 1) / 2);
          if (range == 1)
            cutoff = 0;
          else
            cutoff -= static_cast<int>(range / 2);
        } else {
          cutoff += cutoff + 1;
        }
        break;

      case 3:
        // Binary search inside the bracket.
        range /= 2;
        if (range != 1) {
          if (!decision)
            cutoff -= static_cast<int>(range / 2);
          else
            cutoff += static_cast<int>(range / 2);
        } else if (!decision) {
          cutoff--;
        }
        break;
    }
  }

  return negative ? (-cutoff - 1) : cutoff;
}

void Jb2Decoder::ResetNumCoder() {
  dist_comment_byte_ = 0;
  dist_comment_length_ = 0;
  dist_record_type_ = 0;
  dist_match_index_ = 0;
  abs_loc_x_ = 0;
  abs_loc_y_ = 0;
  abs_size_x_ = 0;
  abs_size_y_ = 0;
  image_size_dist_ = 0;
  inherited_shape_count_dist_ = 0;
  rel_loc_x_current_ = 0;
  rel_loc_x_last_ = 0;
  rel_loc_y_current_ = 0;
  rel_loc_y_last_ = 0;
  rel_size_x_ = 0;
  rel_size_y_ = 0;
  bitcells_.assign(bitcells_.size(), 0);
  leftcell_.assign(leftcell_.size(), 0);
  rightcell_.assign(rightcell_.size(), 0);
  cur_ncell_ = 1;
}

void Jb2Decoder::InitLibrary(const Jb2Dict& dict) {
  // Shapes inherited from a shared dictionary start out in the library, in
  // order, so that a page can match them without re-sending anything.
  const int n = dict.inherited_count();
  lib2shape_.resize(n);
  shape2lib_.resize(n);
  libinfo_.resize(n);
  for (int i = 0; i < n; ++i) {
    lib2shape_[i] = i;
    shape2lib_[i] = i;
    libinfo_[i].Compute(dict.shape(i).bits);
  }
}

int Jb2Decoder::AddLibrary(int shapeno, const Jb2Shape& shape) {
  const int libno = static_cast<int>(lib2shape_.size());
  lib2shape_.push_back(shapeno);
  if (static_cast<int>(shape2lib_.size()) <= shapeno)
    shape2lib_.resize(shapeno + 1, 0);
  shape2lib_[shapeno] = libno;
  libinfo_.emplace_back();
  libinfo_.back().Compute(shape.bits);
  return libno;
}

int Jb2Decoder::CodeMatchIndex(int* shapeno) {
  if (lib2shape_.empty()) throw FormatError("JB2 match with empty library");
  const int match = CodeNum(0, static_cast<int>(lib2shape_.size()) - 1,
                            &dist_match_index_);
  if (match < 0 || match >= static_cast<int>(lib2shape_.size()))
    throw FormatError("JB2 match index out of range");
  *shapeno = lib2shape_[match];
  return match;
}

void Jb2Decoder::CodeImageSizeCommon(bool is_image) {
  if (is_image) {
    image_columns_ = CodeNum(0, kNumHigh, &image_size_dist_);
    image_rows_ = CodeNum(0, kNumHigh, &image_size_dist_);
    if (!image_columns_ || !image_rows_)
      throw FormatError("JB2 zero image size");
    if (image_columns_ > 1 << 16 || image_rows_ > 1 << 16)
      throw FormatError("JB2 implausible image size");
    image_->width = image_columns_;
    image_->height = image_rows_;
    last_left_ = 1 + image_columns_;
    last_row_bottom_ = image_rows_;
  } else {
    // A dictionary declares a zero size; anything else is malformed.
    const int w = CodeNum(0, kNumHigh, &image_size_dist_);
    const int h = CodeNum(0, kNumHigh, &image_size_dist_);
    if (w || h) throw FormatError("JB2 dictionary with nonzero size");
    last_left_ = 1;
    last_row_bottom_ = 0;
  }
  last_row_left_ = 0;
  last_right_ = 0;
  FillShortList(last_row_bottom_);
  got_start_record_ = true;
}

std::string Jb2Decoder::CodeComment() {
  const int size = CodeNum(0, kNumHigh, &dist_comment_length_);
  std::string out;
  out.reserve(static_cast<size_t>(std::max(0, size)));
  for (int i = 0; i < size; ++i)
    out.push_back(static_cast<char>(CodeNum(0, 255, &dist_comment_byte_)));
  return out;
}

void Jb2Decoder::CodeInheritedShapeCount() {
  const int size = CodeNum(0, kNumHigh, &inherited_shape_count_dist_);
  const int have = target().inherited_count();
  if (size > 0 && have == 0)
    throw FormatError("JB2 page needs a shared dictionary that is missing");
  if (size != have)
    throw FormatError("JB2 shared dictionary has the wrong shape count");
}

void Jb2Decoder::CodeAbsoluteMarkSize(Bitmap& bm, int border) {
  const int xsize = CodeNum(0, kNumHigh, &abs_size_x_);
  const int ysize = CodeNum(0, kNumHigh, &abs_size_y_);
  if (xsize != static_cast<uint16_t>(xsize) ||
      ysize != static_cast<uint16_t>(ysize))
    throw FormatError("JB2 bad mark size");
  bm.Init(ysize, xsize, border);
}

void Jb2Decoder::CodeRelativeMarkSize(Bitmap& bm, int cw, int ch, int border) {
  const int xdiff = CodeNum(kNumLow, kNumHigh, &rel_size_x_);
  const int ydiff = CodeNum(kNumLow, kNumHigh, &rel_size_y_);
  const int xsize = cw + xdiff;
  const int ysize = ch + ydiff;
  if (xsize != static_cast<uint16_t>(xsize) ||
      ysize != static_cast<uint16_t>(ysize))
    throw FormatError("JB2 bad relative mark size");
  bm.Init(ysize, xsize, border);
}

int Jb2Decoder::UpdateShortList(int v) {
  if (++short_list_pos_ == 3) short_list_pos_ = 0;
  int* s = short_list_;
  s[short_list_pos_] = v;
  // Median of the last three baselines: a robust guess for the next one.
  return (s[0] >= s[1]) ? ((s[0] > s[2]) ? ((s[1] >= s[2]) ? s[1] : s[2]) : s[0])
                        : ((s[0] < s[2]) ? ((s[1] >= s[2]) ? s[2] : s[1]) : s[0]);
}

void Jb2Decoder::CodeRelativeLocation(Jb2Blit* blit, int rows, int cols) {
  if (!got_start_record_) throw FormatError("JB2 blit before start of data");

  int bottom = 0, left = 0, top = 0, right = 0;

  // One bit says whether this mark starts a new line of text or continues
  // the current one; the two cases predict from different anchors.
  const int new_row = CodeBit(offset_type_dist_);
  if (new_row) {
    const int x_diff = CodeNum(kNumLow, kNumHigh, &rel_loc_x_last_);
    const int y_diff = CodeNum(kNumLow, kNumHigh, &rel_loc_y_last_);
    left = last_row_left_ + x_diff;
    top = last_row_bottom_ + y_diff;
    right = left + cols - 1;
    bottom = top - rows + 1;
    last_left_ = last_row_left_ = left;
    last_right_ = right;
    last_bottom_ = last_row_bottom_ = bottom;
    FillShortList(bottom);
  } else {
    const int x_diff = CodeNum(kNumLow, kNumHigh, &rel_loc_x_current_);
    const int y_diff = CodeNum(kNumLow, kNumHigh, &rel_loc_y_current_);
    left = last_right_ + x_diff;
    bottom = last_bottom_ + y_diff;
    right = left + cols - 1;
    top = bottom + rows - 1;
    last_left_ = left;
    last_right_ = right;
    last_bottom_ = UpdateShortList(bottom);
  }

  blit->bottom = bottom - 1;
  blit->left = left - 1;
}

void Jb2Decoder::CodeAbsoluteLocation(Jb2Blit* blit, int rows, int cols) {
  if (!got_start_record_) throw FormatError("JB2 blit before start of data");
  (void)cols;
  const int left = CodeNum(1, image_columns_, &abs_loc_x_);
  const int top = CodeNum(1, image_rows_, &abs_loc_y_);
  blit->bottom = top - rows + 1 - 1;
  blit->left = left - 1;
}

void Jb2Decoder::CodeBitmapDirectly(Bitmap& bm) {
  bm.MinBorder(3);
  const int dw = bm.cols();
  int dy = bm.rows() - 1;
  // Rows are decoded bottom-up; up2/up1 are the two rows below in memory
  // order, which are the two already-decoded rows above in the image.
  const uint8_t* up2 = bm.row(dy + 2);
  const uint8_t* up1 = bm.row(dy + 1);
  uint8_t* up0 = bm.row(dy);

  while (dy >= 0) {
    int ctx = DirectContext(up2, up1, up0, 0);
    for (int dx = 0; dx < dw;) {
      const int n = zp_.Decode(bitdist_[ctx]);
      up0[dx++] = static_cast<uint8_t>(n);
      ctx = ShiftDirectContext(ctx, n, up2, up1, dx);
    }
    dy -= 1;
    up2 = up1;
    up1 = up0;
    up0 = bm.row(dy);
  }
}

void Jb2Decoder::CodeBitmapByCrossCoding(Bitmap& bm, Bitmap cbm, int libno) {
  const int cw = cbm.cols();
  const int dw = bm.cols();
  const int dh = bm.rows();
  const LibRect& l = libinfo_[libno];

  // Align the two bitmaps on the centre of their ink, not their corners:
  // that is what makes a refinement cheap.
  const int xd2c = (dw / 2 - dw + 1) - ((l.right - l.left + 1) / 2 - l.right);
  const int yd2c = (dh / 2 - dh + 1) - ((l.top - l.bottom + 1) / 2 - l.top);
  if (xd2c < -15 || xd2c > 15 || yd2c < -15 || yd2c > 15)
    throw FormatError("JB2 refinement offset out of range");

  bm.MinBorder(2);
  int want = 2;
  want = std::max(want, 2 - xd2c);
  want = std::max(want, 2 + dw + xd2c - cw);
  want = std::max(want, 3 + std::abs(yd2c));
  cbm.MinBorder(want);

  int dy = dh - 1;
  int cy = dy + yd2c;

  const uint8_t* up1 = bm.row(dy + 1);
  uint8_t* up0 = bm.row(dy);
  const uint8_t* xup1 = cbm.row(cy + 1) + xd2c;
  const uint8_t* xup0 = cbm.row(cy) + xd2c;
  const uint8_t* xdn1 = cbm.row(cy - 1) + xd2c;

  while (dy >= 0) {
    int ctx = CrossContext(up1, up0, xup1, xup0, xdn1, 0);
    for (int dx = 0; dx < dw;) {
      const int n = zp_.Decode(cbitdist_[ctx]);
      up0[dx++] = static_cast<uint8_t>(n);
      ctx = ShiftCrossContext(ctx, n, up1, xup1, xup0, xdn1, dx);
    }
    up1 = up0;
    up0 = bm.row(--dy);
    xup1 = xup0;
    xup0 = xdn1;
    xdn1 = cbm.row((--cy) - 1) + xd2c;
  }
}

void Jb2Decoder::Run(Jb2Dict* dict, Jb2Image* image) {
  dict_ = dict;
  image_ = image;
  const bool is_image = image != nullptr;

  int rectype = kStartOfData;
  int guard = 0;

  while (true) {
    if (++guard > 20'000'000) throw FormatError("JB2 record storm");

    rectype = CodeNum(kStartOfData, kEndOfData, &dist_record_type_);

    Jb2Shape shape;
    Jb2Blit blit;
    bool have_shape = false;
    bool have_blit = false;
    int match = -1;

    switch (rectype) {
      case kStartOfData:
        CodeImageSizeCommon(is_image);
        // One bit reserved for an eventual lossless refinement pass.
        refinement_ = CodeBit(dist_refinement_flag_) != 0;
        InitLibrary(target());
        break;

      case kNewMark:
      case kNewMarkImageOnly:
      case kNonMarkData: {
        if (!is_image) throw FormatError("JB2 image record in a dictionary");
        shape.parent = (rectype == kNonMarkData) ? -2 : -1;
        const int border = (rectype == kNewMark) ? 4 : 3;
        CodeAbsoluteMarkSize(shape.bits, border);
        CodeBitmapDirectly(shape.bits);
        if (rectype == kNonMarkData)
          CodeAbsoluteLocation(&blit, shape.bits.rows(), shape.bits.cols());
        else
          CodeRelativeLocation(&blit, shape.bits.rows(), shape.bits.cols());
        have_shape = have_blit = true;
        break;
      }

      case kNewMarkLibraryOnly:
        shape.parent = -1;
        CodeAbsoluteMarkSize(shape.bits, 4);
        CodeBitmapDirectly(shape.bits);
        have_shape = true;
        break;

      case kMatchedRefine:
      case kMatchedRefineImageOnly:
      case kMatchedRefineLibraryOnly: {
        match = CodeMatchIndex(&shape.parent);
        const Bitmap& parent = target().shape(shape.parent).bits;
        const LibRect& l = libinfo_[match];
        CodeRelativeMarkSize(shape.bits, l.right - l.left + 1,
                             l.top - l.bottom + 1, 4);
        // In a page, a library-only refinement sends the size but no
        // pixels; in a dictionary the pixels follow. The asymmetry is part
        // of the format as deployed, so both paths are reproduced exactly.
        const bool send_pixels = !is_image || rectype != kMatchedRefineLibraryOnly;
        if (send_pixels) CodeBitmapByCrossCoding(shape.bits, parent, match);
        if (rectype == kMatchedRefine || rectype == kMatchedRefineImageOnly) {
          if (!is_image) throw FormatError("JB2 image record in a dictionary");
          CodeRelativeLocation(&blit, shape.bits.rows(), shape.bits.cols());
          have_blit = true;
        }
        have_shape = true;
        break;
      }

      case kMatchedCopy: {
        if (!is_image) throw FormatError("JB2 image record in a dictionary");
        int shapeno = 0;
        match = CodeMatchIndex(&shapeno);
        blit.shapeno = shapeno;
        const LibRect& l = libinfo_[match];
        // The position is coded relative to the library shape's ink box.
        blit.left += l.left;
        blit.bottom += l.bottom;
        CodeRelativeLocation(&blit, l.top - l.bottom + 1,
                             l.right - l.left + 1);
        blit.left -= l.left;
        blit.bottom -= l.bottom;
        have_blit = true;
        break;
      }

      case kRequiredDictOrReset:
        if (!got_start_record_)
          CodeInheritedShapeCount();
        else
          ResetNumCoder();
        break;

      case kPreservedComment:
        mutable_target().comment = CodeComment();
        break;

      case kEndOfData:
        return;

      default:
        throw FormatError("JB2 unknown record type");
    }

    if (have_shape) {
      const int shapeno = mutable_target().AddShape(std::move(shape));
      const bool to_library =
          rectype == kNewMark || rectype == kNewMarkLibraryOnly ||
          rectype == kMatchedRefine || rectype == kMatchedRefineLibraryOnly;
      if (to_library)
        AddLibrary(shapeno, mutable_target().shape(shapeno));
      if (have_blit) blit.shapeno = shapeno;
    }

    if (have_blit && image_) image_->blits.push_back(blit);
  }
}

}  // namespace

const Jb2Shape& Jb2Dict::shape(int i) const {
  const int inherited = inherited_count();
  if (i < 0) throw FormatError("JB2 negative shape index");
  if (i < inherited) return inherited_->shape(i);
  const size_t local = static_cast<size_t>(i - inherited);
  if (local >= shapes_.size()) throw FormatError("JB2 shape index out of range");
  return shapes_[local];
}

Jb2Shape& Jb2Dict::shape(int i) {
  const int inherited = inherited_count();
  if (i < inherited) throw FormatError("JB2 inherited shapes are read-only");
  const size_t local = static_cast<size_t>(i - inherited);
  if (local >= shapes_.size()) throw FormatError("JB2 shape index out of range");
  return shapes_[local];
}

Bitmap Jb2Image::Compose(int subsample) const {
  if (subsample < 1) subsample = 1;
  const int w = (width + subsample - 1) / subsample;
  const int h = (height + subsample - 1) / subsample;
  Bitmap out(h, w, 0);
  for (const Jb2Blit& b : blits) {
    const Jb2Shape& s = shape(b.shapeno);
    if (s.bits.empty()) continue;
    if (subsample == 1)
      out.Blit(s.bits, b.left, b.bottom);
    else
      out.BlitSubsampled(s.bits, b.left, b.bottom, subsample);
  }
  if (subsample > 1) out.set_grays(subsample * subsample + 1);
  return out;
}

std::shared_ptr<Jb2Dict> DecodeJb2Dict(
    ByteSpan data, std::shared_ptr<const Jb2Dict> inherited) {
  auto dict = std::make_shared<Jb2Dict>();
  dict->set_inherited(std::move(inherited));
  Jb2Decoder dec(data);
  dec.Run(dict.get(), nullptr);
  return dict;
}

std::shared_ptr<Jb2Image> DecodeJb2Image(
    ByteSpan data, std::shared_ptr<const Jb2Dict> inherited) {
  auto image = std::make_shared<Jb2Image>();
  image->set_inherited(std::move(inherited));
  Jb2Decoder dec(data);
  dec.Run(nullptr, image.get());
  return image;
}

}  // namespace vellum::djvu
