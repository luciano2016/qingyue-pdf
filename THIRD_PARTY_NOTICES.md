# Third-Party Notices

This file records third-party software incorporated into or distributed with 轻阅PDF. It does not replace the license terms of those projects.

## MuPDF 1.28.0

- Project: MuPDF
- Copyright: Artifex Software, Inc. and contributors
- Website: https://mupdf.com/
- Source archive: https://mupdf.com/downloads/archive/mupdf-1.28.0-source.tar.gz
- Archive SHA256: `21C7F064903154F1C3A7458BEE81F130FC36F9B5147EA13328F9980E02D2DEA2`
- License: GNU Affero General Public License version 3, with commercial licensing available separately from Artifex

轻阅PDF links MuPDF into `quietmupdf.dll`. The repository does not vendor the complete MuPDF source tree; `scripts/fetch-mupdf.ps1` retrieves and verifies the exact source release used by the build. The AGPLv3 text is provided in the repository root [LICENSE](LICENSE).

MuPDF 1.28.0 bundles FreeType 2.14.3, HarfBuzz 6.0.0, IJG libjpeg 10.0, Little CMS 2.16, OpenJPEG 2.5.4, zlib 1.3.2, Gumbo Parser 0.10.1, Brotli 1.1.0, cmark-gfm 0.29.0.gfm.13, Tesseract 5.5.2, Leptonica 1.87.0, Zint 2.13.0.9, ZXing-C++ 2.3.0, jbig2dec, and MuJS. These components use BSD-style, MIT-style, zlib, Apache-2.0, AGPLv3, ISC, FreeType, and project-specific permissive terms. Exact license texts copied from the verified MuPDF source archive are provided in [LICENSES/MUPDF_THIRD_PARTY_LICENSES.txt](LICENSES/MUPDF_THIRD_PARTY_LICENSES.txt) and installed with binary releases.

## SumatraPDF Windows system-font loader

`native/quietmupdf/mupdf_load_system_font.c` is adapted from `ext/mupdf_load_system_font.c` in SumatraPDF.

- Project: SumatraPDF
- Copyright: SumatraPDF project authors
- Source: https://github.com/sumatrapdfreader/sumatrapdf
- License: GNU General Public License version 3
- License text: [LICENSES/GPL-3.0.txt](LICENSES/GPL-3.0.txt)

The adapted loader resolves Windows fonts referenced by name but not embedded in a PDF. The source file retains an attribution header identifying its origin and license.

## Microsoft Visual C++ Runtime

Windows installer builds may redistribute `vcruntime140.dll` from the installed Microsoft Visual C++ toolchain. This component is copyright Microsoft Corporation and is distributed under the Microsoft Visual Studio licensing terms, not under AGPLv3.

## No endorsement

轻阅PDF is an independent project. Artifex, MuPDF, SumatraPDF, and Microsoft do not sponsor or endorse it.
