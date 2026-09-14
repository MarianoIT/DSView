# macOS Apple Silicon migration plan

## Target

Produce a native `arm64` DSView executable and ensure every dynamically linked
dependency is also `arm64`. Do not build or package under Rosetta.

## Current assessment

- The application already has Darwin-specific UI and resource paths.
- `CMakeLists.txt` builds `libsigrok4DSL`, `libsigrokdecode4DSL`, and minizip
  from the repository, so these are source dependencies rather than Homebrew
  packages.
- External dependencies are GLib, Python development headers, FFTW, libusb,
  zlib, Qt, Boost, threads, and pkg-config.
- The previous custom find modules only searched Intel-oriented `/usr/local`
  paths. They now search `/opt/homebrew`, the default Apple Silicon Homebrew
  prefix.
- The legacy CMake baseline and minimum versions in `INSTALL` predate current
  macOS/Apple Silicon tooling.

## Conversion steps

1. Install/update the external packages with the command in `INSTALL`. Run it
   from a native terminal where `uname -m` prints `arm64`.
2. Configure only with the `build-arm64` command in `INSTALL`; do not reuse a
   cache created under Intel Homebrew or Rosetta.
3. Build and test. Enable project tests with `-DENABLE_TESTS=ON` when their
   dependencies are available.
4. Inspect every linked non-system library before packaging:

   ```sh
   file build-arm64/DSView
   otool -L build-arm64/DSView
   ```

   The executable and Homebrew libraries must report `arm64`; no paths below
   `/usr/local` should remain.
5. Create a `.app` bundle, copy the `res`, `demo`, `lang`, and decoder assets,
   then use Qt's deployment tool and `install_name_tool` to make non-system
   library references bundle-relative. Sign and notarize the final bundle.

## Dependency update policy

Homebrew supplies the current compatible releases of CMake, Ninja, GLib,
libusb, zlib, Boost, FFTW, Python, Qt, and pkgconf. The minimum supported
versions in `INSTALL` were raised to a maintained baseline. The embedded
libsigrok4DSL and libsigrokdecode4DSL copies remain pinned because upgrading
them requires an upstream API compatibility pass and hardware capture tests;
they must be upgraded together in a dedicated change, not substituted by
system libsigrok packages.

## Acceptance checks

```sh
uname -m
cmake --build build-arm64
ctest --test-dir build-arm64 --output-on-failure
file build-arm64/DSView
otool -L build-arm64/DSView
```

The first command must print `arm64`, the build and tests must pass, and the
last two commands must show only `arm64` binaries and native dependency paths.