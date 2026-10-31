# Font notice

The Aether UI text (`src/gui/fontdata.c`) is a set of bitmap renderings of **Poppins Regular and Poppins Medium**,
Copyright 2020 The Poppins Project Authors (https://github.com/itfoundry/Poppins), licensed under the
**SIL Open Font License, Version 1.1** (https://openfontlicense.org). The font names carry no Reserved Font Name.

`tools/gen-font-aa.py` rasterises the glyphs (printable ASCII, sizes 11-14 and 20 px) into coverage tables; the original
font files are not shipped in the image and are only needed to regenerate `fontdata.c`. The OFL permits this embedding and
derived rendering provided the copyright notice and licence are kept with the software, which this file does.

The legacy 8x16 bitmap font (`font8x16.c`, used only by the old text API) was generated from DejaVu Sans Mono (Bitstream Vera / DejaVu licence).
