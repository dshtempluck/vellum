<#
.SYNOPSIS
  Downloads the DjVu test corpus and generates a test PDF.

.DESCRIPTION
  The test documents are not committed: they belong to the DjVuLibre
  distribution, not to this project. This fetches them into samples/ on
  demand, and writes a small PDF of its own so that the PDF back end can
  be exercised without shipping anyone else's file.

  Run once after cloning:

      .\tools\fetch-samples.ps1

.PARAMETER Force
  Re-download files that are already present.
#>
param([switch]$Force)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$samples = Join-Path $root "samples"
New-Item -ItemType Directory -Force $samples | Out-Null

# The DjVuLibre documentation set: the DjVu v2 and v3 specifications, the
# DjVuLibre manual in two languages, and two scanned letters. Between them
# they cover bitonal masks, shared shape dictionaries, IW44 colour
# backgrounds, hidden text layers and outlines.
$base = "https://raw.githubusercontent.com/barak/djvulibre/master/doc"
$files = @(
  "djvu2spec.djvu",
  "djvu3spec.djvu",
  "djvulibre-book-en.djvu",
  "djvulibre-book-ru.djvu",
  "lizard2002.djvu",
  "lizard2007.djvu"
)

foreach ($name in $files) {
  $dest = Join-Path $samples $name
  if ((Test-Path $dest) -and -not $Force) {
    Write-Host "  have    $name" -ForegroundColor DarkGray
    continue
  }
  Write-Host "  fetch   $name"
  Invoke-WebRequest -Uri "$base/$name" -OutFile $dest -UseBasicParsing -TimeoutSec 120

  # Every DjVu file starts with the four-byte AT&T preamble; anything else
  # means we were served an error page rather than the document.
  $fs = [IO.File]::OpenRead($dest)
  $magic = New-Object byte[] 4
  $fs.Read($magic, 0, 4) | Out-Null
  $fs.Close()
  if ([Text.Encoding]::ASCII.GetString($magic) -ne "AT&T") {
    Remove-Item $dest
    throw "$name did not download as a DjVu file"
  }
}

# ---------------------------------------------------------------------------
# A minimal PDF, written here rather than copied from anywhere, so the PDF
# path has something to open on a fresh clone.
# ---------------------------------------------------------------------------
$pdfDir = Join-Path $samples "pdf"
New-Item -ItemType Directory -Force $pdfDir | Out-Null
$pdfPath = Join-Path $pdfDir "hello.pdf"

if ((Test-Path $pdfPath) -and -not $Force) {
  Write-Host "  have    pdf/hello.pdf" -ForegroundColor DarkGray
} else {
  Write-Host "  write   pdf/hello.pdf"

  $content = @"
BT /F1 28 Tf 72 700 Td (Vellum test page) Tj ET
BT /F1 12 Tf 72 664 Td (A generated PDF, for exercising the PDF back end.) Tj ET
BT /F1 12 Tf 72 644 Td (Letter size, one page, Helvetica, no compression.) Tj ET
2 w 72 628 m 400 628 l S
"@
  # Each object is built as one string. Splitting a dictionary across a
  # concatenation inside the array literal silently turns it into two
  # separate objects, which produces a structurally valid but blank page.
  $page = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>"
  $stream = "<< /Length $($content.Length) >>`nstream`n$content`nendstream"

  $objects = @(
    "<< /Type /Catalog /Pages 2 0 R >>",
    "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    $page,
    $stream,
    "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"
  )

  # Build the file while recording each object's byte offset for the xref
  # table, which is what makes a PDF randomly accessible.
  $sb = New-Object System.Text.StringBuilder
  [void]$sb.Append("%PDF-1.4`n")
  $offsets = @()
  for ($i = 0; $i -lt $objects.Count; $i++) {
    $offsets += $sb.Length
    [void]$sb.Append("$($i + 1) 0 obj`n$($objects[$i])`nendobj`n")
  }
  $xref = $sb.Length
  [void]$sb.Append("xref`n0 $($objects.Count + 1)`n")
  [void]$sb.Append("0000000000 65535 f `n")
  foreach ($o in $offsets) {
    [void]$sb.Append(("{0:D10} 00000 n `n" -f $o))
  }
  [void]$sb.Append("trailer`n<< /Size $($objects.Count + 1) /Root 1 0 R >>`n")
  [void]$sb.Append("startxref`n$xref`n%%EOF`n")

  # Write as Latin-1 so that one character is one byte and the recorded
  # offsets stay correct.
  [IO.File]::WriteAllText($pdfPath, $sb.ToString(),
                          [Text.Encoding]::GetEncoding(28591))
}

Write-Host "`nsamples ready in $samples" -ForegroundColor Green
Get-ChildItem $samples -Recurse -File |
  ForEach-Object { "  {0,9:N0}  {1}" -f $_.Length, $_.Name }
