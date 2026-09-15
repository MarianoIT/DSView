# macOS Apple Silicon migration plan

## Target

Produce a native `arm64` DSView executable and ensure every dynamically linked
dependency is also `arm64`. Do not build or package under Rosetta.

## Current assessment

- The application already has Darwin-specific UI and resource paths.
- `CMakeLists.txt` builds upstream libsigrok 0.5.2 and libzip 1.11.4 from
  verified source archives, plus the ported DSL protocol, custom decoder and
  minizip sources. These are not Homebrew sigrok packages.
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
3. Build and test. Use CTest; enable connected-device validation with
   `-DDSVIEW_HARDWARE_TESTS=ON`. See `SIGROK_MIGRATION.md` for sanitizer builds.
4. Inspect every linked non-system library before packaging:

   ```sh
   file build.dir/DSView
   otool -L build.dir/DSView
   ```

   The executable and Homebrew libraries must report `arm64`; no paths below
   `/usr/local` should remain.
5. Create a `.app` bundle, copy the `res`, `demo`, `lang`, and decoder assets,
   then use Qt's deployment tool and `install_name_tool` to make non-system
   library references bundle-relative. Sign and notarize the final bundle.

## Dependency update policy

Homebrew supplies the current compatible releases of CMake, Ninja, GLib,
libusb, zlib, Boost, FFTW, Python, Qt, and pkgconf. The minimum supported
versions in `INSTALL` were raised to a maintained baseline. The controller now runs on libsigrok 0.5.2 through an ABI-isolated bridge.
The custom decoder remains pinned. See `SIGROK_MIGRATION.md` for the port's
architecture, calibration storage limitation, tests and compatibility boundary.

## Acceptance checks

```sh
uname -m
cmake --build build-arm64
ctest --test-dir build-arm64 --output-on-failure
file build.dir/DSView
otool -L build.dir/DSView
```

The first command must print `arm64`, the build and tests must pass, and the
last two commands must show only `arm64` binaries and native dependency paths.