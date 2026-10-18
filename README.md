# dpdfcomp

A compressor for PDFs. By default, it will only perform lossless compression.

## Basic usage

```
dpdfcomp [options] input.pdf output.pdf
```

## Compression methods and options

### Font compression

By default, the following font optimizations occur. To disable all, use `--no-fonts`.
* Type 1 fonts are compressed to Type 1C/CFF. — To disable, use `--no-type1-to-cff`.
* Subsetting is applied to all fonts. — To disable, use `--no-font-subset`.
* Fonts with identical characters are merged. — To disable, use `--no-font-merge`.

### Image compression

By default, the following image optimizations occur. To disable all, use `--no-images`. Three different categories of image are considered, and the compression settings for each can be set separately. For each, you can specify a list of methods to try after the equals sign, or use `all` to enable all lossless methods (lossy ones must be enabled manually).

To disable compression of a category of images, set codec to `none`.

If an image does not shrink with any of the specified output codecs, then the original image will be used instead. This applies even if the original image is not in one of the specified output formats. For example, if an input image is in JBIG2, then it will stay as JBIG2 in the output file, even if JBIG2 is not one of your specified output formats. To override this, add `specified-only` to the list of output codecs, which will result in the smallest of your selected codecs being used.

By default, true bilevel images which are encoded in a color codec will be re-encoded as bilevel automatically. To disable that function, use `--no-detect-bilevel`.

* Bilevel images (default: `--bilevel=flate,ccitt`)
	* `flate` - Inflate/deflate encoding
	* `ccitt` - CCITT (Group4/fax compression)
	* `png` or `png-XX` - PNG via zopfliPNG (with XX iterations; 15 if not specified)
	* `jbig2` - JBIG2
* Color images (separate settings for lossless and lossy originals; defaults: `--color-lossless=lzw,zip` and `--color-lossy=none`)
	* `lzw` - TIFF with LZW compression
	* `zip` - TIFF with ZIP compression
	* `png` or `png-XX` - PNG via zopfliPNG (with XX iterations; 15 if not specified)
	* `jp2-lossless` - JPEG-2000 (lossless)
	* `jpeg-XX` - JPEG with quality setting XX% (*lossy*)
	* `jp2-lossy-XX` - JPEG-2000 with quality setting XX% (*lossy*)
	* `bilevel` or `bilevel-XX` - Convert to bilevel (at specified threshold; 50% if not specified) — uses bilevel settings afterwards

White margins can be cropped from images internally using `--crop-internal`. This is technically lossy (since the images, if extracted, will no longer have their original dimensions), though visually lossless, as the placement on the page will be maintained (with a white background added if necessary).

When using JBIG2, the internal striping of JBIG2 images can be controlled with option `--jbig2-stripe=X`, which will set those stripes to be of height *X*. JBIG2 striping does not result in an image being split into multiple images internally, since it is a feature of the JBIG2 codec itself.

Images can be separated into stripes of height *X* using option `--pdf-stripe=X`. This will result in the images actually being split within the PDF, which, though common, may alter rendering in some viewers, and this is technically semi-lossy, because the stripes would be extracted from the PDF as separate images. `--reconstruct-stripes` can be used to merge striped images back into a single image.

## File-level options

By default, the version of the PDF standard used will be upgraded if needed in order to use specified codecs. To override this, use `--no-upgrade`, which will only allow codecs used in the version of the PDF standard used in the input file.

If the input PDF is linearized, then the output will be linearized as well. In order to linearize the output when the input is not already linearized, add option `--linearize`.

# Installation

