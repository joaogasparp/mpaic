
// range coder used here is based on the carryless range coder by dmitry subbotin
// as referenced in jkbonfield/rans_static.

#pragma once

#include <vector>
#include <cstdint>


static const uint32_t RC_TOP    = 1u << 24;     
static const uint32_t RC_BOTTOM = 1u << 16;     
static const uint32_t RC_MAX    = 0xFFFFFFFFu;  


class RangeEncoder {
private:
    uint64_t low;       
    uint32_t range;     
    uint32_t ff_count;  
    uint8_t cache;      
    std::vector<uint8_t>& output;

    void shift_low() {
        if (low < 0xFF000000ull || (low >> 32) != 0) {
            
            output.push_back(cache + static_cast<uint8_t>(low >> 32));
            
            while (ff_count > 0) {
                output.push_back(0xFF + static_cast<uint8_t>(low >> 32));
                ff_count--;
            }
            cache = static_cast<uint8_t>((low >> 24) & 0xFF);
        } else {
            ff_count++;
        }
        low = (low & 0xFFFFFF) << 8;
    }

public:
    RangeEncoder(std::vector<uint8_t>& out) 
        : low(0), range(RC_MAX), ff_count(0), cache(0), output(out) {
        output.clear();
    }

    void encode_interval(uint64_t sym_low, uint64_t sym_high, uint64_t total) {
        uint32_t r = range / static_cast<uint32_t>(total);
        low += sym_low * r;
        if (sym_high < total) {
            range = (sym_high - sym_low) * r;
        } else {
            range -= sym_low * r;
        }

        while (range < RC_TOP) {
            shift_low();
            range <<= 8;
        }
    }

    void encode(uint32_t symbol, const std::vector<uint64_t>& cum_freq, uint64_t total) {
        uint64_t sym_low = (symbol == 0) ? 0 : cum_freq[symbol - 1];
        uint64_t sym_high = cum_freq[symbol];
        encode_interval(sym_low, sym_high, total);
    }

    void finish() {
        for (int i = 0; i < 5; i++) {
            shift_low();
        }
    }
};


class RangeDecoder {
private:
    uint32_t low;
    uint32_t code;
    uint32_t range;
    const std::vector<uint8_t>& input;
    size_t byte_index;

    uint8_t get_byte() {
        if (byte_index >= input.size()) return 0;
        return input[byte_index++];
    }

public:
    RangeDecoder(const std::vector<uint8_t>& in) 
        : low(0), code(0), range(RC_MAX), input(in), byte_index(0) {}

    void start() {
        code = 0;
        for (int i = 0; i < 5; i++) {
            code = (code << 8) | get_byte();
        }
    }

    uint64_t get_cum_value(uint64_t total) const {
        uint32_t r = range / static_cast<uint32_t>(total);
        uint32_t cum = (code - low) / r;
        if (cum >= total) cum = static_cast<uint32_t>(total) - 1;
        return cum;
    }

    void update_interval(uint64_t sym_low, uint64_t sym_high, uint64_t total) {
        uint32_t r = range / static_cast<uint32_t>(total);

        low += sym_low * r;
        if (sym_high < total) {
            range = (sym_high - sym_low) * r;
        } else {
            range -= sym_low * r;
        }

        while (range < RC_TOP) {
            code = (code << 8) | get_byte();
            range <<= 8;
            low <<= 8;
        }
    }

    uint32_t decode(const std::vector<uint64_t>& cum_freq, uint64_t total, uint32_t num_symbols) {
        uint64_t cum = get_cum_value(total);
        
        
        uint32_t lo_idx = 0, hi_idx = num_symbols;
        while (lo_idx < hi_idx) {
            uint32_t mid = lo_idx + (hi_idx - lo_idx) / 2;
            if (cum_freq[mid] <= cum) {
                lo_idx = mid + 1;
            } else {
                hi_idx = mid;
            }
        }
        uint32_t symbol = lo_idx;
        
        uint64_t sym_low = (symbol == 0) ? 0 : cum_freq[symbol - 1];
        uint64_t sym_high = cum_freq[symbol];
        
        update_interval(sym_low, sym_high, total);
        
        return symbol;
    }
};
