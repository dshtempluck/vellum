#include "djvu/iw44.h"

#include <algorithm>
#include <cstring>

namespace vellum::djvu {
namespace {

#include "djvu/iw44tables.inc"

// Fixed-point scale applied to coefficients before reconstruction.
constexpr int kIwShift = 6;
constexpr int kIwRound = 1 << (kIwShift - 1);

// The ten frequency bands, as {first bucket, bucket count}. Band 0 holds
// the lowest-frequency coefficients of every block.
struct BandSpec {
  int start;
  int size;
};
constexpr BandSpec kBands[10] = {
    {0, 1},                      // all low-resolution information
    {1, 1},   {2, 1},  {3, 1},   //
    {4, 4},   {8, 4},  {12, 4},  //
    {16, 16}, {32, 16}, {48, 16},
};
constexpr int kNumBands = 10;

// Per-coefficient decoding state.
enum CoeffState {
  kZero = 1,    // cannot become active at this bit-plane
  kActive = 2,  // already active
  kNew = 4,     // becoming active now
  kUnk = 8,     // may become active
};

// --- inverse wavelet lifting -------------------------------------------
//
// IW44 uses a biorthogonal lifting filter. Reconstruction runs the two
// lifting steps in reverse at each scale: first vertically, then
// horizontally. The interior uses a 4-tap predictor (9*near - far) / 32;
// rows and columns near an edge fall back to shorter filters.

void FilterVertical(short* p, int w, int h, int rowsize, int scale) {
  int y = 0;
  const int s = scale * rowsize;
  const int s3 = s + s + s;
  h = ((h - 1) / scale) + 1;

  while (y - 3 < h) {
    // Step 1: undo the prediction on odd rows.
    {
      short* q = p;
      short* e = q + w;
      if (y >= 3 && y + 3 < h) {
        while (q < e) {
          const int a = q[-s] + q[s];
          const int b = q[-s3] + q[s3];
          *q -= static_cast<short>(((a << 3) + a - b + 16) >> 5);
          q += scale;
        }
      } else if (y < h) {
        short* q1 = (y + 1 < h) ? q + s : nullptr;
        short* q3 = (y + 3 < h) ? q + s3 : nullptr;
        if (y >= 3) {
          while (q < e) {
            const int a = q[-s] + (q1 ? *q1 : 0);
            const int b = q[-s3] + (q3 ? *q3 : 0);
            *q -= static_cast<short>(((a << 3) + a - b + 16) >> 5);
            q += scale;
            if (q1) q1 += scale;
            if (q3) q3 += scale;
          }
        } else if (y >= 1) {
          while (q < e) {
            const int a = q[-s] + (q1 ? *q1 : 0);
            const int b = (q3 ? *q3 : 0);
            *q -= static_cast<short>(((a << 3) + a - b + 16) >> 5);
            q += scale;
            if (q1) q1 += scale;
            if (q3) q3 += scale;
          }
        } else {
          while (q < e) {
            const int a = (q1 ? *q1 : 0);
            const int b = (q3 ? *q3 : 0);
            *q -= static_cast<short>(((a << 3) + a - b + 16) >> 5);
            q += scale;
            if (q1) q1 += scale;
            if (q3) q3 += scale;
          }
        }
      }
    }

    // Step 2: undo the update on even rows, three rows behind.
    {
      short* q = p - s3;
      short* e = q + w;
      if (y >= 6 && y < h) {
        while (q < e) {
          const int a = q[-s] + q[s];
          const int b = q[-s3] + q[s3];
          *q += static_cast<short>(((a << 3) + a - b + 8) >> 4);
          q += scale;
        }
      } else if (y >= 3) {
        short* q1 = (y - 2 < h) ? q + s : q - s;
        while (q < e) {
          const int a = q[-s] + *q1;
          *q += static_cast<short>((a + 1) >> 1);
          q += scale;
          q1 += scale;
        }
      }
    }

    y += 2;
    p += s + s;
  }
}

void FilterHorizontal(short* p, int w, int h, int rowsize, int scale) {
  int y = 0;
  const int s = scale;
  const int s3 = s + s + s;
  rowsize *= scale;

  while (y < h) {
    short* q = p;
    short* e = p + w;
    // Both lifting steps run in one pass over the row, keeping a sliding
    // window of four inputs (a*) and four outputs (b*).
    int a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    int b0 = 0, b1 = 0, b2 = 0, b3 = 0;

    if (q < e) {  // x = 0
      if (q + s < e) a2 = q[s];
      if (q + s3 < e) a3 = q[s3];
      b2 = b3 = q[0] -
                ((((a1 + a2) << 3) + (a1 + a2) - a0 - a3 + 16) >> 5);
      q[0] = static_cast<short>(b3);
      q += s + s;
    }
    if (q < e) {  // x = 2
      a0 = a1;
      a1 = a2;
      a2 = a3;
      if (q + s3 < e) a3 = q[s3];
      b3 = q[0] - ((((a1 + a2) << 3) + (a1 + a2) - a0 - a3 + 16) >> 5);
      q[0] = static_cast<short>(b3);
      q += s + s;
    }
    if (q < e) {  // x = 4
      b1 = b2;
      b2 = b3;
      a0 = a1;
      a1 = a2;
      a2 = a3;
      if (q + s3 < e) a3 = q[s3];
      b3 = q[0] - ((((a1 + a2) << 3) + (a1 + a2) - a0 - a3 + 16) >> 5);
      q[0] = static_cast<short>(b3);
      q[-s3] = static_cast<short>(q[-s3] + ((b1 + b2 + 1) >> 1));
      q += s + s;
    }
    while (q + s3 < e) {  // interior
      a0 = a1;
      a1 = a2;
      a2 = a3;
      a3 = q[s3];
      b0 = b1;
      b1 = b2;
      b2 = b3;
      b3 = q[0] - ((((a1 + a2) << 3) + (a1 + a2) - a0 - a3 + 16) >> 5);
      q[0] = static_cast<short>(b3);
      q[-s3] = static_cast<short>(
          q[-s3] + ((((b1 + b2) << 3) + (b1 + b2) - b0 - b3 + 8) >> 4));
      q += s + s;
    }
    while (q < e) {  // last three columns
      a0 = a1;
      a1 = a2;
      a2 = a3;
      a3 = 0;
      b0 = b1;
      b1 = b2;
      b2 = b3;
      b3 = q[0] - ((((a1 + a2) << 3) + (a1 + a2) - a0 - a3 + 16) >> 5);
      q[0] = static_cast<short>(b3);
      q[-s3] = static_cast<short>(
          q[-s3] + ((((b1 + b2) << 3) + (b1 + b2) - b0 - b3 + 8) >> 4));
      q += s + s;
    }
    while (q - s3 < e) {  // flush the update step past the end
      b0 = b1;
      b1 = b2;
      b2 = b3;
      if (q - s3 >= p)
        q[-s3] = static_cast<short>(q[-s3] + ((b1 + b2 + 1) >> 1));
      q += s + s;
    }

    y += scale;
    p += rowsize;
  }
}

void InverseWavelet(short* p, int w, int h, int rowsize, int begin, int end) {
  for (int scale = begin >> 1; scale >= end; scale >>= 1) {
    FilterVertical(p, w, h, rowsize, scale);
    FilterHorizontal(p, w, h, rowsize, scale);
  }
}

}  // namespace

// One colour plane: a grid of 32x32 coefficient blocks.
struct Iw44Decoder::Map {
  // A block holds 1024 coefficients in bucket order. Buckets are tracked
  // as present or absent, because the bit-plane coder distinguishes
  // "known zero" from "not yet visited".
  struct Block {
    std::vector<short> coeff;
    uint64_t present = 0;