* MacOS users can install via [Homebrew](https://brew.sh): `brew install dbenjaminmiller/tap/dpdfcomp`.
* Windows users can download a binary build from the "Releases" tab.
* Linux users will currently need to build from source.

# Building

Internally, dpdfcomp is based on a library, libdpdfcomp; the command-line interface is a thin frontend.

To build, make sure that you have a compiler for C++20 and C11, as well as CMake (3.24+) and pkg-config/pkgconf. GNU Make may optionally be used; the Mkaefile wraps the CMake configuration.

## Dependencies

* QPDF (11 or newer)
* zlib
* libtiff
* libjpeg-turbo
* openjpeg
* ZopfliPNG (FontTools fork; selected sources included)
* PDFium JBIG2/fax decoder (selected sources included)
* FreeType
* libxm2 headers
* Leptonica
* oneTBB
* afdko 5.0.1 (patched; sources included)
* jbig2enc 0.32 (sources included)

## MacOS

1. Install dependencies: `brew install cmake pkgconf qpdf zlib libtiff freetype libxml2 leptonica tbb jpeg-turbo openjpeg`
2. Make: `make dynamic` or `make static`

## Linux

1. Install dependencies: `sudo apt install build-essential cmake pkg-config libqpdf-dev zlib1g-dev libtiff-dev libfreetype-dev libxml2-dev libleptonica-dev libtbb-dev libturbojpeg0-dev libopenjp2-7-dev` (or equivalent; this example is for Debian/Ubuntu)
2. Make `make dynamic` or `make static`

## Windows

Use a recent Visual Studio/MSVC or MinGW toolchain and matching dependency packages. CMake and pkg-config must be able to find their headers and libraries. For static linking, use static packages and a matching static C runtime, such as vcpkg's `x64-windows-static` triplet. Pass your dependency manager's toolchain file to CMake. Do not mix static and dynamic Windows package variants.

The following Makefile commands use GNU Make, CMake, Ninja, and pkg-config on `PATH`. The same GNU Make wrapper works with either compiler; it is not an NMake makefile. Install the dependencies listed above for the selected triplet first, and replace `C:/vcpkg` with your vcpkg installation directory.

For MinGW, run the following in an MSYS2/MinGW shell with the intended GCC toolchain on `PATH`: `make dynamic CMAKE_ARGS="-G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-mingw-dynamic"` or  `make static CMAKE_ARGS="-G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-mingw-static"`

For MSVC, run the following in a Visual Studio Developer Command Prompt with the intended
target architecture selected and GNU Make available on `PATH`: `make dynamic CMAKE_ARGS="-G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows"` or `make static CMAKE_ARGS="-G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static"`

These examples build for AMD64 (x86_64). For ARM64 builds on MSVC, use `arm64-windows` or `arm64-windows-static` instead.

For ARM64 (AArch64) MinGW-w64, use an [LLVM-MinGW UCRT toolchain](https://github.com/mstorsjo/llvm-mingw/releases) for your build host. Put its `bin` directory on `PATH`, alongside GNU Make, CMake, Ninja, and pkg-config. Install the dependencies listed above into separate ARM64 dynamic and static prefixes, using the same LLVM-MinGW toolchain and C++ runtime. Each prefix must contain the target headers, libraries, CMake packages, and pkg-config metadata. The static build fetches FreeType and oneTBB itself.

(The following instructions can also be adapted for cross-compiling using MinGW on other platforms.)

Save the following as `mingw-arm64.cmake` in the source directory:


```
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER aarch64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER aarch64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH
    "$ENV{DPDFCOMP_DEPS}"
    "$ENV{LLVM_MINGW_ROOT}/aarch64-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Some dependency exports contain bare library names, such as QPDF's z.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-L\"$ENV{DPDFCOMP_DEPS}/lib\"")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-L\"$ENV{DPDFCOMP_DEPS}/lib\"")
```

Run these commands in a Bash shell after adding the toolchain's `bin` directory to `PATH`, adjusting the paths to your installations:

```
export LLVM_MINGW_ROOT="C:/llvm-mingw"
export PKG_CONFIG_PATH=

export DPDFCOMP_DEPS="C:/deps/dpdfcomp-arm64-dynamic"
export PKG_CONFIG_LIBDIR="$DPDFCOMP_DEPS/lib/pkgconfig"
make dynamic BUILD_ROOT=build-mingw-arm64 CMAKE_ARGS="-G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/mingw-arm64.cmake -DCMAKE_PREFIX_PATH=$DPDFCOMP_DEPS"

export DPDFCOMP_DEPS="C:/deps/dpdfcomp-arm64-static"
export PKG_CONFIG_LIBDIR="$DPDFCOMP_DEPS/lib/pkgconfig"
make static BUILD_ROOT=build-mingw-arm64 CMAKE_ARGS="-G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/mingw-arm64.cmake -DCMAKE_PREFIX_PATH=$DPDFCOMP_DEPS"
```

Outputs are `build-mingw-arm64-dynamic/dpdfcomp.exe` and `build-mingw-arm64-static/dpdfcomp.exe`. Copy the dynamic library and dependency DLLs with the dynamic executable. Keep the matching `DPDFCOMP_DEPS` and `PKG_CONFIG_LIBDIR` settings when reconfiguring either build directory.