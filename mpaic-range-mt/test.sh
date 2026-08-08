#!/bin/bash

export LC_NUMERIC=C

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENCODER="$SCRIPT_DIR/encoder"
DECODER="$SCRIPT_DIR/decoder"
CTX_FILE="${CTX_FILE:-}"
DATASET_ROOT="${DATASET_ROOT:-$SCRIPT_DIR/test-data}"
OUTPUT_ROOT="${OUTPUT_ROOT:-$SCRIPT_DIR/compressed_images}"

# =============================================================================
# USER CONFIGURATION
# Set the directory that contains the RAW images to test, then run `./test.sh`.
# The value may also be supplied without editing this file:
#   IMAGES_DIR="/path/to/your/raw/images" ./test.sh
#
# Example (uncomment and edit):
# IMAGES_DIR="/data/astronomical_images/INT"
IMAGES_DIR="${IMAGES_DIR:-}"
# =============================================================================

CTX_ARGS=()
THREAD_ARGS=()

usage() {
    echo "Usage: $0                                  # use IMAGES_DIR configured near the top" >&2
    echo "       $0 <raw-file> [<raw-file> ...]" >&2
    echo "       $0 <dataset-directory>" >&2
    echo "       DATASET_ROOT=<directory> $0 <dataset-name>" >&2
}

if [ $# -eq 0 ]; then
    if [ -z "$IMAGES_DIR" ]; then
        echo "Error: set IMAGES_DIR near the top of this script or pass an input path." >&2
        usage
        exit 2
    fi
    if [ ! -d "$IMAGES_DIR" ]; then
        echo "Error: image directory not found: $IMAGES_DIR" >&2
        exit 2
    fi
    INPUT_DIR="$(cd "$IMAGES_DIR" && pwd)"
    DATASET="$(basename "$INPUT_DIR")"
    INPUT_FILES=("$INPUT_DIR"/*.raw)
elif [ $# -eq 1 ] && [ -d "$1" ]; then
    INPUT_DIR="$(cd "$1" && pwd)"
    DATASET="$(basename "$INPUT_DIR")"
    INPUT_FILES=("$INPUT_DIR"/*.raw)
elif [ $# -eq 1 ] && [ ! -f "$1" ]; then
    DATASET="$1"
    INPUT_DIR="$DATASET_ROOT/$DATASET"
    if [ ! -d "$INPUT_DIR" ]; then
        echo "Error: dataset directory not found: $INPUT_DIR" >&2
        usage
        exit 2
    fi
    INPUT_FILES=("$INPUT_DIR"/*.raw)
else
    INPUT_FILES=("$@")
    DATASET=$(basename "$(dirname "$1")")
fi

OUTPUT_DIR="$OUTPUT_ROOT/$DATASET"
mkdir -p "$OUTPUT_DIR"

num_input_files=0
for input_file in "${INPUT_FILES[@]}"; do
    [ -f "$input_file" ] && ((num_input_files++))
done
if [ "$num_input_files" -eq 0 ]; then
    echo "Error: no RAW input files found." >&2
    exit 2
fi

if [ -n "$CTX_FILE" ]; then
    if [ ! -f "$CTX_FILE" ]; then
        echo "Error: context file not found: $CTX_FILE"
        exit 1
    fi
    CTX_ARGS=(--ctx-file "$CTX_FILE")
fi

if [ -n "${THREADS:-}" ]; then
    THREAD_COUNT="$THREADS"
else
    THREAD_COUNT=$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 1)
fi

if [ -n "$THREAD_COUNT" ]; then
    THREAD_ARGS+=("-t" "$THREAD_COUNT")
fi

if [ ! -f "$ENCODER" ] || [ ! -f "$DECODER" ]; then
    make -C "$SCRIPT_DIR" -j4 > /dev/null 2>&1
fi

if [ ! -x "$ENCODER" ]; then
    echo "Error: $ENCODER not found or not executable. Compile with: make"
    exit 1
fi

if [ ! -x "$DECODER" ]; then
    echo "Error: $DECODER not found or not executable. Compile with: make"
    exit 1
fi

if [ "${SKIP_REBUILD:-0}" != "1" ]; then
    make -C "$SCRIPT_DIR" clean > /dev/null 2>&1
    make -C "$SCRIPT_DIR" -j4 > /dev/null 2>&1
fi

total_compressed_bytes=0
total_pixels=0
total_comp_time=0
total_decomp_time=0
total_memory=0
num_files=0
num_validation_errors=0

if [ -n "$CTX_FILE" ]; then
context_line=$(
python3 - "$CTX_FILE" "$THREAD_COUNT" <<'PY'
import re, sys
path = sys.argv[1]
threads = sys.argv[2]
mode = 'shared'
symbol_bits = 8
max_total = None
max_totals = []
alpha_values = []
groups = []

for raw in open(path, 'r', encoding='utf-8'):
    line = raw.split('#', 1)[0].strip()
    if not line:
        continue
    m = re.match(r'^(byte-model|byte_model|bytemodel)\s*[:=]\s*(\w+)$', line, re.I)
    if m:
        mode = m.group(2).lower()
        continue
    m = re.match(r'^(symbol-bits|symbol_bits|symbolbits|sym-bits|sym_bits|symbits)\s*[:=]\s*(\d+)$', line, re.I)
    if m:
        symbol_bits = int(m.group(2))
        continue
    m = re.match(r'^(max-total|max_total|maxtotal)\s*[:=]\s*(\d+)$', line, re.I)
    if m:
        max_totals.append(m.group(2))
        continue
    m = re.match(r'^alpha\s*[:=]\s*([0-9]+(?:\s*/\s*[0-9]+)?)$', line, re.I)
    if m:
        alpha_values.append(m.group(1).replace(' ', ''))
        continue
    m = re.match(r'^(context|contexts|offsets|coords)\s*[:=]\s*(.*)$', line, re.I)
    if m:
        line = m.group(2)
        coords = re.findall(r'\((-?\d+)\s*,\s*(-?\d+)\)', line)
        groups.append(coords)
        continue

