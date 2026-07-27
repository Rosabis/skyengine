# stb_truetype

`stb_truetype.h` is vendored from the upstream `nothings/stb` repository and
is used only to rasterize trusted operating-system font files on Linux and
Android.

- Upstream: <https://github.com/nothings/stb>
- Revision: `31c1ad37456438565541f4919958214b6e762fb4`
- Header version: 1.26
- SHA-256: `ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab`
- License: public domain or MIT, as stated in the header

MRP-provided files are not passed to this parser. The selected input is a
font file resolved by Fontconfig or found in Android's read-only system font
directories.