    const short* bucket(int n) const {
      if (!((present >> n) & 1)) return nullptr;
      return coeff.data() + static_cast<size_t>(n) * 16;
    }
    short* bucket(int n) {
      if (!((present >> n) & 1)) return nullptr;
      return coeff.data() + static_cast<size_t>(n) * 16;
    }
    short* AllocBucket(int n) {
      if (coeff.empty()) coeff.assign(1024, 0);
      present |= (1ull << n);
      return coeff.data() + static_cast<size_t>(n) * 16;
    }
  };

  int iw = 0, ih = 0, bw = 0, bh = 0, nb = 0;
  std::vector<Block> blocks;

  Map(int w, int h) : iw(w), ih(h) {
    bw = (w + 31) & ~31;
    bh = (h + 31) & ~31;
    if (bw <= 0 || bh <= 0) throw FormatError("IW44 zero image size");
    // Guard against absurd dimensions before allocating.
    if (static_cast<int64_t>(bw) * bh > (int64_t)1 << 28)
      throw FormatError("IW44 image too large");
    nb = (bw / 32) * (bh / 32);
    blocks.resize(nb);
  }

  // Reconstructs this plane into `out` as signed 8-bit samples, writing one
  // value every `pixsep` bytes and `rowsize` bytes per row. `half` renders
  // at half resolution and doubles pixels, which is how chroma planes are
  // stored when the encoder subsampled them.
  void ToPlane(int8_t* out, int rowsize, int pixsep, bool half) const {
    // Padding keeps the lifting filters' pointer arithmetic in bounds: the
    // row cursor can run up to 32 rows past the image.
    std::vector<short> buf(static_cast<size_t>(bw) * (bh + 48), 0);

    // Lay the blocks out into one coefficient matrix, undoing the zigzag.
    short* p = buf.data();
    const Block* block = blocks.data();
    for (int i = 0; i < bh; i += 32) {
      for (int j = 0; j < bw; j += 32) {
        short lift[1024];
        std::memset(lift, 0, sizeof(lift));
        int n = 0;
        for (int n1 = 0; n1 < 64; ++n1) {
          const short* d = block->bucket(n1);
          if (!d) {
            n += 16;
            continue;
          }
          for (int n2 = 0; n2 < 16; ++n2, ++n) lift[kZigzagLoc[n]] = d[n2];
        }
        ++block;
        short* pp = p + j;
        const short* pl = lift;
        for (int ii = 0; ii < 32; ++ii, pp += bw, pl += 32)
          std::memcpy(pp, pl, 32 * sizeof(short));
      }
      p += 32 * bw;
    }

    if (half) {
      InverseWavelet(buf.data(), iw, ih, bw, 32, 2);
      // Replicate each decoded sample into a 2x2 block.
      short* q = buf.data();
      for (int i = 0; i < bh; i += 2, q += bw)
        for (int jj = 0; jj < bw; jj += 2, q += 2)
          q[bw] = q[bw + 1] = q[1] = q[0];
    } else {
      InverseWavelet(buf.data(), iw, ih, bw, 32, 1);
    }

    // DjVu rasters run bottom-up: decoded scanline 0 is the bottom of the
    // image, the same convention the JB2 mask uses. The destination is a
    // top-down Pixmap, so the rows are emitted in reverse. Getting this
    // wrong leaves the background layer mirrored against the mask.
    const short* src = buf.data();
    int8_t* row = out + static_cast<ptrdiff_t>(ih - 1) * rowsize;
    for (int i = 0; i < ih; ++i) {
      int8_t* pix = row;
      for (int j = 0; j < iw; ++j, pix += pixsep) {
        int x = (src[j] + kIwRound) >> kIwShift;
        x = std::clamp(x, -128, 127);
        *pix = static_cast<int8_t>(x);
      }
      row -= rowsize;
      src += bw;
    }
  }
};

// Bit-plane decoder for one plane. Successive slices lower the
// quantization thresholds until every band is exhausted.
struct Iw44Decoder::Codec {
  Map& map;
  int curband = 0;
  int curbit = 1;
  int quant_hi[kNumBands] = {};
  int quant_lo[16] = {};
  char coeffstate[256] = {};
  char bucketstate[16] = {};
  BitContext ctx_start[32] = {};
  BitContext ctx_bucket[kNumBands][8] = {};
  BitContext ctx_mant = 0;
  BitContext ctx_root = 0;