n_lines = len(groups)
if n_lines <= 0:
    print('Context: invalid (no coordinates in context file)')
    sys.exit(0)

if n_lines == 1:
    ctx_text = ','.join(f'({x},{y})' for x, y in groups[0]) if groups[0] else 'none'
else:
    phase_ctx = []
    for i, g in enumerate(groups):
        phase_ctx.append(f'phase{i}=' + (','.join(f'({x},{y})' for x, y in g) if g else 'none'))
    ctx_text = ' | '.join(phase_ctx)

parts = []
if mode:
    parts.append(f'byte-model={mode}')
if symbol_bits is not None:
    parts.append(f'symbol-bits={symbol_bits}')
if len(max_totals) == 1:
    parts.append(f'max-total={max_totals[0]}')
elif len(max_totals) > 1:
    parts.append('max-totals=' + ','.join(max_totals))
if len(alpha_values) == 1:
    parts.append(f'alpha={alpha_values[0]}')
elif len(alpha_values) > 1:
    parts.append('alphas=' + ','.join(alpha_values))
if ctx_text:
    parts.append(f'context={ctx_text}')

# Add threads to parts
parts.append(f'threads={threads}')

print(' | '.join(parts))
PY
)
else
# Fallback for hardcoded context
threads="${THREADS:-$THREAD_COUNT}"
context_line=$(
python3 - "$SCRIPT_DIR/src/encoder.cpp" "$threads" <<'PY'
import sys, re
try:
    threads = sys.argv[2]
    with open(sys.argv[1], 'r') as f:
        text = f.read()

    mode = 'shared'
    m_mode = re.search(r'mode_value\s*=\s*ByteModelMode::(Shared|Split);', text, re.I)
    if m_mode: mode = m_mode.group(1).lower()

    sb = 8
    m_sb = re.search(r'symbol_bits_value\s*=\s*(\d+);', text)
    if m_sb: sb = int(m_sb.group(1))

    max_totals = []
    m_mt = re.search(r'max_total_values\s*=\s*\{([^}]+)\};', text)
    if m_mt: max_totals = [x.strip() for x in m_mt.group(1).split(',')]

    alphas = []
    m_alpha = re.search(r'alpha_values\s*=\s*\{([\s\S]*?)\}\s*;', text)
    if m_alpha:
        pairs = re.findall(r'\{\s*(\d+)\s*,\s*(\d+)\s*\}', m_alpha.group(1))
        alphas = [f"{n}/{d}" for n,d in pairs]

    m = re.search(r'load_hardcoded_context_config[\s\S]*?offset_groups\s*=\s*\{([\s\S]*?)\}\s*;', text)
    ctx_text = "default"
    if m:
        inner = m.group(1)
        phases = [p.strip() for p in inner.split('\n') if p.strip()]
        out = []
        phase_idx = 0
        for p in phases:
            pairs = re.findall(r'\{\s*(-?\d+)\s*,\s*(-?\d+)\s*\}', p)
            if pairs:
                out.append(f"phase{phase_idx}:" + ",".join(f"({x},{y})" for x,y in pairs))
                phase_idx += 1
        if out:
            ctx_text = " | ".join(out)

    parts = []
    parts.append(f'byte-model={mode}')
    parts.append(f'symbol-bits={sb}')
    if len(max_totals) == 1: parts.append(f'max-total={max_totals[0]}')
    elif len(max_totals) > 1: parts.append('max-totals=' + ','.join(max_totals))
    if len(alphas) == 1: parts.append(f'alpha={alphas[0]}')
    elif len(alphas) > 1: parts.append('alphas=' + ','.join(alphas))
    parts.append(f'context={ctx_text}')
    parts.append(f'threads={threads}')

    print('[HARDCODED] ' + ' | '.join(parts))
except Exception:
    threads_str = sys.argv[2] if len(sys.argv) > 2 else "auto"
    print(f"[HARDCODED] context=default | threads={threads_str}")
PY
)
fi

