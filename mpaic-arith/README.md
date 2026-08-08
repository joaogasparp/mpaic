# Context-based Adaptive Arithmetic Compressor

Lossless compressor for grayscale astronomical images (8-bit and 16-bit RAW). It
combines an **adaptive arithmetic coder** with **configurable context modelling**,
where the neighbourhood used to predict each symbol is described by an external
configuration file.

This is the single-threaded arithmetic-coding variant of the mpaic family. See also:

- [mpaic-arith-mt](../mpaic-arith-mt/README.md) - multithreaded arithmetic variant
- [mpaic-range](../mpaic-range/README.md) - range-coder variant
- [mpaic-range-mt](../mpaic-range-mt/README.md) - multithreaded range-coder variant

## How it works

The compressor is a **context-based adaptive entropy coder**. It never sends the
probability model: encoder and decoder build the exact same statistics on the fly
from already-processed pixels, so the model stays perfectly synchronised. The
pipeline has four stages.

### 1. Symbol decomposition (phases)

Each pixel is a sample of 8 or 16 bits. It is split into fixed-size **symbols** of
`symbol-bits` bits (4 or 8), from the most significant group to the least
significant. Every symbol position is called a **phase**:

| Sample | `symbol-bits` | Symbols per sample | Phases |
| --- | --- | --- | --- |
| 8-bit | 8 | 1 | phase 0 |
| 8-bit | 4 | 2 | phase 0 (high nibble), phase 1 (low nibble) |
| 16-bit | 8 | 2 | phase 0 (high byte), phase 1 (low byte) |
| 16-bit | 4 | 4 | phase 0…3 |

Smaller symbols mean smaller alphabets ($2^{4}=16$ vs $2^{8}=256$), so each
context gathers statistics faster and the per-context memory is much smaller.

### 2. Context formation

For the symbol being coded, the model looks at a set of **causal neighbours**
given by the `(dx, dy)` offsets in the configuration. From each neighbour it takes
the symbol value at the *same phase* and concatenates those values into an integer
**context index**:

$$\text{ctx} = \sum_{i=0}^{k-1} v_i \cdot 2^{\,b\,(k-1-i)}$$

where $k$ is the number of offsets, $b$ is `symbol-bits`, and $v_i$ is the
neighbour symbol value. This means $k$ offsets with $b$-bit symbols produce up to
$2^{b\,k}$ distinct contexts. Neighbours falling outside the image contribute the
value `0`. Offsets **must be causal** (`dy < 0`, or `dy == 0` with `dx < 0`) so the
decoder has already reconstructed every neighbour when it needs it.

### 3. Adaptive probability model

Each context owns an `AdaptiveModel`: a frequency table over the alphabet. The
probability of symbol $s$ in context $c$ uses an **`alpha` pseudocount** ($n/d$)
for smoothing so unseen symbols never get zero probability:

$$P(s\mid c) = \frac{\text{count}_s + n}{\text{count}_\text{total} + n\,|S|}$$

Counts start at $n$ and each observation adds $d$, so the initial distribution is
uniform ($1/|S|$) and then adapts toward what the image actually contains.
Cumulative frequencies are maintained with a Fenwick (binary-indexed) tree for the
256-symbol alphabet and a fully unrolled scan for the 16-symbol alphabet. When a
count would exceed `max-total` the whole table is **rescaled** (halved), which both
bounds memory/precision and lets the model forget stale statistics and track local
image changes.

### 4. Arithmetic coding

The interval $[\text{cum\_low}, \text{cum\_high})$ of the current symbol is fed to a
binary **arithmetic coder** (Witten–Neal–Cleary, 31-bit precision). The coder
narrows a range proportionally to $P(s\mid c)$, emitting bits as the range
normalises. A rarer symbol costs close to $-\log_2 P(s\mid c)$ bits, so the output
size approaches the model's entropy. After coding, the model is `update`d with the
observed symbol and the next symbol is processed.

### shared vs split models

- **split**: one independent model per phase - statistics never mix between phases.
- **shared**: a single model spans all phases; per-phase context indices are
  offset into a common space so different phases can still be distinguished.

### Integrity

The header stores image geometry, the full context configuration, and a **CRC32**
of the original pixels. On decode with `-c/--compare` the reconstructed image is
hashed and checked against it, confirming the round-trip is lossless.

## Requirements

- A C++17 compiler (e.g. `g++`)
- `make`
- `bash`, `python3`, `bc`, and common Unix tools (only for `test.sh`)

