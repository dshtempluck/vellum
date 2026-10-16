# Reverse engineering notes

What was examined, how, and what it implies for the viewer in this
repository. Two targets: **Adobe Acrobat**, as the reference for what a PDF
viewer should do, and **DjVu**, as a format with no Acrobat support that the
viewer had to gain from scratch.

Everything below is static analysis of shipped binaries and published
format data. No Acrobat code was copied or translated; what was taken from
Acrobat is its *architecture and behaviour*, which is what the viewer
reimplements independently.

---

## 1. Adobe Acrobat

### Subject

| | |
|---|---|
| Product | Adobe Acrobat (64-bit) |
| Version | 26.002.21931 (`Acrobat.exe` file version 26.2.21931.0) |
| Install root | `C:\Program Files\Adobe\Acrobat DC\Acrobat` |
| Architecture | PE32+ / x86-64 |

### Method and its limits

No native disassembler (Ghidra, IDA, Hopper) was registered with the
analysis tooling on this host, so there is **no decompilation in these
notes**. The evidence is:

* export and import tables (`dumpbin /exports`, `/dependents`)
* ASCII and UTF-16 string extraction from `Acrobat.dll` (182,740 unique
  strings)
* PE resource inventory of `AcrobatRes.dll` (1,186 resource entries)
* the on-disk module and process layout

That is enough to establish the module graph, the API surface of each
layer, and the viewer's feature vocabulary. It is **not** enough to
establish algorithms, data structures, or control flow. Claims below are
labelled accordingly.

### 1.1 Module graph (observed)

`Acrobat.exe` is a 6.8 MB shell. It imports no Adobe module at all — only
Win32 — and the real application lives in `Acrobat.dll` (90.9 MB), entered
through the exported `AcroWinMain` / `AcroWinMainSandbox`.

```
Acrobat.exe ──► Acrobat.dll :: AcroWinMain
                  │
                  ├── AdobePDFL.dll     PDF object model + page rendering
                  ├── AGM.dll           Adobe Graphics Manager (rasterizer)
                  ├── CoolType.dll      font scaling/hinting  ──► DWrite.dll
                  ├── ACE.dll           Adobe Color Engine (ICC)
                  ├── ARE.dll           Adobe Raster Engine
                  ├── AIDE.dll          image decoding
                  ├── JP2KLib.dll       JPEG 2000  (for /JPXDecode)
                  ├── BIB.dll, BIBUtils.dll    portability substrate
                  ├── AXE8SharedExpat.dll      XML  (XFA, XMP)
                  ├── AdobeXMP.dll, FileInfo.dll   metadata
                  ├── PDFPort.dll       print/port abstraction
                  ├── thorvg-1.dll      vector graphics (recent addition)
                  ├── sqlite.dll        local store
                  └── plug_ins/*.api    31 plug-ins, loaded via the HFT
```

Out of process:

* `AcroCEF/libcef.dll` — 176 MB of Chromium Embedded Framework. The modern
  panels (Home, Tools, sign-in) are web surfaces, not native ones.
* `AcroBroker.exe` — the protected-mode sandbox broker.
* `AcroTextExtractor.exe` — text extraction isolated into its own process.

### 1.2 The four-layer object model (observed)

Export names in `AdobePDFL.dll` (1,523 exports) cluster cleanly, and the
clusters *are* Acrobat's documented layer model:

| Layer | Exports | Meaning | Representative entry points |
|---|---:|---|---|
| **AS** | 265 | Acrobat Support — platform abstraction | `ASFileSysOpenFile64`, `ASAtomFromString`, `ASFileStmRdOpen` |
| **Cos** | 109 | the PDF object graph itself | `CosDocOpenWithParams`, `CosArrayGet`, `CosStreamOpenStm`, `CosObjEnum` |
| **PD** | 1,084 | document semantics | `PDDocAcquirePage`, `PDPageDrawContentsToMemory`, `PDBookmarkGetByTitleASText` |
| **AV** | — | the viewer: windows, tools, menus | not exported; reached by plug-ins through the HFT |

