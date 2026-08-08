#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <set>
#include <cctype>
#include <new>
#include <utility>
#include <filesystem>
#include <thread>
#include "image_io.hpp"
#include "arithmetic_codec.hpp"
#include "context_model.hpp"

namespace {

uint32_t crc32(const uint16_t* data, size_t n) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < n; i++) {
        uint16_t val = data[i];
        for (int b = 0; b < 2; b++) {
            uint8_t byte = (b == 0) ? (val & 0xFF) : (val >> 8);
            crc ^= byte;
            for (int j = 0; j < 8; j++) {
                crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
            }
        }
    }
    return ~crc;
}

enum class ByteModelMode : uint8_t {
    Split = 0,
    Shared = 1,
};

bool parse_byte_mode(const std::string& s, ByteModelMode& mode) {
    std::string v = s;
    std::transform(v.begin(), v.end(), v.begin(), ::tolower);
    if (v == "split") {
        mode = ByteModelMode::Split;
        return true;
    }
    if (v == "shared") {
        mode = ByteModelMode::Shared;
        return true;
    }
    return false;
}

std::string trim_copy(const std::string& input) {
    size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
        start++;
    }
    size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        end--;
    }
    return input.substr(start, end - start);
}

bool parse_symbol_bits_value(int value, int& symbol_bits) {
    if (value != 4 && value != 8) {
        return false;
    }
    symbol_bits = value;
    return true;
}

bool resolve_phase_directive_values(const std::vector<uint32_t>& max_total_values,
                                    const std::vector<std::pair<uint16_t, uint16_t>>& alpha_values,
                                    int symbols_per_sample,
                                    uint32_t default_max_total,
                                    uint16_t default_alpha_num,
                                    uint16_t default_alpha_den,
                                    std::vector<uint32_t>& phase_max_totals,
                                    std::vector<uint16_t>& phase_alpha_nums,
                                    std::vector<uint16_t>& phase_alpha_dens,
                                    std::string& err) {
    if (symbols_per_sample <= 0) {
        err = "invalid symbols-per-sample";
        return false;
    }

    auto expand_max_totals = [&]() -> bool {
        if (max_total_values.empty()) {
            phase_max_totals.assign(static_cast<size_t>(symbols_per_sample), default_max_total);
            return true;
        }
        if (max_total_values.size() == 1) {
            phase_max_totals.assign(static_cast<size_t>(symbols_per_sample), max_total_values.front());
            return true;
        }
        if (max_total_values.size() == static_cast<size_t>(symbols_per_sample)) {
            phase_max_totals = max_total_values;
            return true;
        }
        err = "header must have 1 max-total value (global) or " +
              std::to_string(symbols_per_sample) + " values (per phase)";
        return false;
    };

    auto expand_alphas = [&]() -> bool {
        if (alpha_values.empty()) {
            phase_alpha_nums.assign(static_cast<size_t>(symbols_per_sample), default_alpha_num);
            phase_alpha_dens.assign(static_cast<size_t>(symbols_per_sample), default_alpha_den);
            return true;
        }
        if (alpha_values.size() == 1) {
            phase_alpha_nums.assign(static_cast<size_t>(symbols_per_sample), alpha_values.front().first);
            phase_alpha_dens.assign(static_cast<size_t>(symbols_per_sample), alpha_values.front().second);
            return true;
        }
        if (alpha_values.size() == static_cast<size_t>(symbols_per_sample)) {
            phase_alpha_nums.reserve(alpha_values.size());
            phase_alpha_dens.reserve(alpha_values.size());
            for (const auto& value : alpha_values) {
                phase_alpha_nums.push_back(value.first);
                phase_alpha_dens.push_back(value.second);
            }
            return true;
        }
        err = "header must have 1 alpha value (global) or " +
              std::to_string(symbols_per_sample) + " values (per phase)";
        return false;
    };

    return expand_max_totals() && expand_alphas();
}

bool resolve_phase_offsets(const std::vector<std::vector<ContextOffset>>& offset_groups,
                          int symbols_per_sample,
                          std::vector<std::vector<ContextOffset>>& phase_offsets,
                          std::string& err) {
    phase_offsets.clear();
    if (symbols_per_sample <= 0) {
        err = "invalid symbols-per-sample";
        return false;
    }
    if (offset_groups.empty()) {
        err = "context configuration is empty";
        return false;
    }

    if (offset_groups.size() == 1) {
        phase_offsets.assign(static_cast<size_t>(symbols_per_sample), offset_groups[0]);
        return true;
    }

    if (offset_groups.size() == static_cast<size_t>(symbols_per_sample)) {
        phase_offsets = offset_groups;
        return true;
    }

    err = "header must have 1 context line (global) or " +
          std::to_string(symbols_per_sample) + " lines (per phase)";
    return false;
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " <input_file> [<output_file>] [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -c, --compare            Compare decoded image hash against header\n";
    std::cout << "  -t, --threads <num>      Number of threads (default: hardware concurrency)\n";
    std::cout << "  -v, --verbose            Show detailed information\n";
    std::cout << "  -h, --help               Show this help\n";
}