  explicit Codec(Map& m) : map(m) {
    // Band 0 carries sixteen coefficients with four distinct step sizes;
    // bands 1..9 have one step each.
    int i = 0;
    const int* q = kIwQuant;
    for (; i < 4;) quant_lo[i++] = *q++;
    for (int j = 0; j < 4; ++j) quant_lo[i++] = *q;
    ++q;
    for (int j = 0; j < 4; ++j) quant_lo[i++] = *q;
    ++q;
    for (int j = 0; j < 4; ++j) quant_lo[i++] = *q;
    ++q;
    quant_hi[0] = 0;
    for (int j = 1; j < kNumBands; ++j) quant_hi[j] = *q++;
  }

  // True when this band cannot contribute at the current bit-plane. For
  // band 0 it also marks which of its sixteen coefficients are still live.
  bool IsNullSlice(int band) {
    if (band == 0) {
      bool is_null = true;
      for (int i = 0; i < 16; ++i) {
        const int threshold = quant_lo[i];
        coeffstate[i] = kZero;
        if (threshold > 0 && threshold < 0x8000) {
          coeffstate[i] = kUnk;
          is_null = false;
        }
      }
      return is_null;
    }
    const int threshold = quant_hi[band];
    return !(threshold > 0 && threshold < 0x8000);
  }

