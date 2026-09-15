# Vendored source releases

| Archive | Source | SHA-256 |
| --- | --- | --- |
| `dist/libsigrok-0.5.2.tar.gz` | [sigrok release](https://sigrok.org/download/source/libsigrok/libsigrok-0.5.2.tar.gz) | `4d341f90b6220d3e8cb251dacf726c41165285612248f2c52d15df4590a1ce3c` |
| `dist/libzip-1.11.4.tar.gz` | [libzip release](https://libzip.org/download/libzip-1.11.4.tar.gz) | `82e9f2f2421f9d7c2466bbc3173cd09595a88ea37db0d559a9d0a2dc60dc722e` |

CMake verifies these hashes before extracting into each build directory. It
builds static libraries with private prefixes; no Homebrew/system package is
modified. Upstream libsigrok needs libzip for session storage. The DSL frontend's
existing file-format implementation remains separate.

The archives contain their original license notices. libsigrok's top-level
license is GPL-3.0-or-later, with per-file notices in its source; libzip uses a
three-clause BSD license. See the copies in `licenses/` and original archives.
The only upstream source patch is `CMake/PatchSigrok.cmake`, applied at build
time to make the enabled driver registry safe under AddressSanitizer.
