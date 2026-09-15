![DreamSourceLab Logo](DSView/icons/dsl_logo.svg)


# DSView 
DSView is a GUI program for supporting various instruments from [DreamSourceLab](http://www.dreamsourcelab.com), including logic analyzers, oscilloscopes, etc. DSView is based on the [sigrok project](https://sigrok.org).

The sigrok project aims at creating a portable, cross-platform, Free/Libre/Open-Source signal analysis software suite that supports various device types (such as logic analyzers, oscilloscopes, multimeters, and more).

# Status

The DSView software is in a usable state and has official tarball releases. However, it is still a work in progress. Some basic functionality is available and working, but other things are always on the TODO list.

# Building

DSView requires CMake 3.21 or newer, a C99/C++11 compiler, Qt 6, GLib, libusb,
zlib, Boost, FFTW, Python development headers, and pkg-config. Build from a
clean source checkout; do not reuse a build directory across platforms or CPU
architectures.

## Linux

On Debian or Ubuntu, install the build dependencies:

```sh
sudo apt update
sudo apt install build-essential cmake ninja-build pkg-config qt6-base-dev \
	libglib2.0-dev libusb-1.0-0-dev zlib1g-dev libboost-dev libfftw3-dev \
	python3-dev
```

Configure and build:

```sh
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux
sudo cmake --install build-linux
```

## macOS Apple Silicon

Install Xcode Command Line Tools and native Apple Silicon Homebrew, then install
the dependencies:

```sh
xcode-select --install
brew update
brew install cmake ninja glib libusb zlib boost fftw python@3.12 qt pkgconf
```

Configure a native `arm64` build:

```sh
cmake -S . -B build-arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_OSX_ARCHITECTURES=arm64 \
	-DCMAKE_PREFIX_PATH="$(brew --prefix qt);$(brew --prefix)"
cmake --build build-arm64
cmake --install build-arm64
```

The executable is written to `build.dir/DSView`. Verify it is native before
packaging:

```sh
file build.dir/DSView
otool -L build.dir/DSView
```

## Automation CLI

The build also produces `build.dir/dsview-cli`, a non-GUI command-line interface
that writes one JSON document to standard output. It is intended for scripts and
LLM-driven tooling.

```sh
build.dir/dsview-cli devices list
build.dir/dsview-cli capture run --device-index 0 --mode logic \
	--samplerate 1000000 --samples 10000 --channel 0 --timeout-ms 5000
```

DSO captures use a bounded timeout and report `capture_timeout` when the
instrument does not complete the acquisition. Set `DSVIEW_CLI_DEBUG=1` to write
driver diagnostics to standard error without affecting JSON standard output.

## Windows

Use the MSYS2 MinGW64 environment. Install MSYS2, open **MSYS2 MinGW x64**, and
install the matching compiler, Qt 6, and libraries:

```sh
pacman -Syu
pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake \
	mingw-w64-x86_64-ninja mingw-w64-x86_64-pkgconf \
	mingw-w64-x86_64-qt6-base mingw-w64-x86_64-glib2 \
	mingw-w64-x86_64-libusb mingw-w64-x86_64-zlib \
	mingw-w64-x86_64-boost mingw-w64-x86_64-fftw \
	mingw-w64-x86_64-python
```

Configure and build from the MinGW64 shell:

```sh
cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows
cmake --install build-windows
```

Run the generated executable from the MinGW64 environment, or deploy the Qt and
MinGW runtime DLLs with the application before distributing it.

# Useful links

- [dreamsourcelab.com](https://www.dreamsourcelab.com)
- [kickstarter.com](https://www.kickstarter.com/projects/dreamsourcelab/dslogic-multifunction-instruments-for-everyone)
- [sigrok.org](https://sigrok.org)

# Copyright and license

DSView software is licensed under the terms of the GNU General Public License
(GPL), version 3 or later.

While some individual source code files are licensed under the GPLv2+, and
some files are licensed under the GPLv3+, this doesn't change the fact that
the program as a whole is licensed under the terms of the GPLv3+ (e.g. also
due to the fact that it links against GPLv3+ libraries).

Please see the individual source files for the full list of copyright holders.
