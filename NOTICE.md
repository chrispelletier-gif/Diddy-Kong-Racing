# Third-party notices

The decompilation is CC0 and the 3DS port (`port/`) is MIT (see [LICENSE.md](LICENSE.md) and
[port/LICENSE](port/LICENSE)). These parts are not, or are linked into the built
3DS game (`dkr.3dsx`, `dkr.cia`) from devkitPro's packages. Their licences ask that the notices below go with the
game when it is shared; the release notes link to this file.

## Linked into the 3DS game

| Library | Used for | Licence |
| --- | --- | --- |
| [libctru](https://github.com/devkitPro/libctru) | 3DS system services | zlib |
| [citro3d](https://github.com/devkitPro/citro3d) | 3DS GPU | zlib |
| [zlib](https://zlib.net/) | Compressed data in bug reports | zlib |
| [libcurl](https://curl.se/) | HTTPS for the debug build's updater and bug reports | curl (MIT style) |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | TLS for libcurl | Apache 2.0 |
| newlib and the devkitARM runtime | C library | BSD-style (see devkitPro) |

libcurl and Mbed TLS are linked into the player build too, though only the debug build's tools use them.

- **zlib licence** (libctru, citro3d, zlib): the software is provided as-is; the origin must not be misrepresented,
  altered versions must be marked as such, and the notice must stay with the source.
- **curl licence**: Copyright (c) Daniel Stenberg and many contributors. Permission to use, copy, modify and
  distribute this software for any purpose with or without fee is granted, provided that the copyright notice and
  this permission notice appear in all copies.
- **Apache License 2.0** (Mbed TLS): Copyright The Mbed TLS Contributors. Licensed under the Apache License, Version
  2.0 (https://www.apache.org/licenses/LICENSE-2.0). Distributed on an "as is" basis, without warranties or
  conditions of any kind.

## In this repository

| Path | What | Licence |
| --- | --- | --- |
| `port/src/plat/third_party/stb_image.h` | PNG loader (SD card portraits) | Public domain or MIT, your choice (see the end of the file) |
| `port/3ds/github_ca.h` | Root certificates for GitHub (Sectigo/USERTrust, DigiCert, ISRG), from Mozilla's list | Public certificates |
| `tools/n64crc.c` | N64 ROM checksum tool used by the decompilation's N64 build (not part of the 3DS game) | GPL 2.0 or later |
| `port/tools/n64_compare/` | Test harness for the ares emulator (not part of the 3DS game) | MIT, like the rest of `port/`; ares itself is ISC and is downloaded, not included |

## App icon and banner

The 3DS icon and HOME menu banner (`port/3ds/icon.png`, `port/3ds/cia/`) are original artwork for this project. Their
lettering is set in the [Lilita One](https://fonts.google.com/specimen/Lilita+One) font, under the SIL Open Font
License 1.1, which allows it in images; the font file itself is not included. The banner jingle was synthesized
for this project. No Nintendo or Rare artwork, logos or audio are used.

## Build tools (not distributed)

devkitARM, 3dsxtool, smdhtool and picasso (devkitPro), and for the .cia makerom and bannertool, are used to build
the game but are not part of it.