  int Prepare(int fbucket, int nbucket, Map::Block& blk) {
    int bbstate = 0;
    char* cstate = coeffstate;
    if (fbucket) {
      for (int buckno = 0; buckno < nbucket; ++buckno, cstate += 16) {
        int bstatetmp = 0;
        const short* pcoeff = blk.bucket(fbucket + buckno);
        if (!pcoeff) {
          bstatetmp = kUnk;  // coefficient states filled in on demand
        } else {
          for (int i = 0; i < 16; ++i) {
            const int cs = pcoeff[i] ? kActive : kUnk;
            cstate[i] = static_cast<char>(cs);
            bstatetmp |= cs;
          }
        }
        bucketstate[buckno] = static_cast<char>(bstatetmp);
        bbstate |= bstatetmp;
      }
    } else {
      const short* pcoeff = blk.bucket(0);
      if (!pcoeff) {
        bbstate = kUnk;
      } else {
        for (int i = 0; i < 16; ++i) {
          int cs = coeffstate[i];
          if (cs != kZero) cs = pcoeff[i] ? kActive : kUnk;
          cstate[i] = static_cast<char>(cs);
          bbstate |= cs;
        }
      }
      bucketstate[0] = static_cast<char>(bbstate);
    }
    return bbstate;
  }

  void DecodeBuckets(ZpDecoder& zp, int band, Map::Block& blk, int fbucket,
                     int nbucket) {
    int bbstate = Prepare(fbucket, nbucket, blk);

    // A single bit can declare a whole band of a block empty.
    if (nbucket < 16 || (bbstate & kActive)) {
      bbstate |= kNew;
    } else if (bbstate & kUnk) {
      if (zp.Decode(ctx_root)) bbstate |= kNew;
    }

    if (bbstate & kNew) {
      for (int buckno = 0; buckno < nbucket; ++buckno) {
        if (!(bucketstate[buckno] & kUnk)) continue;
        // Context: how much energy the parent band already has here, plus
        // whether this block is active at all.
        int ctx = 0;
        if (band > 0) {
          int k = (fbucket + buckno) << 2;
          const short* b = blk.bucket(k >> 4);
          if (b) {
            k &= 0xf;
            if (b[k]) ctx += 1;
            if (b[k + 1]) ctx += 1;
            if (b[k + 2]) ctx += 1;
            if (ctx < 3 && b[k + 3]) ctx += 1;
          }
        }
        if (bbstate & kActive) ctx |= 4;
        if (zp.Decode(ctx_bucket[band][ctx])) bucketstate[buckno] |= kNew;
      }
    }

    // Newly active coefficients: one bit each, then a sign.
    if (bbstate & kNew) {
      int thres = quant_hi[band];
      char* cstate = coeffstate;
      for (int buckno = 0; buckno < nbucket; ++buckno, cstate += 16) {
        if (!(bucketstate[buckno] & kNew)) continue;
        short* pcoeff = blk.bucket(fbucket + buckno);
        if (!pcoeff) {
          pcoeff = blk.AllocBucket(fbucket + buckno);
          if (fbucket == 0) {
            for (int i = 0; i < 16; ++i)
              if (cstate[i] != kZero) cstate[i] = kUnk;
          } else {
            for (int i = 0; i < 16; ++i) cstate[i] = kUnk;
          }
        }
        // "gotcha" counts how many coefficients here are still unknown; it
        // conditions the context so runs of zeros get cheaper.
        int gotcha = 0;
        constexpr int kMaxGotcha = 7;
        for (int i = 0; i < 16; ++i)
          if (cstate[i] & kUnk) gotcha += 1;

        for (int i = 0; i < 16; ++i) {
          if (!(cstate[i] & kUnk)) continue;
          if (band == 0) thres = quant_lo[i];
          int ctx = (gotcha >= kMaxGotcha) ? kMaxGotcha : gotcha;
          if (bucketstate[buckno] & kActive) ctx |= 8;
          if (zp.Decode(ctx_start[ctx])) {
            cstate[i] |= kNew;
            const int halfthres = thres >> 1;
            const int coeff = thres + halfthres - (halfthres >> 2);
            pcoeff[i] = static_cast<short>(zp.DecodeIw() ? -coeff : coeff);
          }
          if (cstate[i] & kNew)
            gotcha = 0;
          else if (gotcha > 0)
            gotcha -= 1;
        }
      }
    }

    // Already-active coefficients: one refinement bit each.
    if (bbstate & kActive) {
      int thres = quant_hi[band];
      char* cstate = coeffstate;
      for (int buckno = 0; buckno < nbucket; ++buckno, cstate += 16) {
        if (!(bucketstate[buckno] & kActive)) continue;
        short* pcoeff = blk.bucket(fbucket + buckno);
        if (!pcoeff) continue;
        for (int i = 0; i < 16; ++i) {
          if (!(cstate[i] & kActive)) continue;
          int coeff = pcoeff[i];
          if (coeff < 0) coeff = -coeff;
          if (band == 0) thres = quant_lo[i];
          if (coeff <= 3 * thres) {
            // Second mantissa bit is worth modelling; later ones are not.
            coeff = coeff + (thres >> 2);
            if (zp.Decode(ctx_mant))
              coeff = coeff + (thres >> 1);
            else
              coeff = coeff - thres + (thres >> 1);
          } else {
            if (zp.DecodeIw())
              coeff = coeff + (thres >> 1);
            else
              coeff = coeff - thres + (thres >> 1);
          }
          pcoeff[i] = static_cast<short>(pcoeff[i] > 0 ? coeff : -coeff);
        }
      }
    }
  }

