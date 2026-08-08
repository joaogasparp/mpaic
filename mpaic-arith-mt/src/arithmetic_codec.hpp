// arithmetic coder used here is based on the implementation from
// https://github.com/dmitrykravchenko2018/arithmetic_coding
// and on the 1987 paper by witten, neal, and cleary.

#pragma once

#include <stdio.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>


const int CODE_VALUE = 31;
const uint32_t MAX_VALUE = ((1u << CODE_VALUE) - 1);
const uint32_t FIRST_QTR = (MAX_VALUE / 4 + 1);
const uint32_t HALF = (2 * FIRST_QTR);
const uint32_t THIRD_QTR = (3 * FIRST_QTR);


class ArithmeticEncoder {
private:
    uint32_t low, high;
    int opposite_bits;
    int buffer;
    int bits_in_buf;
    std::vector<uint8_t>& output;

    void write_bit(int bit) {
        buffer >>= 1;
        if (bit)
            buffer |= 0x80;
        bits_in_buf++;
        if (bits_in_buf == 8) {
            output.push_back(static_cast<uint8_t>(buffer));
            bits_in_buf = 0;
        }
    }

    void output_bits(int bit) {
        write_bit(bit);
        while (opposite_bits > 0) {
            write_bit(!bit);
            opposite_bits--;
        }
    }

public:
    ArithmeticEncoder(std::vector<uint8_t>& out) 
        : low(0), high(MAX_VALUE), opposite_bits(0), buffer(0), bits_in_buf(0), output(out) {
        output.clear();
    }

    void encode_interval(uint64_t sym_low, uint64_t sym_high, uint64_t total) {
        uint64_t range = high - low + 1;

        high = low + static_cast<uint32_t>((range * sym_high) / total) - 1;
        low = low + static_cast<uint32_t>((range * sym_low) / total);

        for (;;) {
            if (high < HALF) {
                output_bits(0);
            } else if (low >= HALF) {
                output_bits(1);
                low -= HALF;
                high -= HALF;
            } else if (low >= FIRST_QTR && high < THIRD_QTR) {
                opposite_bits++;
                low -= FIRST_QTR;
                high -= FIRST_QTR;
            } else {
                break;
            }
            low = 2 * low;
            high = 2 * high + 1;
        }
    }

    
    
    
    void encode(uint32_t symbol, const std::vector<uint64_t>& cum_freq, uint64_t total) {
        uint64_t sym_low = (symbol == 0) ? 0 : cum_freq[symbol - 1];
        uint64_t sym_high = cum_freq[symbol];
        encode_interval(sym_low, sym_high, total);
    }

    void finish() {
        opposite_bits++;
        if (low < FIRST_QTR)
            output_bits(0);
        else
            output_bits(1);
        
        
        if (bits_in_buf > 0) {
            buffer >>= (8 - bits_in_buf);
            output.push_back(static_cast<uint8_t>(buffer));
        }
    }
};


class ArithmeticDecoder {
private:
    uint32_t low, high;
    uint32_t value;
    int buffer;
    int bits_in_buf;
    const std::vector<uint8_t>& input;
    size_t byte_index;

    int get_bit() {
        if (bits_in_buf == 0) {
            if (byte_index >= input.size()) {
                buffer = 0;
            } else {
                buffer = input[byte_index++];
            }
            bits_in_buf = 8;
        }
        int t = buffer & 1;
        buffer >>= 1;
        bits_in_buf--;
        return t;
    }

public:
    ArithmeticDecoder(const std::vector<uint8_t>& in) 
        : low(0), high(MAX_VALUE), value(0), buffer(0), bits_in_buf(0), input(in), byte_index(0) {}

    void start() {
        value = 0;
        for (int i = 1; i <= CODE_VALUE; i++)
            value = 2 * value + get_bit();
    }

    uint64_t get_cum_value(uint64_t total) const {
        uint64_t range = high - low + 1;
        return (((static_cast<uint64_t>(value - low) + 1) * total - 1) / range);
    }

    void update_interval(uint64_t sym_low, uint64_t sym_high, uint64_t total) {
        uint64_t range = high - low + 1;

        high = low + static_cast<uint32_t>((range * sym_high) / total) - 1;
        low = low + static_cast<uint32_t>((range * sym_low) / total);

        for (;;) {
            if (high < HALF) {
                
            } else if (low >= HALF) {
                value -= HALF;
                low -= HALF;
                high -= HALF;
            } else if (low >= FIRST_QTR && high < THIRD_QTR) {
                value -= FIRST_QTR;
                low -= FIRST_QTR;
                high -= FIRST_QTR;
            } else {
                break;
            }
            low = 2 * low;
            high = 2 * high + 1;
            value = 2 * value + get_bit();
        }
    }

    
    
    uint32_t decode(const std::vector<uint64_t>& cum_freq, uint64_t total, uint32_t num_symbols) {
        uint64_t cum = get_cum_value(total);
        
        
        uint32_t lo = 0, hi = num_symbols;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (cum_freq[mid] <= cum) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        uint32_t symbol_index = lo;
        
        
        uint64_t sym_low = (symbol_index == 0) ? 0 : cum_freq[symbol_index - 1];
        uint64_t sym_high = cum_freq[symbol_index];
        
        update_interval(sym_low, sym_high, total);
        
        return symbol_index;
    }
};
