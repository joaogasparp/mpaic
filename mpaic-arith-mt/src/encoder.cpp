#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <set>
#include <cctype>
#include <new>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <filesystem>
#include <sys/resource.h>
#include <thread>
#include <future>
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

const char* byte_mode_name(ByteModelMode mode) {
    return mode == ByteModelMode::Shared ? "shared" : "split";
}

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

bool parse_mode_directive(const std::string& line, ByteModelMode& mode) {
    std::string s = line;
    for (char& ch : s) {
        if (ch == '=' || ch == ':') {
            ch = ' ';
        }
    }

    std::stringstream ss(s);
    std::string key;
    std::string value;
    if (!(ss >> key >> value)) {
        return false;
    }

    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    if (key != "byte-model" && key != "byte_model" && key != "bytemodel") {
        return false;
    }
    return parse_byte_mode(value, mode);
}

bool parse_symbol_bits_value(int value, int& symbol_bits) {
    if (value != 4 && value != 8) {
        return false;
    }
    symbol_bits = value;
    return true;
}

bool parse_max_total_directive(const std::string& line, uint32_t& max_total) {
    std::string s = line;
    for (char& ch : s) {
        if (ch == '=' || ch == ':') ch = ' ';
    }
    std::stringstream ss(s);
    std::string key;
    uint32_t value = 0;
    if (!(ss >> key >> value)) return false;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    if (key != "max-total" && key != "max_total" && key != "maxtotal") return false;
    if (value < 2 || value > 65535) return false;
    max_total = value;
    return true;
}

bool parse_alpha_part(const std::string& token, uint16_t& value) {
    if (token.empty()) {
        return false;
    }
    std::stringstream ss(token);
    unsigned int temp = 0;
    char extra = 0;
    if (!(ss >> temp) || (ss >> extra)) {
        return false;
    }
    if (temp == 0 || temp > 65535) {
        return false;
    }
    value = static_cast<uint16_t>(temp);
    return true;
}

bool parse_alpha_directive(const std::string& line, uint16_t& alpha_num, uint16_t& alpha_den) {
    std::string s = line;
    for (char& ch : s) {
        if (ch == '=' || ch == ':') ch = ' ';
    }
    std::stringstream ss(s);
    std::string key;
    std::string value;
    if (!(ss >> key >> value)) return false;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    if (key != "alpha") return false;

    std::string num_str = value;
    std::string den_str = "1";
    size_t slash = value.find('/');
    if (slash != std::string::npos) {
        num_str = value.substr(0, slash);
        den_str = value.substr(slash + 1);
    }

    uint16_t num = 0;
    uint16_t den = 0;
    if (!parse_alpha_part(num_str, num) || !parse_alpha_part(den_str, den)) {
        return false;
    }
    alpha_num = num;
    alpha_den = den;
    return true;
}

bool parse_symbol_bits_directive(const std::string& line, int& symbol_bits) {
    std::string s = line;
    for (char& ch : s) {
        if (ch == '=' || ch == ':') {
            ch = ' ';
        }
    }

    std::stringstream ss(s);
    std::string key;
    int value = 0;
    if (!(ss >> key >> value)) {
        return false;
    }

    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    if (key != "symbol-bits" && key != "symbol_bits" && key != "symbolbits" &&
        key != "sym-bits" && key != "sym_bits" && key != "symbits") {
        return false;
    }
    return parse_symbol_bits_value(value, symbol_bits);
}