echo "$context_line"
echo -e "Bytes\tBPS\tComp_Time (s)\tDecomp_Time (s)\tMemory (Kb)\tValid\tFilename"

for input_file in "${INPUT_FILES[@]}"; do
    [ -f "$input_file" ] || continue

    filename=$(basename "$input_file")
    base="${filename%.raw}"
    output_file="$OUTPUT_DIR/${base}.crxmt"
    decoded_file="$OUTPUT_DIR/${base}.decoded.raw"

    enc_err_file=$(mktemp)
    enc_stats=$("$ENCODER" "$input_file" "$output_file" "${CTX_ARGS[@]}" "${THREAD_ARGS[@]}" 2> "$enc_err_file")
    enc_status=$?

    if [ $enc_status -ne 0 ] || [ ! -f "$output_file" ]; then
        rm -f "$enc_err_file"
        echo "ERROR: compression failed for $filename" >&2
        ((num_validation_errors++))
        continue
    fi

    eff_line=$(grep -m1 '^eficiencia-entropica:' "$enc_err_file" || true)
    rm -f "$enc_err_file"

    IFS=$'\t' read -r compressed_size bps comp_time memory num_pixels <<< "$enc_stats"

    dec_stats=$("$DECODER" "$output_file" "$decoded_file" -c "${THREAD_ARGS[@]}")
    dec_status=$?
    if [ $dec_status -ne 0 ]; then
        decomp_time="0"
        validation="FAIL"
    else
        IFS=$'\t' read -r decomp_time validation <<< "$dec_stats"
    fi

    if [ "$validation" != "OK" ]; then
        ((num_validation_errors++))
    fi

    if [ -n "$eff_line" ]; then
        echo "$eff_line"
    fi
    echo -e "$compressed_size\t$bps\t${comp_time}s\t${decomp_time}s\t${memory}KB\t$validation\t$(basename "$output_file")"

    total_compressed_bytes=$((total_compressed_bytes + compressed_size))
    total_pixels=$((total_pixels + num_pixels))
    total_comp_time=$(echo "$total_comp_time + $comp_time" | bc -l)
    total_decomp_time=$(echo "$total_decomp_time + $decomp_time" | bc -l)
    total_memory=$((total_memory + memory))
    ((num_files++))
done

if [ $total_pixels -gt 0 ]; then
    avg_bps=$(echo "scale=6; ($total_compressed_bytes * 8) / $total_pixels" | bc -l)
else
    avg_bps=0
fi
if [ $num_files -gt 0 ]; then
    avg_comp_time=$(echo "scale=3; $total_comp_time / $num_files" | bc -l)
    avg_decomp_time=$(echo "scale=3; $total_decomp_time / $num_files" | bc -l)
    avg_memory=$(echo "$total_memory / $num_files" | bc | tr -d '[:space:]')
else
    avg_comp_time=0
    avg_decomp_time=0
    avg_memory=0
fi
echo ""
echo "BPS: $avg_bps | Comp_Time: ${avg_comp_time}s | Decomp_Time: ${avg_decomp_time}s | Memory: ${avg_memory} Kb"
if [ $num_validation_errors -gt 0 ]; then
    echo "WARNING: $num_validation_errors validation errors!" >&2
    exit 1
fi