`HFTGetVersion` and `RestorePlugInFrame` are exported, confirming the
Host Function Table: plug-ins do not link against Acrobat, they request
tables of function pointers at load time.

`ASAtom` (interned strings), `ASFixed` (16.16 fixed point) and the
`ASFileSys` indirection appear throughout, which is why the whole stack can
be retargeted to a non-filesystem source.

### 1.3 The rendering contract (observed)

The `PDPage` family is the most directly useful evidence for building a
viewer, because the shape of the draw calls tells you what a viewer needs:

```
PDPageDrawContentsToWindow        draw to an HDC
PDPageDrawContentsToWindowEx
PDPageDrawContentsToMemory        draw to a caller's buffer
PDPageDrawContentsPlaced          draw under an arbitrary matrix
PDPageDrawContentsPlacedEx
PDPageDrawContentsPlacedToWindow
PDPageDrawContentsWithParams      struct-based, extensible
PDPageDrawContentsPlacedWithParams
PDPageDrawContentsForPrinting     separate path for print
PDPageAcquireThumb                thumbnails are a first-class concept
```

Four observations shaped this project:

1. **Render-to-memory is a peer of render-to-window.** A viewer that only
   draws to a window cannot cache, thread, or produce thumbnails.
2. **Placement is a matrix, not a rectangle.** Zoom and rotation are one
   concept, not two.
3. **Printing has its own path** — the screen path is allowed to approximate.
4. **Thumbnails are part of the document model**, not a UI afterthought.

Other clusters worth noting:

* `PDDocCreateWordFinder` / `PDWordFinder*` / `PDTextSelect*` (53 exports):
  text extraction produces a *word list*, and selection is expressed as
  **quads** (`PDTextSelectEnumQuads`) — quadrilaterals, not rectangles, so
  that rotated and skewed text highlights correctly.
* `PDOC*` — 83 exports for optional content (layers), including
  `PDOCRegisterFindOutZoomProc`: layer visibility can depend on zoom.
* `PDDocAuthorize`, `PDCryptAuthorizeFilterAccess`: encryption is a
  document-open concern, not a render concern.

### 1.4 Viewer behaviour (observed, from strings)

`Acrobat.dll` yields 10,565 distinct `AV*` identifiers. The ones that
define user-visible behaviour:

**Zoom modes** — the literal `AVZoomType` members are present:
`AVZoomFitPage`, `AVZoomFitWidth`, `AVZoomFitVisibleWidth`, `AVZoomNoVary`
(= fixed percentage).

**Page layouts** — all six PDF layout names appear exactly once each, i.e.
as a name table: `SinglePage`, `OneColumn`, `TwoColumnLeft`,
`TwoColumnRight`, `TwoPageLeft`, `TwoPageRight`, plus `Continuous`.

**Fit commands** — `FitPage`, `FitWidth`, `FitHeight`, `FitVisible`,
`FitWindow`, and `…Toggle` variants of each.

**Tools** — `HandTool`, `HandSelects`, `HandHoverDelay`.

**Navigation panels** — `Bookmarks`, `Thumbnails`, `Comments`,
`Attachments`, `ArticleThreads`, `BookmarkListModel`, `BookmarkNavFrame`.

**Find** — `FindDialog`, `FindAgain`, `FindAll`, `FindAndReplace`,
`BookmarksSearch`, plus a whole separate indexing engine exported from
`Acrobat.dll` as `ix*` (`ixCreateIndexEx`, `ixStartRetrievalSession`,
`ixStem8BitWord` — stemming, so full-text search is linguistic, not
substring).

**View architecture** — `AVScrolledPageView`,
`AVScrolledPageViewWithDistributor`, `AVAVDocumentViewDocDistributor`,
`AVWindowDocDistributor`. The repeated *Distributor* suffix, together with
the pervasive `…DidChange` / `…BROADCAST` / `…Proc` / `…Shim` naming, is
consistent with a per-document observer that fans notifications out to
every view. (*Inference from naming, not from code.*)