bool parse_context_list_directive(const std::string& line, std::vector<ContextOffset>& parsed,
                                  bool& matched, std::string& err) {
    matched = false;
    parsed.clear();

    size_t sep_pos = line.find('=');
    if (sep_pos == std::string::npos) {
        sep_pos = line.find(':');
    }
    if (sep_pos == std::string::npos) {
        return false;
    }

    std::string key = trim_copy(line.substr(0, sep_pos));
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    if (key != "context" && key != "contexts" && key != "offsets" && key != "coords") {
        return false;
    }

    matched = true;
    std::string rhs = trim_copy(line.substr(sep_pos + 1));
    if (rhs.empty()) {
        return true;
    }

    size_t pos = 0;
    while (pos < rhs.size()) {
        while (pos < rhs.size() && (std::isspace(static_cast<unsigned char>(rhs[pos])) || rhs[pos] == ',')) {
            pos++;
        }
        if (pos >= rhs.size()) break;

        int dx = 0;
        int dy = 0;
        int consumed = 0;
        if (std::sscanf(rhs.c_str() + pos, " ( %d , %d ) %n", &dx, &dy, &consumed) != 2 || consumed <= 0) {
            err = "invalid context tuple in directive";
            return false;
        }
        parsed.push_back({dx, dy});
        pos += static_cast<size_t>(consumed);
    }

    return true;
}

bool parse_context_line(const std::string& line, std::vector<ContextOffset>& out) {
    out.clear();
    std::string s = line;
    size_t comment_pos = s.find('#');
    if (comment_pos != std::string::npos) {
        s = s.substr(0, comment_pos);
    }

    size_t pos = 0;
    while (pos < s.size()) {
        while (pos < s.size() && (std::isspace(static_cast<unsigned char>(s[pos])) || s[pos] == ',' || s[pos] == ';')) {
            pos++;
        }
        if (pos >= s.size()) break;

        int dx = 0;
        int dy = 0;
        int consumed = 0;
        if (std::sscanf(s.c_str() + pos, " ( %d , %d ) %n", &dx, &dy, &consumed) != 2 || consumed <= 0) {
            return false;
        }
        out.push_back({dx, dy});
        pos += static_cast<size_t>(consumed);
    }

    return true;
}

bool is_causal_offset(const ContextOffset& off) {
    return (off.dy < 0) || (off.dy == 0 && off.dx < 0);
}

void load_hardcoded_context_config(std::vector<std::vector<ContextOffset>>& offset_groups,
                                   ByteModelMode& mode_value,
                                   int& symbol_bits_value,
                                   std::vector<uint32_t>& max_total_values,
                                   std::vector<std::pair<uint16_t, uint16_t>>& alpha_values) {
    mode_value = ByteModelMode::Shared;
    symbol_bits_value = 4;
    max_total_values = {8191, 8191, 2047, 511};
    alpha_values = {{1, 20}, {1, 2}, {1, 2}, {1, 1}};
    offset_groups = {
        {{-4, 0}, {8, -1}, {0, -1}, {0, -2}, {0, -4}},
        {{-1, 0}, {-4, 0}, {-5, 0}, {0, -1}, {-1, -1}},
        {{-1, 0}, {-4, 0}, {-5, 0}, {-9, 0}, {11, -1}, {-5, -1}},
        {{-1, 0}, {-20, 0}},
    };
}