The implementation targets POSIX systems and uses APIs such as `mmap` and
`mkstemp`; it does not build natively on Windows without a compatibility layer or
porting changes.

## Build

```bash
make
```

Produces two executables: `encoder` and `decoder`.

## Usage

### Encode

```bash
./encoder <input_image> [<output_file>] [options]
```

| Option | Description |
| --- | --- |
| `-r, --rows <n>` | Image height (required for RAW unless inferrable from filename) |
| `-c, --cols <n>` | Image width (required for RAW unless inferrable from filename) |
| `-b, --bpp <8\|16>` | Bits per pixel (default: 8) |
| `-e, --endian <le\|be>` | Endianness for 16-bit input (default: le) |
| `-f, --ctx-file <path>` | Context configuration file (see below) |
| `-v, --verbose` | Enable verbose encoder diagnostics |
| `-h, --help` | Show help |

If no output file is given, one is derived automatically with the `.cax`
extension. If `--rows`/`--cols` are omitted, geometry is auto-detected from the
filename when possible.

Example:

```bash
./encoder image.raw out.cax -r 1024 -c 1024 -b 16 -e le -f context_default.txt
```

### Decode

```bash
./decoder <input_file> [<output_file>] [options]
```

| Option | Description |
| --- | --- |
| `-c, --compare` | Verify the decoded image against the CRC32 stored in the header |
| `-v, --verbose` | Accepted for CLI compatibility; currently adds no decoder output |
| `-h, --help` | Show help |

## Context configuration file

Passing `-f <file>` overrides the compiled fallback configuration. The supplied
`context_default.txt` is a ready-to-use profile. Lines starting with `#` are
comments. Supported directives:

| Directive | Values | Meaning |
| --- | --- | --- |
| `byte-model` | `shared` \| `split` | Share one model across phases or keep one model per phase |
| `symbol-bits` | `4` \| `8` | Symbol size in bits (must divide the sample bit-depth) |
| `max-total` | `2`–`65535` | Frequency-count ceiling before rescaling |
| `alpha` | `n` or `n/d` | Pseudocount (initial frequency `n`, increment `d`) |
| `context` | `(dx,dy),(dx,dy),…` | Causal neighbour offsets |

`max-total`, `alpha`, and `context` can be given once (global) or once per phase.
All offsets must be **causal** (already decoded): `dy < 0`, or `dy == 0` with `dx < 0`.

Example ([context_default.txt](context_default.txt)):

```text
byte-model=shared
symbol-bits=4

max-total=4095
max-total=4095
max-total=4095
max-total=4095

alpha=1/2
alpha=1/2
alpha=1/2
alpha=1/2

context=(-1,0),(0,-1),(-1,-1),(0,-2)
context=(-1,0),(-2,0),(0,-1),(-1,-1)
context=(-1,0),(-2,0),(-3,0),(0,-1),(-1,-1)
context=(-1,0),(-2,0),(-4,0)
```

## Benchmark script

`test.sh` compresses and decompresses one or more RAW files, validates every
round-trip, and prints a per-file table plus aggregate statistics. Test datasets
are not included. Inputs must have geometry in their filename (for example,
`image_u16be-1x1024x1024.raw`) so the encoder can infer their dimensions. Set
`IMAGES_DIR` in the **USER CONFIGURATION** section near the top of `test.sh`, or
provide the path when invoking the script.

```bash
./test.sh                         # after configuring IMAGES_DIR in the script
IMAGES_DIR=/path/to/dataset ./test.sh
./test.sh path/to/dataset
./test.sh path/to/img1.raw path/to/img2.raw
DATASET_ROOT=/path/to/datasets ./test.sh INT
CTX_FILE=context_default.txt ./test.sh path/to/dataset
OUTPUT_ROOT=/tmp/cacx-results ./test.sh path/to/dataset
```

Output columns: `Bytes`, `BPS` (bits per sample), `Comp_Time`, `Decomp_Time`,
`Memory`, `Valid`, `Filename`. Generated files go to `compressed_images/` by
default and are ignored by Git.

## Output format

| Field | Description |
| --- | --- |
| Magic | `CAX1` |
| Version | `6` (32-bit length) or `7` (64-bit length) |
| Geometry | width, height, channels, bpp, pixel format, max value |
| Integrity | CRC32 of the original pixels |
| Model | symbol-bits, byte-model, per-phase offsets, `max-total`, `alpha` |
| Payload | arithmetic-coded bitstream |