**Codecs** — each PDF stream filter name occurs exactly once, i.e. a
dispatch table: `FlateDecode`, `LZWDecode`, `DCTDecode`, `JPXDecode`,
`CCITTFaxDecode`, `JBIG2Decode`, `RunLengthDecode`, `ASCIIHexDecode`,
`ASCII85Decode`, `Crypt`.

**DjVu** — zero occurrences, case-insensitive, anywhere in `Acrobat.dll`.
Acrobat does not read DjVu, which is what makes the second half of this
project additive rather than redundant.

### 1.5 Text layout engine (observed)

`AcroForm.api` (20.6 MB, 1,632 exports) is not only forms. It exports 330+
`hb_*` symbols — a complete **HarfBuzz** — alongside Adobe's own
`WRServices` / `Optyca` line-breaking engine (`OptycaImpl::SetBidiAlgorithm`,
`WRSearchFont`, `IWRFontAccess`). Complex-script shaping lives in the forms
plug-in, which is a strong hint that it arrived with XFA forms and was then
reused.

---

## 2. DjVu

### Subject

No DjVu reader was installed on this machine, and the usual download
mirrors (SourceForge) refused automated requests. The analysis target
became the **DjVuLibre reference implementation** (cloned from
`github.com/barak/djvulibre`) read as *format documentation*, corroborated
against real files: the repository ships the DjVu v2 and v3 specifications
and two books **as DjVu documents**, which is both the specification and a
test corpus.

### 2.1 Container structure (observed)

A DjVu file is IFF-85 with a four-byte `AT&T` preamble. Chunk census across
the six sample documents:

| Chunk | Meaning |
|---|---|
| `FORM:DJVM` | multi-page document |
| `FORM:DJVU` | one page |
| `FORM:DJVI` | shared component (a shape dictionary) |
| `DIRM` | document directory — BZZ-compressed |
| `NAVM` | outline / bookmarks — BZZ-compressed |
| `INFO` | page width, height, dpi, orientation |
| `INCL` | names a `FORM:DJVI` this page depends on |
| `Djbz` | JB2 shape dictionary shared across pages |
| `Sjbz` | JB2-coded bitonal mask (the text layer) |
| `Smmr` | MMR/G4-coded mask (alternative) |
| `BG44`, `FG44` | IW44 wavelet background / foreground |
| `BGjp`, `FGjp` | JPEG background / foreground |
| `BG2k`, `FG2k` | JPEG 2000 background / foreground |
| `FGbz` | JB2-coded colour palette for the foreground |
| `TXTa`, `TXTz` | hidden text layer (plain / BZZ) |
| `ANTa`, `ANTz` | annotations |
| `TH44` | embedded thumbnails |

The three-layer model is the point of the format: a 300 dpi bitonal mask
carries text at full resolution, while the photographic background rides
along at a third of that. Composition is `mask ? foreground : background`.

### 2.2 The codec stack (implemented from format data)

Four codecs, each built on the one below:

```
BZZ (DIRM, NAVM, TXTz, ANTz)   JB2 (Sjbz, Djbz)   IW44 (BG44, FG44)
            └──────────────────────┴──────────────────┘
                              ZP coder
```

**ZP coder** — an adaptive binary arithmetic coder: a 16-bit interval
register, a code register fed from a byte lookahead, and a 256-state
probability machine where each state carries both a probability increment
and its adaptation targets. DjVu requires the *unpatched* table; the
reference implementation patches it for non-DjVu use and explicitly notes
that doing so breaks DjVu compatibility.

**BZZ** — Burrows–Wheeler transform, then a move-to-front stage whose ranks
are entropy-coded with the ZP coder. The MTF table is re-sorted by running
frequency rather than pure recency. A sentinel row marks the BWT rotation.