bool load_context_file(const std::string& path, std::vector<std::vector<ContextOffset>>& offset_groups,
                       bool& mode_found, ByteModelMode& mode_value,
                       bool& symbol_bits_found, int& symbol_bits_value,
                       std::vector<uint32_t>& max_total_values,
                       std::vector<std::pair<uint16_t, uint16_t>>& alpha_values,
                       std::string& err) {
    std::ifstream in(path);
    if (!in) {
        err = "could not open context file: " + path;
        return false;
    }

    offset_groups.clear();
    max_total_values.clear();
    alpha_values.clear();
    mode_found = false;
    symbol_bits_found = false;
    std::string line;
    int line_no = 0;

    while (std::getline(in, line)) {
        line_no++;

        size_t comment_pos = line.find('#');
        std::string core = (comment_pos == std::string::npos) ? line : line.substr(0, comment_pos);
        core = trim_copy(core);
        if (core.empty()) {
            continue;
        }

        ByteModelMode parsed_mode;
        if (parse_mode_directive(core, parsed_mode)) {
            mode_found = true;
            mode_value = parsed_mode;
            continue;
        }

        uint32_t parsed_max_total = 0;
        if (parse_max_total_directive(core, parsed_max_total)) {
            max_total_values.push_back(parsed_max_total);
            continue;
        }

        uint16_t parsed_alpha_num = 0;
        uint16_t parsed_alpha_den = 0;
        if (parse_alpha_directive(core, parsed_alpha_num, parsed_alpha_den)) {
            alpha_values.emplace_back(parsed_alpha_num, parsed_alpha_den);
            continue;
        }

        int parsed_symbol_bits = 0;
        if (parse_symbol_bits_directive(core, parsed_symbol_bits)) {
            symbol_bits_found = true;
            symbol_bits_value = parsed_symbol_bits;
            continue;
        }

        bool context_directive_matched = false;
        std::vector<ContextOffset> context_group;
        std::string context_err;
        if (parse_context_list_directive(core, context_group, context_directive_matched, context_err)) {
            std::set<std::pair<int, int>> seen_group;
            for (const auto& off : context_group) {
                if (!is_causal_offset(off)) {
                    err = "non-causal offset on line " + std::to_string(line_no) +
                          ": (" + std::to_string(off.dx) + "," + std::to_string(off.dy) + ")";
                    return false;
                }
                auto key = std::make_pair(off.dx, off.dy);
                if (!seen_group.insert(key).second) {
                    err = "duplicate offset on line " + std::to_string(line_no);
                    return false;
                }
            }
            offset_groups.push_back(context_group);
            continue;
        }
        if (context_directive_matched) {
            err = "invalid context directive on line " + std::to_string(line_no) + ": " + context_err;
            return false;
        }

        if (!parse_context_line(core, context_group)) {
            err = "invalid context line " + std::to_string(line_no) + ": " + line;
            return false;
        }

        std::set<std::pair<int, int>> seen_group;
        for (const auto& off : context_group) {
            if (!is_causal_offset(off)) {
                err = "non-causal offset on line " + std::to_string(line_no) +
                      ": (" + std::to_string(off.dx) + "," + std::to_string(off.dy) + ")";
                return false;
            }
            auto key = std::make_pair(off.dx, off.dy);
            if (!seen_group.insert(key).second) {
                err = "duplicate offset on line " + std::to_string(line_no);
                return false;
            }
        }
        offset_groups.push_back(context_group);
    }

    if (offset_groups.empty()) {
        err = "context file is empty";
        return false;
    }

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
        err = "context file must have 1 max-total value (global) or " +
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
        err = "context file must have 1 alpha value (global) or " +
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

    err = "context file must have 1 context line (global) or " +
          std::to_string(symbols_per_sample) + " lines (per phase)";
    return false;
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " <input_image> [<output_file>] [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -r, --rows <value>       Number of rows (height) [REQUIRED for RAW]\n";
    std::cout << "  -c, --cols <value>       Number of columns (width) [REQUIRED for RAW]\n";
    std::cout << "  -b, --bpp <8|16>         Bits per pixel (default: 8)\n";
    std::cout << "  -e, --endian <le|be>     Endianness for 16-bit (default: le)\n";
    std::cout << "  -f, --ctx-file <path>    Context offsets file\n";
    std::cout << "  -t, --threads <num>      Number of threads (default: hardware concurrency)\n";
    std::cout << "  -v, --verbose            Verbose output\n";
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

std::string default_output_path(const std::string& input_file) {
    std::string base = strip_extension(input_file);
    size_t pos = base.find("astronomical_images");
    if (pos != std::string::npos) {
        base = "compressed_images" + base.substr(pos + 19);
    } else {
        size_t slash = base.rfind('/');
        if (slash != std::string::npos) {
            base = "compressed_images/" + base.substr(slash + 1);
        } else {
            base = "compressed_images/" + base;
        }
    }
    return base + ".caxmt";
}

struct BandResult {
    std::vector<uint8_t> encoded;
    double ideal_bits = 0.0;
};

void encode_band(uint32_t row_start, uint32_t row_end, uint32_t width, uint32_t height,
                 const uint16_t* pixels,
                 int symbols_per_sample, int symbol_bits, uint32_t num_symbols,
                 ByteModelMode byte_model_mode,
                 const std::vector<std::vector<ContextOffset>>& phase_offsets,
                 const std::vector<uint32_t>& phase_max_totals,
                 const std::vector<uint16_t>& phase_alpha_nums,
                 const std::vector<uint16_t>& phase_alpha_dens,
                 const std::vector<uint32_t>& phase_symbol_shifts,
                 const std::vector<uint32_t>& phase_contexts,
                 const std::vector<uint64_t>& phase_context_base,
                 uint64_t total_contexts, bool shared_large_context_space,
                 bool report_entropy, BandResult& result) {
    
    ArithmeticEncoder encoder(result.encoded);
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
                uint16_t pixel = pixels[row_base + col];
                for (int s = 0; s < symbols_per_sample; s++) {
                    uint8_t sym = static_cast<uint8_t>((pixel >> phase_symbol_shifts[s]) & (num_symbols - 1u));
                    get_neighbors(pixels, row, col, width, height, phase_offsets[s], neighbors.data(), row_start, row_end);
                    uint32_t ctx_val = compute_context(neighbors.data(), static_cast<int>(phase_offsets[s].size()), s, symbol_bits, symbols_per_sample);
                    AdaptiveModel& model = ctx_models[s][ctx_val];

                    uint32_t cum_lo = 0, cum_hi = 0;
                    model.cumulative_interval(sym, cum_lo, cum_hi);
                    if (report_entropy) {
                        uint64_t freq = model.symbol_freq(sym);
                        if (freq > 0 && model.total > 0) {
                            double p_sym = static_cast<double>(freq) / static_cast<double>(model.total);
                            result.ideal_bits += -std::log2(p_sym);
                        }
                    }
                    encoder.encode_interval(cum_lo, cum_hi, model.total);
                    model.update(sym);
                }
            }
        }
    } else {
        ContextModel& model_store = ctx_models[0];
        for (uint32_t row = row_start; row < row_end; row++) {
            size_t row_base = static_cast<size_t>(row) * width;
            for (uint32_t col = 0; col < width; col++) {
                uint16_t pixel = pixels[row_base + col];
                for (int s = 0; s < symbols_per_sample; s++) {
                    uint8_t sym = static_cast<uint8_t>((pixel >> phase_symbol_shifts[s]) & (num_symbols - 1u));
                    uint32_t base_ctx = compute_context_shared(pixels, row, col, width, height, s, symbols_per_sample, phase_offsets[s], symbol_bits, row_start, row_end);
                    uint64_t ctx_val = shared_large_context_space
                        ? ((static_cast<uint64_t>(static_cast<uint32_t>(s)) << 32) | static_cast<uint64_t>(base_ctx))
                        : (phase_context_base[s] + static_cast<uint64_t>(base_ctx));
                    AdaptiveModel& model = model_store.get_or_create(ctx_val, phase_max_totals[s], phase_alpha_nums[s], phase_alpha_dens[s]);

                    uint32_t cum_lo = 0, cum_hi = 0;
                    model.cumulative_interval(sym, cum_lo, cum_hi);
                    if (report_entropy) {
                        uint64_t freq = model.symbol_freq(sym);
                        if (freq > 0 && model.total > 0) {
                            double p_sym = static_cast<double>(freq) / static_cast<double>(model.total);
                            result.ideal_bits += -std::log2(p_sym);
                        }
                    }
                    encoder.encode_interval(cum_lo, cum_hi, model.total);
                    model.update(sym);
                }
            }
        }
    }
    encoder.finish();
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
    std::string context_file;

    int rows = 0;
    int cols = 0;
    int bpp = 8;
    bool big_endian = false;
    int symbol_bits = 8;
    uint32_t max_total = 16384;
    uint16_t alpha_num = 1;
    uint16_t alpha_den = 1;
    ByteModelMode byte_model_mode = ByteModelMode::Shared;
    bool verbose = false;
    int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 1;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if ((arg == "-r" || arg == "--rows") && i + 1 < argc) {
            rows = std::stoi(argv[++i]);
        } else if ((arg == "-c" || arg == "--cols") && i + 1 < argc) {
            cols = std::stoi(argv[++i]);
        } else if ((arg == "-b" || arg == "--bpp") && i + 1 < argc) {
            bpp = std::stoi(argv[++i]);
            if (bpp != 8 && bpp != 16) {
                std::cerr << "Error: bpp must be 8 or 16\n";
                return 1;
            }
        } else if ((arg == "-e" || arg == "--endian") && i + 1 < argc) {
            std::string endian = argv[++i];
            std::transform(endian.begin(), endian.end(), endian.begin(), ::tolower);
            if (endian == "be" || endian == "big") {
                big_endian = true;
            } else if (endian == "le" || endian == "little") {
                big_endian = false;
            } else {
                std::cerr << "Error: endian must be 'le' or 'be'\n";
                return 1;
            }
        } else if ((arg == "-f" || arg == "--ctx-file") && i + 1 < argc) {
            context_file = argv[++i];
        } else if ((arg == "-t" || arg == "--threads") && i + 1 < argc) {
            num_threads = std::stoi(argv[++i]);
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
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
        std::cerr << "Error: input file is required\n";
        print_usage(argv[0]);
        return 1;
    }

    if (output_file.empty()) {
        output_file = default_output_path(input_file);
    }

    std::vector<std::vector<ContextOffset>> offset_groups;
    std::vector<uint32_t> max_total_values;
    std::vector<std::pair<uint16_t, uint16_t>> alpha_values;
    if (!context_file.empty()) {
        std::string err;
        bool mode_from_file_found = false;
        ByteModelMode mode_from_file = byte_model_mode;
        bool symbol_bits_from_file_found = false;
        int symbol_bits_from_file = symbol_bits;
        if (!load_context_file(context_file, offset_groups, mode_from_file_found, mode_from_file,
                               symbol_bits_from_file_found, symbol_bits_from_file,
                               max_total_values, alpha_values,
                               err)) {
            std::cerr << "Error: " << err << "\n";
            return 1;
        }
        if (mode_from_file_found) {
            byte_model_mode = mode_from_file;
        }
        if (symbol_bits_from_file_found) {
            symbol_bits = symbol_bits_from_file;
        }
    } else {
        ByteModelMode hardcoded_mode = ByteModelMode::Shared;
        int hardcoded_symbol_bits = 4;
        load_hardcoded_context_config(offset_groups, hardcoded_mode, hardcoded_symbol_bits,
                                      max_total_values, alpha_values);
        byte_model_mode = hardcoded_mode;
        symbol_bits = hardcoded_symbol_bits;
    }

    MappedU16 pixels;
    ImageInfo info;

    bool success = false;
    uint32_t auto_w, auto_h, auto_ch;
    int auto_bits;
    bool auto_be;
    if (rows <= 0 || cols <= 0) {
        if (parse_raw_geometry(input_file, auto_w, auto_h, auto_ch, auto_bits, auto_be)) {
            cols = auto_w;
            rows = auto_h;
            bpp = auto_bits;
            big_endian = auto_be;
        } else {
            std::cerr << "Error: could not detect geometry from filename. Use -r and -c\n";
            print_usage(argv[0]);
            return 1;
        }
    }
    PixelFormat in_format = bpp == 8 ? PixelFormat::UINT8 :
                            (big_endian ? PixelFormat::UINT16_BE : PixelFormat::UINT16_LE);
    std::string tmp_hint;
    try {
        std::filesystem::path op(output_file);
        if (op.has_parent_path()) tmp_hint = op.parent_path().string();
    } catch (...) {}
    success = map_raw_native(input_file, cols, rows, 1, in_format, tmp_hint, pixels);
    if (success) {
        info.width = cols;
        info.height = rows;
        info.channels = 1;
        info.format = in_format;
        info.max_value = (bpp == 8) ? 255 : 65535;
    }

    if (!success) {
        std::cerr << "Error: failed to read image\n";
        return 1;
    }

    bool is_8bit = (info.format == PixelFormat::UINT8);
    int sample_bits = is_8bit ? 8 : 16;
    int symbols_per_sample = sample_bits / symbol_bits;
    uint32_t num_symbols = 1u << symbol_bits;

    std::vector<std::vector<ContextOffset>> phase_offsets;
    {
        std::string err;
        if (!resolve_phase_offsets(offset_groups, symbols_per_sample, phase_offsets, err)) {
            std::cerr << "Error: " << err << "\n";
            return 1;
        }
    }

    std::vector<uint32_t> phase_nctx(static_cast<size_t>(symbols_per_sample), 0);
    std::vector<uint32_t> phase_contexts(static_cast<size_t>(symbols_per_sample), 0);
    std::vector<uint32_t> phase_symbol_shifts(static_cast<size_t>(symbols_per_sample), 0);
    for (int s = 0; s < symbols_per_sample; ++s) {
        phase_nctx[s] = static_cast<uint32_t>(phase_offsets[s].size());
        phase_contexts[s] = calc_num_contexts(static_cast<int>(phase_nctx[s]), symbol_bits);
        phase_symbol_shifts[s] = static_cast<uint32_t>(symbol_bits * (symbols_per_sample - 1 - s));
    }

    std::vector<uint32_t> phase_max_totals;
    std::vector<uint16_t> phase_alpha_nums;
    std::vector<uint16_t> phase_alpha_dens;
    {
        std::string err;
        if (!resolve_phase_directive_values(max_total_values, alpha_values, symbols_per_sample,
                                            max_total, alpha_num, alpha_den,
                                            phase_max_totals, phase_alpha_nums, phase_alpha_dens,
                                            err)) {
            std::cerr << "Error: " << err << "\n";
            return 1;
        }
    }

    std::vector<uint64_t> phase_context_base(static_cast<size_t>(symbols_per_sample), 0);
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

    const char* report_entropy_env = std::getenv("CACX_REPORT_ENTROPY");
    bool report_entropy = (report_entropy_env && report_entropy_env[0] == '1');

    uint32_t width = info.width;
    uint32_t height = info.height;

    if (num_threads > (int)height) num_threads = (int)height;
    int actual_threads = num_threads;
    int band_count = actual_threads;
    if (band_count > (int)height) band_count = (int)height;
    std::vector<BandResult> band_results(static_cast<size_t>(band_count));
    std::vector<std::thread> threads;
    uint32_t rows_per_band = height / static_cast<uint32_t>(band_count);
    int worker_threads = std::min(actual_threads, band_count);

    for (int worker = 0; worker < worker_threads; worker++) {
        threads.emplace_back([&, worker]() {
            for (int band = worker; band < band_count; band += worker_threads) {
                uint32_t start_row = static_cast<uint32_t>(band) * rows_per_band;
                uint32_t end_row = (band == band_count - 1) ? height : static_cast<uint32_t>(band + 1) * rows_per_band;
                encode_band(start_row, end_row, width, height, pixels.data(),
                            symbols_per_sample, symbol_bits, num_symbols, byte_model_mode,
                            std::ref(phase_offsets), std::ref(phase_max_totals),
                            std::ref(phase_alpha_nums), std::ref(phase_alpha_dens),
                            std::ref(phase_symbol_shifts), std::ref(phase_contexts),
                            std::ref(phase_context_base), total_contexts, shared_large_context_space,
                            report_entropy, std::ref(band_results[static_cast<size_t>(band)]));
            }
        });
    }

    for (auto& worker : threads) {
        worker.join();
    }
    double total_ideal_bits = 0;
    for (int band = 0; band < band_count; band++) {
        total_ideal_bits += band_results[static_cast<size_t>(band)].ideal_bits;
    }

    std::ofstream out(output_file, std::ios::binary);
    out.write("CAXM", 4);
    uint8_t version = 1;
    out.write(reinterpret_cast<char*>(&version), 1);

    uint32_t w = info.width, h = info.height;
    out.write(reinterpret_cast<char*>(&w), 4);
    out.write(reinterpret_cast<char*>(&h), 4);

    uint8_t ch = info.channels;
    uint8_t bpp_out = is_8bit ? 8 : 16;
    uint8_t fmt = static_cast<uint8_t>(info.format);
    out.write(reinterpret_cast<char*>(&ch), 1);
    out.write(reinterpret_cast<char*>(&bpp_out), 1);
    out.write(reinterpret_cast<char*>(&fmt), 1);

    uint16_t max_val = info.max_value;
    out.write(reinterpret_cast<char*>(&max_val), 2);

    uint32_t image_hash = crc32(pixels.data(), pixels.size());
    out.write(reinterpret_cast<char*>(&image_hash), 4);

    uint8_t n_ctx_out = static_cast<uint8_t>(phase_nctx[0]);
    uint8_t sb_out = static_cast<uint8_t>(symbol_bits);
    uint8_t mode_out = static_cast<uint8_t>(byte_model_mode);
    out.write(reinterpret_cast<char*>(&n_ctx_out), 1);
    out.write(reinterpret_cast<char*>(&sb_out), 1);
    out.write(reinterpret_cast<char*>(&mode_out), 1);

    uint8_t symbols_per_sample_out = static_cast<uint8_t>(symbols_per_sample);
    out.write(reinterpret_cast<char*>(&symbols_per_sample_out), 1);
    for (int s = 0; s < symbols_per_sample; ++s) {
        uint8_t noff = static_cast<uint8_t>(phase_offsets[s].size());
        out.write(reinterpret_cast<char*>(&noff), 1);
        for (const auto &off : phase_offsets[s]) {
            int16_t dx = static_cast<int16_t>(off.dx);
            int16_t dy = static_cast<int16_t>(off.dy);
            out.write(reinterpret_cast<char*>(&dx), 2);
            out.write(reinterpret_cast<char*>(&dy), 2);
        }
        uint32_t pmax = phase_max_totals[s];
        uint16_t palpha_num = phase_alpha_nums[s];
        uint16_t palpha_den = phase_alpha_dens[s];
        out.write(reinterpret_cast<char*>(&pmax), 4);
        out.write(reinterpret_cast<char*>(&palpha_num), 2);
        out.write(reinterpret_cast<char*>(&palpha_den), 2);
    }

    uint32_t n_bands = static_cast<uint32_t>(band_count);
    out.write(reinterpret_cast<char*>(&n_bands), 4);
    for (int band = 0; band < band_count; band++) {
        uint32_t band_len = static_cast<uint32_t>(band_results[static_cast<size_t>(band)].encoded.size());
        out.write(reinterpret_cast<char*>(&band_len), 4);
    }

    size_t total_data_len = 0;
    for (int band = 0; band < band_count; band++) {
        out.write(reinterpret_cast<char*>(band_results[static_cast<size_t>(band)].encoded.data()),
                  band_results[static_cast<size_t>(band)].encoded.size());
        total_data_len += band_results[static_cast<size_t>(band)].encoded.size();
    }
    out.close();

    size_t compressed_size = std::filesystem::file_size(output_file);
    double bps = (double)compressed_size * 8.0 / pixels.size();
    if (report_entropy) {
        double total_bits = static_cast<double>(total_data_len) * 8.0;
        double diff_bits = std::fabs(total_bits - total_ideal_bits);
        double diff_ratio = (total_bits > 0.0) ? (diff_bits / total_bits) : 0.0;
        fprintf(stderr, "eficiencia-entropica: bits_teoricos=%.2f | bits_praticos=%.0f | diferenca=%.2f (%.6f%%)\n",
                total_ideal_bits, total_bits, diff_bits, diff_ratio * 100.0);
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double comp_time = std::chrono::duration<double>(end_time - start_time).count();

    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    long memory_kb = usage.ru_maxrss;
    printf("%zu\t%.6f\t%.2f\t%ld\t%zu\n", compressed_size, bps, comp_time, memory_kb, pixels.size());

    return 0;
}