  // Decodes one slice. Returns false once every threshold has collapsed.
  bool CodeSlice(ZpDecoder& zp) {
    if (curbit < 0) return false;
    if (!IsNullSlice(curband)) {
      for (int blockno = 0; blockno < map.nb; ++blockno)
        DecodeBuckets(zp, curband, map.blocks[blockno], kBands[curband].start,
                      kBands[curband].size);
    }
    // Halve this band's threshold and move to the next band, then the next
    // bit-plane.
    quant_hi[curband] >>= 1;
    if (curband == 0)
      for (int i = 0; i < 16; ++i) quant_lo[i] >>= 1;
    if (++curband >= kNumBands) {
      curband = 0;
      curbit += 1;
      if (quant_hi[kNumBands - 1] == 0) {
        curbit = -1;
        return false;
      }
    }
    return true;
  }
};

Iw44Decoder::Iw44Decoder() = default;
Iw44Decoder::~Iw44Decoder() = default;

void Iw44Decoder::AddChunk(ByteSpan chunk) {
  ByteReader r(chunk);

  // Primary header: which chunk this is, and how many slices it carries.
  const int serial = r.u8();
  const int slices = r.u8();
  if (serial != serial_)
    throw FormatError("IW44 chunk out of sequence");
  const int nslices = slice_ + slices;

  if (serial_ == 0) {
    const int major = r.u8();
    const int minor = r.u8();
    if ((major & 0x7f) != 1) throw FormatError("IW44 unsupported version");
    const int xhi = r.u8(), xlo = r.u8(), yhi = r.u8(), ylo = r.u8();
    int crcbdelay = 0;
    if ((major & 0x7f) == 1 && minor >= 2) crcbdelay = r.u8();

    width_ = (xhi << 8) | xlo;
    height_ = (yhi << 8) | ylo;
    crcb_delay_ = 0;
    crcb_half_ = false;
    if (minor >= 2) {
      crcb_delay_ = crcbdelay & 0x7f;
      crcb_half_ = (crcbdelay & 0x80) == 0;
    }
    // The high bit of `major` marks a grayscale image: no chroma planes.
    if (major & 0x80) crcb_delay_ = -1;

    y_ = std::make_unique<Map>(width_, height_);
    ycodec_ = std::make_unique<Codec>(*y_);
    if (crcb_delay_ >= 0) {
      cb_ = std::make_unique<Map>(width_, height_);
      cr_ = std::make_unique<Map>(width_, height_);
      cbcodec_ = std::make_unique<Codec>(*cb_);
      crcodec_ = std::make_unique<Codec>(*cr_);
    }
  }

  if (!ycodec_) throw FormatError("IW44 data before header");

  ZpDecoder zp(chunk.subspan(r.pos(), chunk.size() - r.pos()));
  bool more = true;
  while (more && slice_ < nslices) {
    more = ycodec_->CodeSlice(zp);
    // Chroma can be delayed, spending the first slices on luminance only.
    if (crcodec_ && cbcodec_ && crcb_delay_ <= slice_) {
      more = cbcodec_->CodeSlice(zp) || more;
      more = crcodec_->CodeSlice(zp) || more;
    }
    ++slice_;
  }
  serial_ += 1;
}

Pixmap Iw44Decoder::ToPixmap() const {
  if (!y_) return Pixmap();

  Pixmap out(width_, height_);
  // Rows are padded to a 4-byte boundary, so the plane writer must step by
  // the real stride; using width*3 would walk each row a little further
  // than the last and shear the image.
  const int rowsize = static_cast<int>(out.stride());

  // The planes are reconstructed straight into the RGB buffer's channels,
  // then converted in place.
  y_->ToPlane(reinterpret_cast<int8_t*>(out.row(0)), rowsize, 3, false);
  if (cb_ && cr_ && crcb_delay_ >= 0) {
    cb_->ToPlane(reinterpret_cast<int8_t*>(out.row(0)) + 1, rowsize, 3,
                 crcb_half_);
    cr_->ToPlane(reinterpret_cast<int8_t*>(out.row(0)) + 2, rowsize, 3,
                 crcb_half_);
    for (int i = 0; i < height_; ++i) {
      uint8_t* q = out.row(i);
      for (int j = 0; j < width_; ++j, q += 3) {
        const int8_t y = static_cast<int8_t>(q[0]);
        const int8_t b = static_cast<int8_t>(q[1]);
        const int8_t rr = static_cast<int8_t>(q[2]);
        // DjVu's integer YCbCr variant.
        const int t1 = b >> 2;
        const int t2 = rr + (rr >> 1);
        const int t3 = y + 128 - t1;
        q[0] = static_cast<uint8_t>(std::clamp(t3 + (b << 1), 0, 255));
        q[1] = static_cast<uint8_t>(std::clamp(t3 - (t2 >> 1), 0, 255));
        q[2] = static_cast<uint8_t>(std::clamp(y + 128 + t2, 0, 255));
      }
    }
  } else {
    for (int i = 0; i < height_; ++i) {
      uint8_t* q = out.row(i);
      for (int j = 0; j < width_; ++j, q += 3) {
        const int v = 127 - static_cast<int8_t>(q[0]);
        const uint8_t g = static_cast<uint8_t>(std::clamp(v, 0, 255));
        q[0] = q[1] = q[2] = g;
      }
    }
  }
  return out;
}

}  // namespace vellum::djvu