**JB2** — symbol matching. A page is a list of *blits*: small bitmaps
(in practice individual glyphs) stamped at positions. A repeated glyph
costs a library index and a delta-coded position; a near-repeat is coded as
a *refinement* of an earlier shape, using a context window that spans both
the new bitmap and the one being refined. Pixels use a 10-bit neighbourhood
context (1,024 adaptive contexts) for fresh shapes and an 11-bit one (2,048)
for refinements. Positions are predicted from the previous mark on the line,
or from a three-entry median of recent baselines when a new line starts.

**IW44** — a biorthogonal wavelet. Each 32×32 block of coefficients is
grouped into 10 frequency bands of 16-coefficient buckets and sent
bit-plane by bit-plane, so a truncated chunk sequence still yields a
complete, lower-quality image. Colour is a YCbCr variant with optionally
half-resolution, delayed chroma.

### 2.3 Format constants

Three tables are *data definitions of the bitstream* — without the exact
values no conforming file can be decoded:

* the 256-state ZP adaptation table (states 0–250 defined, 251–255 zero
  padding),
* IW44's 16 per-subband quantization steps,
* IW44's 1,024-entry zigzag map from bucket order to block position.

These are extracted mechanically by `tools/gen_zptable.py` and
`tools/gen_iw44tables.py` into generated includes, so their provenance is
explicit and they can be regenerated rather than hand-copied.

### 2.4 Rasters are bottom-up

DjVu bitmaps and pixmaps index row 0 as the **bottom** scanline. This is
easy to miss because it is a convention of the data structures rather than
anything stated in a chunk header, and getting it wrong is not obvious on a
bitonal page — it only shows once a background layer is composited against
a mask, as a mirrored image. Both layers are flipped on the way into this
viewer's top-down rasters.

---

## 3. What the viewer took from each

| From Acrobat | How it appears here |
|---|---|
| AS / Cos / PD / AV layering | `core/` → format back ends → `doc/Document` → `view/` + `ui/` |
| Render-to-memory as the primary path | `Document::Render` returns a bitmap; the window never renders |
| Placement as a matrix | one `LayoutEngine` owns zoom, rotation and layout together |
| `AVZoomType` | `ZoomMode::{kFitPage,kFitWidth,kFitVisibleWidth,kFitHeight,kActualSize,kCustom}` |
| Six PDF page layouts | `PageLayout` + the cover-page flag |
| Thumbnails in the document model | nav pane renders through the same cache as the page view |
| Selection as quads | `TextRun::box` in normalised page coordinates |
| Hand / select / marquee tools | `Tool` |
| Plug-in isolation by layer | PDF and DjVu back ends share only `Document` |

| From DjVu | How it appears here |
|---|---|
| IFF container, DIRM, INCL | `djvu/iff.*`, `djvu/document.*` |
| ZP, BZZ, JB2, IW44 | `djvu/zp.*`, `bzz.*`, `jb2.*`, `iw44.*` |
| Three-layer composition | `DjvuDocument::Render` |
| Hidden text zone tree | `TextRun`s, feeding selection and search |
| NAVM outline | `OutlineItem`, shown in the bookmarks panel |

## 4. Open questions

Things the available evidence could not settle:

* **How Acrobat rasterizes.** AGM exports only five symbols
  (`AGMInitialize`, `AGMGetVersion`, …); everything else is reached through
  internal tables. Without a decompiler its scan conversion, anti-aliasing
  and blend-mode handling are unknown.
* **Acrobat's cache and threading policy.** The `…Distributor` naming
  suggests a per-document observer, but neither the eviction policy nor the
  threading model is observable from names.
* **The `ix*` search index format.** Exported and clearly a stemming
  full-text engine, but its on-disk structure was not examined.
* **DjVu `Smmr`, `BGjp`/`FGjp`, `BG2k`/`FG2k`, `ANTa`/`ANTz`.** Understood
  from the format description but not implemented; no file in the corpus
  uses them. Pages carrying them render from whatever layers *are*
  supported rather than failing.
