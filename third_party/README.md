# Vendored source releases

| Archive | Source | SHA-256 |
| --- | --- | --- |
| `dist/libsigrok-0.5.2.tar.gz` | [sigrok release](https://sigrok.org/download/source/libsigrok/libsigrok-0.5.2.tar.gz) | `4d341f90b6220d3e8cb251dacf726c41165285612248f2c52d15df4590a1ce3c` |
| `dist/libsigrokdecode-0.5.3.tar.gz` | [sigrok release](https://sigrok.org/download/source/libsigrokdecode/libsigrokdecode-0.5.3.tar.gz) | `c50814aa6743cd8c4e88c84a0cdd8889d883c3be122289be90c63d7d67883fc0` |
| `dist/libzip-1.11.4.tar.gz` | [libzip release](https://libzip.org/download/libzip-1.11.4.tar.gz) | `82e9f2f2421f9d7c2466bbc3173cd09595a88ea37db0d559a9d0a2dc60dc722e` |

For libsigrok and libzip, CMake verifies these hashes before extracting into each build directory. It
builds static libraries with private prefixes; no Homebrew/system package is
modified. Upstream libsigrok needs libzip for session storage. The DSL frontend's
existing file-format implementation remains separate.

The archives contain their original license notices. libsigrok's top-level
license is GPL-3.0-or-later, with per-file notices in its source; libzip uses a
three-clause BSD license. See the copies in `licenses/` and original archives.
The upstream libsigrok source patch is `CMake/PatchSigrok.cmake`, applied at build
time to make the enabled driver registry safe under AddressSanitizer.

The libsigrokdecode archive is the pristine provenance copy for the C engine
imported into `libsigrokdecode4DSL`; CMake compiles those checked-in sources.
Its GPL-3.0-or-later license is copied into `licenses/`. DSView adaptations and
memory fixes are documented in `SIGROK_MIGRATION.md`. Existing DSView Python
scripts retain their individual licenses and are ported separately from the C
engine. Verify the provenance archive with:

```sh
shasum -a 256 third_party/dist/libsigrokdecode-0.5.3.tar.gz
```
