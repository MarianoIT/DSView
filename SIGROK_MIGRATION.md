# DSL controller on libsigrok 0.5.2

The DSL controller now runs on upstream libsigrok's device and session lifecycle.
The old `libsigrok4DSL/session.c` polling loop is removed. Version 0.5.2 is the
latest **released** libsigrok listed by [sigrok](https://sigrok.org/wiki/Downloads);
it is not the development branch. Upgrading alone does not establish memory safety.

## Architecture and compatibility

- `sigrok/core.c` uses the upstream ABI. It registers DSL driver callbacks,
  owns the libusb context, and runs real `sr_session_*` sessions and event sources.
- `libsigrok4DSL/core-adapter.c` bridges the existing frontend API to that core.
  The USB protocol, FPGA configuration, device profiles and DSO metadata remain
  in the DSL controller. Upstream's stock driver does not support the connected
  DSCope U3P100 (`2a0e:002b`, firmware 2.1).
- Legacy symbols are prefixed to prevent accidental binding to incompatible
  upstream functions or data structures. The CTest symbol audit checks both
  static archives, including input/output plugin descriptors.
- Proprietary packets travel as opaque vendor envelopes through the upstream
  session bus. This is a DSView integration, not a standalone sigrok-cli driver
  or conversion of proprietary packets to upstream analog packets. The bridge
  deliberately uses the private ABI of the pinned upstream release.
- `CMake/PatchSigrok.cmake` replaces upstream linker-section enumeration with
  bounded arrays for the two enabled stock drivers. This avoids reading ASan
  redzones between separately allocated globals; sanitizers are not suppressed.
- The custom `libsigrokdecode4DSL` decoder API remains in use. Updating it to
  official libsigrokdecode is still separate work; this controller port does
  **not** complete the broader request to update every dependency.

## Memory and lifecycle corrections

- Bound DSO payloads by received bytes, split instant samples from their metadata
  trailer, and keep trailer storage alive until complete.
- Join capture threads after natural completion as well as cancellation.
  Preserve cancellation requests arriving before the upstream session starts.
- Cancel and drain submitted libusb transfers on failed starts before freeing
  their storage; handle allocation and submission failures. Balance USB device
  references in scan results, retained device instances and reconnection events.
- Reapply the sample-rate divider at acquisition start after channel changes.
- Read the existing calibration record without overrunning its 30-byte size;
  decode packed VGA values without unaligned integer loads.
- Free shared log writers, bound long formatted messages and terminate domain
  strings. Skip unpositioned trace sentinels during GUI layout normalization.
- CLI device indices now address the same enumeration printed by `devices list`.
  Previously index zero could silently select Demo while a physical instrument
  was listed. Capture JSON now identifies the selected device.

### Calibration storage limitation

The previous reader/writer accessed 32 bytes while transferring a 30-byte NVM
record. The historical data includes digital gain but omits combined ADC gains
2 and 3. Existing byte positions, record sizes and address markers are preserved;
the two absent gains use unity. A save requiring non-unity values in those absent
fields returns an error before writing NVM, and the GUI reports save failures.
No calibration write or automatic calibration was used in hardware validation.
Supporting persistence of those two gains requires an explicitly versioned
storage format and instrument compatibility work.

## Build and validation

The vendored release archives and their hashes are documented in
[`third_party/README.md`](third_party/README.md). The build needs a native C/C++
toolchain, POSIX shell and make, CMake, pkg-config, GLib/GIO, libusb and the existing
DSView dependencies. It does not install or replace system sigrok libraries.

```sh
cmake -S . -B build-port -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DDSVIEW_HARDWARE_TESTS=ON
cmake --build build-port -j6
ctest --test-dir build-port --output-on-failure
./build.dir/dsview-cli --version
./build.dir/dsview-cli devices list
./build.dir/dsview-cli capture run --device-index 0 --mode dso \
  --samplerate 1000000 --samples 10000 --trigger-source auto
```

The hardware test requires a connected DSL oscilloscope and explicitly rejects
Demo. Leave `DSVIEW_HARDWARE_TESTS` off for builds without hardware. It performs
20 cycles alternating instant/continuous capture, one/two enabled channels,
immediate/delayed cancellation, and close/reopen. It checks sample counts and
reads the complete advertised payload under ASan. Continuous acquisition may
deliver additional complete frames before the stop request is processed.

```sh
cmake -S . -B build-port-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DDSVIEW_SANITIZE=ON -DDSVIEW_HARDWARE_TESTS=ON \
  -DDSVIEW_OUTPUT_DIR="$PWD/build-port-asan/bin"
cmake --build build-port-asan -j6
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-port-asan --output-on-failure
```

`DSVIEW_OUTPUT_DIR` isolates instrumented binaries and resources from the normal
`build.dir` output. Build type is honored instead of forcing Release and `-O3`.
`DSVIEW_CLI_DEBUG=1` enables protocol diagnostics in the CLI and hardware test.

## Validation boundary

Tested on macOS ARM64 with the connected DSCope U3P100, firmware 2.1. Hardware
tests cover capture, cancellation and reopen, not EEPROM writes, physical USB
unplug/replug, every sample rate, every DSL model or all GUI interactions.
An offscreen GUI startup smoke check is not an interactive GUI acceptance test.
Linux, Windows and cross-compilation have not been validated for this port.
Passing ASan/UBSan on these paths is not a claim that all memory defects are fixed.
Existing CLI voltage normalization is not a metrology/calibration validation.

## Recorded checks (2026-09-15)

| Command/check | Result |
| --- | --- |
| `cmake --build build-port -j6` | GUI, CLI and test targets built on ARM64 |
| `ctest --test-dir build-port --output-on-failure` | 5/5 passed, including 20 physical-device cycles |
| `cmake --build build-port-asan -j6` | Debug build with ASan and UBSan, including upstream core |
| `ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-port-asan --output-on-failure` | 5/5 passed; no sanitizer report |
| `MallocStackLogging=1 leaks --atExit -- ./build.dir/dsview-hardware-test 20` | 20 cycles passed; 0 leaks / 0 leaked bytes reported |
| `file build.dir/DSView build.dir/dsview-cli` | Both Mach-O ARM64 |
| `./build.dir/dsview-cli --version` | `dsview-cli libsigrok 0.5.2` |
| CLI physical capture with the command above | DSCope U3P100, 10,000 samples, raw codes 59–128; hardware frequency estimate 996.016 Hz |
| Offscreen ASan/UBSan GUI startup, 8 seconds, then SIGTERM | Opened DSCope U3P100 and remained running without a sanitizer report |

The macOS leak tool reported restricted inspection of writable contents because
of process debugging permissions, but completed its allocation/stack analysis.
No exclusion or sanitizer suppression was used. Detailed local execution logs
are in the ignored `build-sigrok-migration/artifacts/` directory.