std::string strip_extension(const std::string& path) {
    size_t slash = path.rfind('/');
    size_t dot = path.rfind('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        return path.substr(0, dot);
    }
    return path;
}

std::string default_output_path(const std::string& input_file, const ImageInfo& info) {
    std::string base = strip_extension(input_file);
    size_t pos = base.find("compressed_images");
    if (pos != std::string::npos) {
        base.replace(pos, 17, "decompressed_images");
    } else {
        size_t slash = base.rfind('/');
        if (slash != std::string::npos) {
            base = "decompressed_images/" + base.substr(slash + 1);
        } else {
            base = "decompressed_images/" + base;
        }
    }
    (void)info;
    return base + ".raw";
}

void decompress_band(uint32_t row_start, uint32_t row_end, uint32_t width, uint32_t height,
                     uint16_t* pixels,
                     const std::vector<uint8_t>& band_encoded,
                     int symbols_per_sample, int symbol_bits, uint32_t num_symbols,
                     ByteModelMode byte_model_mode,
                     const std::vector<std::vector<ContextOffset>>& phase_offsets,
                     const std::vector<uint32_t>& phase_max_totals,
                     const std::vector<uint16_t>& phase_alpha_nums,
                     const std::vector<uint16_t>& phase_alpha_dens,
                     const std::vector<uint32_t>& phase_symbol_shifts,
                     const std::vector<uint32_t>& phase_contexts,
                     const std::vector<uint64_t>& phase_context_base,
                     uint64_t total_contexts, bool shared_large_context_space) {
    
    ArithmeticDecoder decoder(band_encoded);
    decoder.start();
    
    int model_count = (byte_model_mode == ByteModelMode::Split) ? symbols_per_sample : 1;
    std::vector<ContextModel> ctx_models(model_count);
    
    if (byte_model_mode == ByteModelMode::Split) {
        for (int s = 0; s < symbols_per_sample; ++s) {
            ctx_models[s].init(phase_contexts[s], num_symbols, phase_max_totals[s],
                               phase_alpha_nums[s], phase_alpha_dens[s]);
        }
    } else {
        ctx_models[0].init(static_cast<uint32_t>(std::min<uint64_t>(total_contexts, UINT32_MAX)),
                           num_symbols, phase_max_totals[0], phase_alpha_nums[0], phase_alpha_dens[0]);
    }

    size_t max_phase_nctx = 0;
    for (int s = 0; s < symbols_per_sample; ++s) {
        max_phase_nctx = std::max(max_phase_nctx, static_cast<size_t>(phase_offsets[s].size()));
    }
    std::vector<uint16_t> neighbors(max_phase_nctx);

    if (byte_model_mode == ByteModelMode::Split) {
        for (uint32_t row = row_start; row < row_end; row++) {
            size_t row_base = static_cast<size_t>(row) * width;
            for (uint32_t col = 0; col < width; col++) {
                size_t idx = row_base + col;
                uint16_t pixel = 0;
                for (int s = 0; s < symbols_per_sample; s++) {
                    get_neighbors(pixels, row, col, width, height, phase_offsets[s], neighbors.data(), row_start, row_end);
                    uint32_t ctx_val = compute_context(neighbors.data(), static_cast<int>(phase_offsets[s].size()), s, symbol_bits, symbols_per_sample);
                    AdaptiveModel& model = ctx_models[s][ctx_val];

                    uint64_t cum = decoder.get_cum_value(model.total);
                    uint32_t cum_lo = 0, cum_hi = 0;
                    uint32_t sym = model.find_symbol_with_interval(cum, cum_lo, cum_hi);
                    decoder.update_interval(cum_lo, cum_hi, model.total);
                    model.update(sym);

                    pixel |= static_cast<uint16_t>(sym) << phase_symbol_shifts[s];
                    pixels[idx] = pixel;
                }
            }
        }
    } else {
        ContextModel& model_store = ctx_models[0];
        for (uint32_t row = row_start; row < row_end; row++) {
            size_t row_base = static_cast<size_t>(row) * width;
            for (uint32_t col = 0; col < width; col++) {
                size_t idx = row_base + col;
                uint16_t pixel = 0;
                for (int s = 0; s < symbols_per_sample; s++) {
                    uint32_t base_ctx = compute_context_shared(pixels, row, col, width, height, s, symbols_per_sample, phase_offsets[s], symbol_bits, row_start, row_end);
                    uint64_t ctx_val = shared_large_context_space
                        ? ((static_cast<uint64_t>(static_cast<uint32_t>(s)) << 32) | static_cast<uint64_t>(base_ctx))
                        : (phase_context_base[s] + static_cast<uint64_t>(base_ctx));
                    AdaptiveModel& model = model_store.get_or_create(ctx_val, phase_max_totals[s], phase_alpha_nums[s], phase_alpha_dens[s]);

                    uint64_t cum = decoder.get_cum_value(model.total);
                    uint32_t cum_lo = 0, cum_hi = 0;
                    uint32_t sym = model.find_symbol_with_interval(cum, cum_lo, cum_hi);
                    decoder.update_interval(cum_lo, cum_hi, model.total);
                    model.update(sym);

                    pixel |= static_cast<uint16_t>(sym) << phase_symbol_shifts[s];
                    pixels[idx] = pixel;
                }
            }
        }
    }
}

}  

