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

struct PhaseDirectiveValues {
    std::vector<uint32_t> max_totals;
    std::vector<std::pair<uint16_t, uint16_t>> alphas;
};

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

// valida um inteiro positivo em texto (sem lixo extra) e dentro do limite
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

// aceita alpha=n ou alpha=n/d e devolve numerador/denominador
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

    return !out.empty();
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

        // alpha vem do ficheiro em texto; aqui convertemos e validamos n/d
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
    std::cout << "Usage: " << prog << " <input_file> [<output_file>] [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -c, --compare            Compare decoded image hash against header\n";
    std::cout << "  -v, --verbose            Show detailed information (including verify status)\n";
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

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--compare" || arg == "-c") {
            do_verify = true;
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
    if (std::string(magic) != "CAX1") {
        std::cerr << "Format invalid (expected CAX1)\n";
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
    int n_ctx = n_ctx_in;
    int symbol_bits = symbol_bits_in;
    uint32_t max_total = 16384;
    uint16_t alpha_num = 1;
    uint16_t alpha_den = 1;
    ByteModelMode byte_model_mode = ByteModelMode::Split;
    uint8_t mode_in = 0;
    in.read(reinterpret_cast<char*>(&mode_in), 1);
    if (mode_in > static_cast<uint8_t>(ByteModelMode::Shared)) {
        std::cerr << "Error: invalid byte-model in encoded file\n";
        return 1;
    }
    byte_model_mode = static_cast<ByteModelMode>(mode_in);
    if (!parse_symbol_bits_value(symbol_bits, symbol_bits)) {
        std::cerr << "Error: invalid symbol-bits in encoded file\n";
        return 1;
    }

    std::vector<std::vector<ContextOffset>> header_phase_offsets;
    std::vector<uint32_t> header_phase_max_totals;
    std::vector<uint16_t> header_phase_alpha_nums;
    std::vector<uint16_t> header_phase_alpha_dens;
    bool header_has_phase_info = false;
    std::streampos header_pos = in.tellg();
    uint8_t maybe_symbols = 0;
    in.read(reinterpret_cast<char*>(&maybe_symbols), 1);
    if (in) {
        int maybe_sp = static_cast<int>(maybe_symbols);
        if (maybe_sp >= 1 && maybe_sp <= 8) {
            try {
                header_phase_offsets.resize(static_cast<size_t>(maybe_sp));
                header_phase_max_totals.resize(static_cast<size_t>(maybe_sp));
                header_phase_alpha_nums.resize(static_cast<size_t>(maybe_sp));
                header_phase_alpha_dens.resize(static_cast<size_t>(maybe_sp));
                for (int s = 0; s < maybe_sp; ++s) {
                    uint8_t nctx = 0;
                    in.read(reinterpret_cast<char*>(&nctx), 1);
                    uint8_t noff = 0;
                    in.read(reinterpret_cast<char*>(&noff), 1);
                    for (int i = 0; i < noff; ++i) {
                        int16_t dx = 0, dy = 0;
                        in.read(reinterpret_cast<char*>(&dx), 2);
                        in.read(reinterpret_cast<char*>(&dy), 2);
                        header_phase_offsets[static_cast<size_t>(s)].push_back({dx, dy});
                    }
                    uint32_t pmax = 0;
                    uint16_t pnum = 1, pden = 1;
                    in.read(reinterpret_cast<char*>(&pmax), 4);
                    in.read(reinterpret_cast<char*>(&pnum), 2);
                    in.read(reinterpret_cast<char*>(&pden), 2);
                    header_phase_max_totals[static_cast<size_t>(s)] = pmax;
                    header_phase_alpha_nums[static_cast<size_t>(s)] = pnum;
                    header_phase_alpha_dens[static_cast<size_t>(s)] = pden;
                }
                header_has_phase_info = true;
            } catch (...) {
                header_has_phase_info = false;
            }
        }
    }
    if (!header_has_phase_info) {
        std::cerr << "Error: stream has no phase information in header\n";
        return 1;
    }

    if (n_ctx < 0) {
        std::cerr << "Error: invalid n_ctx in encoded file\n";
        return 1;
    }

    std::vector<std::vector<ContextOffset>> offset_groups;
    std::vector<uint32_t> max_total_values;
    std::vector<std::pair<uint16_t, uint16_t>> alpha_values;

    offset_groups = header_phase_offsets;
    max_total_values.resize(header_phase_max_totals.size());
    alpha_values.resize(header_phase_alpha_nums.size());
    for (size_t i = 0; i < header_phase_max_totals.size(); ++i) {
        max_total_values[i] = header_phase_max_totals[i];
        alpha_values[i] = std::make_pair(header_phase_alpha_nums[i], header_phase_alpha_dens[i]);
    }

    uint64_t data_len = 0;
    in.read(reinterpret_cast<char*>(&data_len), 8);

    std::vector<uint8_t> encoded(data_len);
    in.read(reinterpret_cast<char*>(encoded.data()), static_cast<std::streamsize>(data_len));
    in.close();

    ArithmeticDecoder decoder(encoded);
    decoder.start();

    uint32_t width = info.width;
    uint32_t height = info.height;
    size_t total_pixels = static_cast<size_t>(width) * height * info.channels;
    bool is_8bit = (bpp <= 8);
    int sample_bits = is_8bit ? 8 : 16;
    if (sample_bits % symbol_bits != 0) {
        std::cerr << "Error: symbol-bits does not divide image bit-depth\n";
        return 1;
    }
    int symbols_per_sample = sample_bits / symbol_bits;
    uint32_t num_symbols = 1u << symbol_bits;

    std::vector<std::vector<ContextOffset>> phase_offsets;
    {
        std::string err;
        if (!resolve_phase_offsets(offset_groups, symbols_per_sample, phase_offsets, err)) {
            std::cerr << "Erro: " << err << "\n";
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
        if (phase_contexts[s] == 0) {
            std::cerr << "Error: invalid context configuration for phase " << s
                      << " (nctx=" << phase_nctx[s] << ", ctx-bits=" << symbol_bits << ")\n";
            return 1;
        }
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
            std::cerr << "Erro: " << err << "\n";
            return 1;
        }
    }

    if (!phase_max_totals.empty()) {
        max_total = phase_max_totals.front();
    }
    if (!phase_alpha_nums.empty()) {
        alpha_num = phase_alpha_nums.front();
        alpha_den = phase_alpha_dens.front();
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
        if (shared_large_context_space) {
            total_contexts = UINT32_MAX;
        }
    } else {
        for (int s = 0; s < symbols_per_sample; ++s) {
            total_contexts += phase_contexts[s];
        }
    }

    int model_count = (byte_model_mode == ByteModelMode::Split) ? symbols_per_sample : 1;
    std::vector<ContextModel> ctx_models(model_count);
    // aplica alpha n/d ao inicializar os modelos (freq inicial = n, incremento = d)
    try {
        if (byte_model_mode == ByteModelMode::Split) {
            for (int s = 0; s < symbols_per_sample; ++s) {
                ctx_models[s].init(phase_contexts[s], num_symbols, phase_max_totals[s],
                                   phase_alpha_nums[s], phase_alpha_dens[s]);
            }
        } else {
            ctx_models[0].init(static_cast<uint32_t>(std::min<uint64_t>(total_contexts, UINT32_MAX)),
                               num_symbols, max_total, alpha_num, alpha_den);
        }
    } catch (const std::bad_alloc&) {
        std::cerr << "Erro: memoria insuficiente para " << total_contexts
                  << " contextos.\n";
        return 1;
    }

    MappedU16 pixels;
    {
        std::string tmp_hint;
        try {
            std::filesystem::path op(output_file);
            if (op.has_parent_path()) tmp_hint = op.parent_path().string();
        } catch (...) {}
        if (!map_create_writable(total_pixels, tmp_hint, pixels)) {
            return 1;
        }
    }
    size_t max_phase_nctx = 0;
    for (int s = 0; s < symbols_per_sample; ++s) {
        max_phase_nctx = std::max(max_phase_nctx, static_cast<size_t>(phase_nctx[s]));
    }
    std::vector<uint16_t> neighbors(max_phase_nctx);
    bool all_phases_same = true;
    for (size_t s = 1; s < phase_offsets.size(); ++s) {
        if (phase_offsets[s].size() != phase_offsets[0].size()) {
            all_phases_same = false;
            break;
        }
        for (size_t i = 0; i < phase_offsets[0].size(); ++i) {
            if (phase_offsets[s][i].dx != phase_offsets[0][i].dx ||
                phase_offsets[s][i].dy != phase_offsets[0][i].dy) {
                all_phases_same = false;
                break;
            }
        }
        if (!all_phases_same) break;
    }

    bool fast_ctx_lr = (byte_model_mode == ByteModelMode::Split &&
                        symbol_bits == 4 &&
                        phase_nctx[0] == 2 &&
                        all_phases_same &&
                        phase_offsets[0].size() == 2 &&
                        phase_offsets[0][0].dx == -1 && phase_offsets[0][0].dy == 0 &&
                        phase_offsets[0][1].dx == -2 && phase_offsets[0][1].dy == 0);

    if (fast_ctx_lr) {
        for (uint32_t row = 0; row < height; row++) {
            size_t row_base = static_cast<size_t>(row) * width;
            for (uint32_t col = 0; col < width; col++) {
                size_t idx = row_base + col;
                uint16_t n0 = (col > 0) ? pixels[idx - 1] : 0;
                uint16_t n1 = (col > 1) ? pixels[idx - 2] : 0;

                uint16_t pixel = 0;
                for (int s = 0; s < symbols_per_sample; s++) {
                    uint32_t shift = static_cast<uint32_t>(4 * (symbols_per_sample - 1 - s));
                    uint32_t ctx = (((n0 >> shift) & 0xFu) << 4) | ((n1 >> shift) & 0xFu);

                    AdaptiveModel& model = ctx_models[s][ctx];
                    uint64_t cum = decoder.get_cum_value(model.total);
                    uint32_t cum_lo = 0;
                    uint32_t cum_hi = 0;
                    uint32_t sym = model.find_symbol_with_interval(cum, cum_lo, cum_hi);
                    decoder.update_interval(cum_lo, cum_hi, model.total);
                    model.update(sym);

                    pixel |= static_cast<uint16_t>(sym) << shift;
                }

                pixels[idx] = pixel;
            }
        }
    } else if (byte_model_mode == ByteModelMode::Split) {
        const bool split_reuse_neighbors = all_phases_same;
        const int split_nctx = static_cast<int>(phase_nctx[0]);
        std::vector<uint16_t> split_neighbors(split_reuse_neighbors ? static_cast<size_t>(split_nctx) : 0);
        std::vector<std::vector<uint16_t>> split_neighbors_by_phase;
        if (!split_reuse_neighbors) {
            split_neighbors_by_phase.resize(static_cast<size_t>(symbols_per_sample));
            for (int s = 0; s < symbols_per_sample; ++s) {
                split_neighbors_by_phase[static_cast<size_t>(s)].resize(static_cast<size_t>(phase_nctx[s]));
            }
        }
        for (uint32_t row = 0; row < height; row++) {
            size_t row_base = static_cast<size_t>(row) * width;
            for (uint32_t col = 0; col < width; col++) {
                size_t idx = row_base + col;
                if (split_reuse_neighbors) {
                    get_neighbors(pixels.data(), row, col, width, height, phase_offsets[0], split_neighbors.data());
                } else {
                    for (int s = 0; s < symbols_per_sample; ++s) {
                        get_neighbors(pixels.data(), row, col, width, height, phase_offsets[s],
                                      split_neighbors_by_phase[static_cast<size_t>(s)].data());
                    }
                }

                uint16_t pixel = 0;
                for (int s = 0; s < symbols_per_sample; s++) {
                    const uint16_t* ctx_neighbors = split_reuse_neighbors
                        ? split_neighbors.data()
                        : split_neighbors_by_phase[static_cast<size_t>(s)].data();
                    int ctx_n = split_reuse_neighbors ? split_nctx : static_cast<int>(phase_nctx[s]);
                    uint32_t ctx = compute_context(ctx_neighbors, ctx_n, s, symbol_bits, symbols_per_sample);

                    AdaptiveModel& model = ctx_models[s][ctx];
                    uint64_t cum = decoder.get_cum_value(model.total);
                    uint32_t cum_lo = 0;
                    uint32_t cum_hi = 0;
                    uint32_t sym = model.find_symbol_with_interval(cum, cum_lo, cum_hi);
                    decoder.update_interval(cum_lo, cum_hi, model.total);
                    model.update(sym);

                    uint32_t shift = static_cast<uint32_t>(symbol_bits * (symbols_per_sample - 1 - s));
                    pixel |= static_cast<uint16_t>(sym) << shift;
                    pixels[idx] = pixel;
                }

                pixels[idx] = pixel;
            }
        }
    } else {
        for (uint32_t row = 0; row < height; row++) {
            size_t row_base = static_cast<size_t>(row) * width;
            const uint16_t* row_pixels = pixels.data() + row_base;
            for (uint32_t col = 0; col < width; col++) {
                size_t idx = row_base + col;

                uint16_t pixel = 0;
                for (int s = 0; s < symbols_per_sample; s++) {
                    const auto& phase = phase_offsets[s];
                    uint32_t base_ctx = compute_context_shared(pixels.data(), row, col, width, height,
                                                        s, symbols_per_sample,
                                                        phase, symbol_bits);

                    uint64_t ctx = shared_large_context_space
                        ? ((static_cast<uint64_t>(static_cast<uint32_t>(s)) << 32) | static_cast<uint64_t>(base_ctx))
                        : (phase_context_base[s] + static_cast<uint64_t>(base_ctx));

                    AdaptiveModel& model = ctx_models[0].get_or_create(ctx, phase_max_totals[s], phase_alpha_nums[s], phase_alpha_dens[s]);
                    uint64_t cum = decoder.get_cum_value(model.total);
                    uint32_t cum_lo = 0;
                    uint32_t cum_hi = 0;
                    uint32_t sym = model.find_symbol_with_interval(cum, cum_lo, cum_hi);
                    decoder.update_interval(cum_lo, cum_hi, model.total);
                    model.update(sym);

                    uint32_t shift = static_cast<uint32_t>(symbol_bits * (symbols_per_sample - 1 - s));
                    pixel |= static_cast<uint16_t>(sym) << shift;
                    pixels[idx] = pixel;
                }

                pixels[idx] = pixel;
            }
        }
    }

    std::string actual_output = output_file;
    if (actual_output.empty()) {
        actual_output = default_output_path(input_file, info);
    }

    if (!actual_output.empty()) {
        std::filesystem::path out_p(actual_output);
        out_p.replace_extension(".raw");
        actual_output = out_p.string();

        try {
            if (out_p.has_parent_path()) {
                std::filesystem::create_directories(out_p.parent_path());
            }
        } catch (...) {
            // ignore directory creation errors
        }

        bool success = write_raw_stream(actual_output, pixels.data(), pixels.size(), info);

        if (!success) {
            std::cerr << "Error: failed to write image\n";
            return 1;
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double decomp_time = std::chrono::duration<double>(end_time - start_time).count();

    if (do_verify) {
        std::string validation = "OK";
        uint32_t decoded_hash = crc32(pixels.data(), pixels.size());
        if (decoded_hash != header_hash) {
            validation = "FAIL";
        }
        printf("%.6f\t%s\n", decomp_time, validation.c_str());
    } else {
        printf("%.6f\n", decomp_time);
    }

    return 0;
}
