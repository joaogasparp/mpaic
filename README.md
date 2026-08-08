# Multi-Phase Astronomical Image Compressors

**Version 1.0**

Source-only distribution of four context-based lossless compressors for 8-bit and 16-bit grayscale astronomical images. The collection provides two entropy coders, each in a single-threaded and a band-parallel multithreaded implementation.

| Variant                                    | Entropy coder     | Parallelism      | Stream format     |
| ------------------------------------------ | ----------------- | ---------------- | ----------------- |
| [mpaic-arith](mpaic-arith/README.md)       | Arithmetic coding | Single thread    | `.cax` (`CAX1`)   |
| [mpaic-arith-mt](mpaic-arith-mt/README.md) | Arithmetic coding | Horizontal bands | `.caxmt` (`CAXM`) |
| [mpaic-range](mpaic-range/README.md)       | Range coding      | Single thread    | `.crx` (`CRX1`)   |
| [mpaic-range-mt](mpaic-range-mt/README.md) | Range coding      | Horizontal bands | `.crxmt` (`CRXM`) |

The four stream formats are distinct. Decode a file with the decoder from the same variant that produced it.

## Build

Requirements are a C++17 compiler and `make`; multithreaded variants also require pthreads. Build every variant from this directory with:

```bash
make
```

Alternatively, build one variant directly:

```bash
make -C mpaic-arith
```

Each build produces `encoder` and `decoder` inside that variant's directory. Use `make clean` at the collection root to remove all eight executables.

## Quick start

```bash
cd mpaic-arith
make
./encoder image.raw image.cax -r 1024 -c 1024 -b 16 -e le -f context_default.txt
./decoder image.cax image.decoded.raw -c
```

RAW geometry can also be inferred from filenames ending in a form such as `_u16be-1x1024x1024.raw`. See each variant's README for all options, context-file syntax, format details, and the portable round-trip benchmark helper.

## Distribution contents

Each variant contains only its source files, `Makefile`, README, context profile, and test helper. Prebuilt executables, compressed/decompressed datasets, benchmark logs, and local machine paths are intentionally excluded. Generated files are covered by the root `.gitignore`.

The code targets POSIX systems and uses facilities including memory mapping, temporary files, resource accounting, and pthreads. Native Windows builds require a compatibility layer or porting changes.

## License

This project is licensed under the MIT License. See the LICENSE file for details.

The entropy-coder components retain their original attribution comments. See the corresponding source files for upstream acknowledgements.