int main(int argc, char* argv[]) {
    auto start_time = std::chrono::high_resolution_clock::now();

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string input_file;
    std::string output_file;
    bool do_verify = false;
    bool verbose = false;
    int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 1;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--compare" || arg == "-c") {
            do_verify = true;
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if ((arg == "-t" || arg == "--threads") && i + 1 < argc) {
            num_threads = std::stoi(argv[++i]);
        } else if (arg[0] != '-' && input_file.empty()) {
            input_file = arg;
        } else if (arg[0] != '-' && output_file.empty()) {
            output_file = arg;
        } else if (arg[0] == '-') {
            std::cerr << "Error: unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (input_file.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    std::ifstream in(input_file, std::ios::binary);
    if (!in) {
        std::cerr << "Error: could not open file\n";
        return 1;
    }

    char magic[5] = {0};
    in.read(magic, 4);
    if (std::string(magic) != "CAXM") {
        std::cerr << "Format invalid (expected CAXM)\n";
        return 1;
    }

    uint8_t version;
    in.read(reinterpret_cast<char*>(&version), 1);
    if (version != 1) {
        std::cerr << "Error: unsupported stream version (expected 1)\n";
        return 1;
    }

    ImageInfo info;
    in.read(reinterpret_cast<char*>(&info.width), 4);
    in.read(reinterpret_cast<char*>(&info.height), 4);

    uint8_t ch, bpp, fmt;
    in.read(reinterpret_cast<char*>(&ch), 1);
    in.read(reinterpret_cast<char*>(&bpp), 1);
    in.read(reinterpret_cast<char*>(&fmt), 1);
    info.channels = ch;
    info.format = static_cast<PixelFormat>(fmt);

    in.read(reinterpret_cast<char*>(&info.max_value), 2);

    uint32_t header_hash = 0;
    in.read(reinterpret_cast<char*>(&header_hash), 4);

    uint8_t n_ctx_in = 3, symbol_bits_in = 8;
    in.read(reinterpret_cast<char*>(&n_ctx_in), 1);
    in.read(reinterpret_cast<char*>(&symbol_bits_in), 1);
    int symbol_bits = symbol_bits_in;
    
    ByteModelMode byte_model_mode;
    uint8_t mode_in = 0;
    in.read(reinterpret_cast<char*>(&mode_in), 1);
    byte_model_mode = static_cast<ByteModelMode>(mode_in);

    uint8_t symbols_per_sample_in = 0;
    in.read(reinterpret_cast<char*>(&symbols_per_sample_in), 1);
    int symbols_per_sample = symbols_per_sample_in;

    std::vector<std::vector<ContextOffset>> offset_groups(symbols_per_sample);
    std::vector<uint32_t> max_total_values(symbols_per_sample);
    std::vector<std::pair<uint16_t, uint16_t>> alpha_values(symbols_per_sample);

    for (int s = 0; s < symbols_per_sample; s++) {
        uint8_t noff = 0;
        in.read(reinterpret_cast<char*>(&noff), 1);
        for (int i = 0; i < noff; i++) {
            int16_t dx, dy;
            in.read(reinterpret_cast<char*>(&dx), 2);
            in.read(reinterpret_cast<char*>(&dy), 2);
            offset_groups[s].push_back({dx, dy});
        }
        in.read(reinterpret_cast<char*>(&max_total_values[s]), 4);
        in.read(reinterpret_cast<char*>(&alpha_values[s].first), 2);
        in.read(reinterpret_cast<char*>(&alpha_values[s].second), 2);
    }

    uint32_t n_bands;
    in.read(reinterpret_cast<char*>(&n_bands), 4);
    std::vector<uint32_t> band_lengths(n_bands);
    for (uint32_t i = 0; i < n_bands; i++) {
        in.read(reinterpret_cast<char*>(&band_lengths[i]), 4);
    }

    std::vector<std::vector<uint8_t>> band_encoded(n_bands);
    for (uint32_t i = 0; i < n_bands; i++) {
        band_encoded[i].resize(band_lengths[i]);
        in.read(reinterpret_cast<char*>(band_encoded[i].data()), band_lengths[i]);
    }
    in.close();

    uint32_t width = info.width;
    uint32_t height = info.height;
    uint32_t num_symbols = 1u << symbol_bits;

    std::vector<std::vector<ContextOffset>> phase_offsets = offset_groups;
    std::vector<uint32_t> phase_max_totals = max_total_values;
    std::vector<uint16_t> phase_alpha_nums(symbols_per_sample);
    std::vector<uint16_t> phase_alpha_dens(symbols_per_sample);
    for (int s = 0; s < symbols_per_sample; s++) {
        phase_alpha_nums[s] = alpha_values[s].first;
        phase_alpha_dens[s] = alpha_values[s].second;
    }

    std::vector<uint32_t> phase_contexts(symbols_per_sample);
    std::vector<uint32_t> phase_symbol_shifts(symbols_per_sample);
    for (int s = 0; s < symbols_per_sample; ++s) {
        phase_contexts[s] = calc_num_contexts(static_cast<int>(phase_offsets[s].size()), symbol_bits);
        phase_symbol_shifts[s] = static_cast<uint32_t>(symbol_bits * (symbols_per_sample - 1 - s));
    }

    std::vector<uint64_t> phase_context_base(symbols_per_sample, 0);
    uint64_t total_contexts = 0;
    bool shared_large_context_space = false;
    if (byte_model_mode == ByteModelMode::Shared) {
        for (int s = 0; s < symbols_per_sample; ++s) {
            if (phase_contexts[s] > (UINT32_MAX - total_contexts)) {
                shared_large_context_space = true;
                break;
            }
            phase_context_base[s] = total_contexts;
            total_contexts += phase_contexts[s];
        }
    } else {
        for (int s = 0; s < symbols_per_sample; ++s) {
            total_contexts += phase_contexts[s];
        }
    }

    MappedU16 pixels;
    {
        std::string tmp_hint;
        try {
            std::filesystem::path op(output_file);
            if (op.has_parent_path()) tmp_hint = op.parent_path().string();
        } catch (...) {}
        if (!map_create_writable(static_cast<size_t>(width) * height, tmp_hint, pixels)) {
            return 1;
        }
    }
    std::vector<std::thread> threads;
    uint32_t rows_per_thread = height / n_bands;
    int actual_threads = std::min(num_threads, static_cast<int>(n_bands));

    for (int t_idx = 0; t_idx < actual_threads; t_idx++) {
        threads.emplace_back([&, t_idx]() {
            for (uint32_t i = t_idx; i < n_bands; i += actual_threads) {
                uint32_t start_row = i * rows_per_thread;
                uint32_t end_row = (i == n_bands - 1) ? height : (i + 1) * rows_per_thread;
                decompress_band(start_row, end_row, width, height, pixels.data(),
                                std::ref(band_encoded[i]), symbols_per_sample, symbol_bits, num_symbols,
                                byte_model_mode, std::ref(phase_offsets), std::ref(phase_max_totals),
                                std::ref(phase_alpha_nums), std::ref(phase_alpha_dens),
                                std::ref(phase_symbol_shifts), std::ref(phase_contexts),
                                std::ref(phase_context_base), total_contexts, shared_large_context_space);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    if (verbose) {
        std::cerr << "threads=" << actual_threads << "\n";
    }

    if (output_file.empty()) {
        output_file = default_output_path(input_file, info);
    }
    write_raw_stream(output_file, pixels.data(), pixels.size(), info);

    auto end_time = std::chrono::high_resolution_clock::now();
    double decomp_time = std::chrono::duration<double>(end_time - start_time).count();

    if (do_verify) {
        std::string validation = "OK";
        uint32_t decoded_hash = crc32(pixels.data(), pixels.size());
        if (decoded_hash != header_hash) validation = "FAIL";
        printf("%.6f\t%s\n", decomp_time, validation.c_str());
    } else {
        printf("%.6f\n", decomp_time);
    }

    return 0;
}
