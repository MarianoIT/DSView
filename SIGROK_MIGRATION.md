# Sigrok controller and decoder migration

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
- The decoder engine is now based on official libsigrokdecode 0.5.3. The
  `libsigrokdecode4DSL` directory remains for existing install/resource paths;
  its C engine was imported from the verified official archive. The previous
  `0.6.0-git-3914467` label identified the DSView fork, not an official release.
  This pins the latest published release, not the upstream development branch.

## Decoder integration

`dsview_decode` builds the imported engine as a static library. DSView now sends
interleaved samples through the official `srd_session_send` ABI and starts opaque
sessions with `srd_session_start(session)`. `sigrok/decode-input.h` converts channel
bit planes, including non-byte offsets, constant channels and physical mappings.
Chunk boundaries are exclusive and clamped to the snapshot block and requested
range. The frontend no longer accesses the private session instance list.

Small DSView extensions retain numeric annotations, channel/option descriptions,
tags, annotation types, log integration and optional `end()` hooks. Parent hooks
flush before stacked children and share the capture endpoint. The 153 existing
DSView Python decoders are retained; scripts that used an integer `matched` mask
now adapt the official tuple via `common.sigrok_compat.match_mask`. This is not
an import of the entire upstream decoder catalog.

Memory corrections include dynamic annotation text vectors, bounded numeric
formatting, removal of an extra Python instance reference, recursive destruction
of stacked instances, and cleanup of synchronization resources. Native 0.5.3
also leaked match arrays across input chunks and lists of channel-map keys;
these are corrected without sanitizer suppressions. Python 3.14 uses its native
thread initialization rather than the removed `PyEval_InitThreads` call.
Decoder region controls are clipped before drawing, preventing an out-of-range
floating-point-to-integer conversion when a capture endpoint is offscreen.

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

## Decoder validation (2026-09-15)

Build and run without an attached instrument:

```sh
cmake --build build-port -j6
ctest --test-dir build-port --output-on-failure -LE hardware
cmake --build build-port-asan -j6
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-port-asan --output-on-failure -LE hardware
MallocStackLogging=1 leaks --atExit -- ./build.dir/dsview-decode-test
```

The protocol test loads all 153 production decoders, exercises 20 Clock sessions,
SPI byte A5 on remapped physical channel 9, malformed buffer rejection, and
bit-plane conversion with offsets and constant/missing channels. Test-only Python
fixtures exercise 20 stacked sessions, completion ordering/timestamps, 12 annotation
texts, signed numeric conversion, and propagation of intentional Python errors.
Loading a decoder is not a functional test of every supported protocol.

Physical decoder validation is available as `dsview-decode-test --hardware`
(or CTest `dsview-decode-hardware` with `DSVIEW_HARDWARE_TESTS=ON`). It captures
10,000 real DSO samples, thresholds them, decodes Clock in 17-sample chunks, and
compares annotation counts to independently counted edges. It requires a periodic
signal and rejects Demo. During this migration the instrument was not detected;
this new physical decoding check remains pending. The earlier controller hardware
results above do not establish physical validation of the new decoder engine.

Recorded decoder results:

| Check | Result |
| --- | --- |
| Release build and `ctest --test-dir build-port --output-on-failure -LE hardware` | Build succeeded; 5/5 passed |
| ASan/UBSan build and equivalent CTest command above | Build succeeded; 5/5 passed, no sanitizer report |
| Offscreen ASan/UBSan GUI startup, 8 seconds then SIGTERM | Remained running, no sanitizer report |
| Initial macOS `leaks` run | Identified 694 allocations / 27,408 bytes rooted in match arrays and channel-map key lists; both causes corrected |
| Repeated `leaks --atExit -- ./build.dir/dsview-decode-test` | macOS MallocStackLogging aborts in `uniquing_table_node_release_internal`; no final leak count available |
| `dsview-decode-test --hardware` and `dsview-cli devices list` | Physical instrument unavailable; enumeration contains only Demo |

The migration builds and passes software regression checks. Physical acceptance
and a final independent leak count are still outstanding. No rollback was made.
