#pragma once

#include <vector>
#include <array>
#include <cstdint>
#include <algorithm>
#pragma once

#include <vector>
#include <array>
#include <cstdint>
#include <algorithm>
#include <stdexcept>

class AdaptiveModel {
public:
    static const int DEFAULT_NSYM = 256;

    uint32_t total;
    uint32_t max_total_;
    uint16_t alpha_num_;
    uint16_t alpha_den_;

        // the formula is p(s|c) = (cont_s + alpha) / (cont_total + alpha*|S|), with alpha = n/d
        // multiplying both sides by d gives p(s|c) = (d*cont_s + n) / (d*cont_total + n*|S|)
        // at the start cont_s = 0 and cont_total = 0, so the initial probability is n/(n*|S|) = 1/|S|
        // with 4 bits and 2^4 = 16, the initial probability is 1/16 = 0.0625
        // note: the initial counts are larger, but the relative weight is the same, so the initial probability is still 1/|S|
        // implementation: initialize frequencies with n and add d for each observed symbol
        explicit AdaptiveModel(uint32_t num_symbols = DEFAULT_NSYM, uint32_t max_total = 16384,
                                                     uint16_t alpha_num = 1, uint16_t alpha_den = 1)
                : total(0), max_total_(max_total),
                    alpha_num_(alpha_num),
                    alpha_den_(alpha_den),
                    small_mode_(num_symbols == 16),
                    // alpha = n/d: iniciamos as frequencias com n
                    freq_(small_mode_ ? 0 : num_symbols, alpha_num_),
          fenwick_(small_mode_ ? 0 : num_symbols + 1, 0),
          num_symbols_(num_symbols),
          small_freq_{},
          small_fenwick_{} {
        if (small_mode_) {
            std::fill(small_freq_.begin(), small_freq_.end(), alpha_num_);
            std::fill(small_fenwick_.begin(), small_fenwick_.end(), 0);
            fenwick_top_bit_ = 0;
        }
        rebuild_fenwick();
    }

    void reset() {
        if (small_mode_) {
            std::fill(small_freq_.begin(), small_freq_.end(), alpha_num_);
        } else {
            std::fill(freq_.begin(), freq_.end(), alpha_num_);
            fenwick_top_bit_ = 1u;
            while ((fenwick_top_bit_ << 1) <= num_symbols_) fenwick_top_bit_ <<= 1;
        }
        rebuild_fenwick();
    }

    
    void update(uint32_t symbol) {
        if (symbol >= num_symbols_) {
            throw std::out_of_range("symbol out of range");
        }
        // cada simbolo observado soma d (contagem em escala d)
        if (small_mode_) {
            small_freq_[symbol] = static_cast<uint32_t>(small_freq_[symbol] + alpha_den_);
            total += alpha_den_;

            if (small_freq_[symbol] >= rescale_threshold()) {
                rescale();
            }
            return;
        } else {
            freq_[symbol] = static_cast<uint32_t>(freq_[symbol] + alpha_den_);
            fenwick_add(symbol, alpha_den_);
            total += alpha_den_;

            if (symbol_freq(symbol) >= rescale_threshold()) {
                rescale();
            }
            return;
        }
    }

    inline uint32_t cumulative_low(uint32_t symbol) const {
        if (small_mode_) {
            const uint32_t *p = small_freq_.data();
            uint32_t sum = 0;
            for (uint32_t i = 0; i < symbol; ++i) {
                sum += *p++;
            }
            return sum;
        }
        return (symbol == 0) ? 0 : fenwick_prefix(symbol - 1);
    }

    inline uint32_t cumulative_high(uint32_t symbol) const {
        if (small_mode_) {
            const uint32_t *p = small_freq_.data();
            uint32_t sum = 0;
            for (uint32_t i = 0; i <= symbol; ++i) {
                sum += *p++;
            }
            return sum;
        }
        return fenwick_prefix(symbol);
    }

    inline void cumulative_interval(uint32_t symbol, uint32_t& low, uint32_t& high) const {
        if (small_mode_) {
            const uint32_t *p = small_freq_.data();
            uint32_t sum = 0;
            for (uint32_t i = 0; i <= symbol; ++i) {
                sum += *p++;
            }
            high = sum;
            low = high - small_freq_[symbol];
            return;
        }
        high = fenwick_prefix(symbol);
        low = high - symbol_freq(symbol);
    }

    uint32_t symbol_freq(uint32_t symbol) const {
        return small_mode_ ? small_freq_[symbol] : freq_[symbol];
    }

    inline uint32_t find_symbol(uint64_t cum_value) const {
        if (small_mode_) {
            const uint32_t *p = small_freq_.data();
            uint32_t sum = 0;
            sum += p[0]; if (cum_value < sum) return 0;
            sum += p[1]; if (cum_value < sum) return 1;
            sum += p[2]; if (cum_value < sum) return 2;
            sum += p[3]; if (cum_value < sum) return 3;
            sum += p[4]; if (cum_value < sum) return 4;
            sum += p[5]; if (cum_value < sum) return 5;
            sum += p[6]; if (cum_value < sum) return 6;
            sum += p[7]; if (cum_value < sum) return 7;
            sum += p[8]; if (cum_value < sum) return 8;
            sum += p[9]; if (cum_value < sum) return 9;
            sum += p[10]; if (cum_value < sum) return 10;
            sum += p[11]; if (cum_value < sum) return 11;
            sum += p[12]; if (cum_value < sum) return 12;
            sum += p[13]; if (cum_value < sum) return 13;
            sum += p[14]; if (cum_value < sum) return 14;
            sum += p[15]; if (cum_value < sum) return 15;
            return num_symbols_ - 1;
        }

        uint32_t idx = 0;
        uint32_t bit = fenwick_top_bit_;

        uint64_t sum = 0;
        while (bit != 0) {
            uint32_t next = idx + bit;
            if (next <= num_symbols_ && sum + fenwick_value(next) <= cum_value) {
                idx = next;
                sum += fenwick_value(next);
            }
            bit >>= 1;
        }

        if (idx >= num_symbols_) {
            return num_symbols_ - 1;
        }
        return idx;
    }

    inline uint32_t find_symbol_with_interval(uint64_t cum_value, uint32_t& low, uint32_t& high) const {
        if (small_mode_) {
            const uint32_t *p = small_freq_.data();
            uint32_t sum = 0;
            uint32_t next;
            next = sum + p[0]; if (cum_value < next) { low = sum; high = next; return 0; } sum = next;
            next = sum + p[1]; if (cum_value < next) { low = sum; high = next; return 1; } sum = next;
            next = sum + p[2]; if (cum_value < next) { low = sum; high = next; return 2; } sum = next;
            next = sum + p[3]; if (cum_value < next) { low = sum; high = next; return 3; } sum = next;
            next = sum + p[4]; if (cum_value < next) { low = sum; high = next; return 4; } sum = next;
            next = sum + p[5]; if (cum_value < next) { low = sum; high = next; return 5; } sum = next;
            next = sum + p[6]; if (cum_value < next) { low = sum; high = next; return 6; } sum = next;
            next = sum + p[7]; if (cum_value < next) { low = sum; high = next; return 7; } sum = next;
            next = sum + p[8]; if (cum_value < next) { low = sum; high = next; return 8; } sum = next;
            next = sum + p[9]; if (cum_value < next) { low = sum; high = next; return 9; } sum = next;
            next = sum + p[10]; if (cum_value < next) { low = sum; high = next; return 10; } sum = next;
            next = sum + p[11]; if (cum_value < next) { low = sum; high = next; return 11; } sum = next;
            next = sum + p[12]; if (cum_value < next) { low = sum; high = next; return 12; } sum = next;
            next = sum + p[13]; if (cum_value < next) { low = sum; high = next; return 13; } sum = next;
            next = sum + p[14]; if (cum_value < next) { low = sum; high = next; return 14; } sum = next;
            next = sum + p[15]; if (cum_value < next) { low = sum; high = next; return 15; } sum = next;
            low = total - small_freq_[num_symbols_ - 1];
            high = total;
            return num_symbols_ - 1;
        }

        uint32_t idx = 0;
        uint32_t bit = fenwick_top_bit_;

        uint64_t sum = 0;
        while (bit != 0) {
            uint32_t next = idx + bit;
            if (next <= num_symbols_ && sum + fenwick_value(next) <= cum_value) {
                idx = next;
                sum += fenwick_value(next);
            }
            bit >>= 1;
        }

        if (idx >= num_symbols_) {
            idx = num_symbols_ - 1;
        }
        low = static_cast<uint32_t>(sum);
        high = low + symbol_freq(idx);
        return idx;
    }

    uint32_t num_symbols() const {
        return num_symbols_;
    }

private:
    bool small_mode_;
    std::vector<uint32_t> freq_;
    std::vector<uint32_t> fenwick_;
    uint32_t num_symbols_;
    std::array<uint32_t, 16> small_freq_;
    std::array<uint32_t, 17> small_fenwick_;
    uint32_t fenwick_top_bit_ = 0;

    uint32_t fenwick_value(uint32_t one_based_idx) const {
        return small_mode_ ? small_fenwick_[one_based_idx] : fenwick_[one_based_idx];
    }

    void fenwick_add(uint32_t idx, uint32_t delta) {
        for (uint32_t i = idx + 1; i <= num_symbols_; i += (i & -i)) {
            if (small_mode_) {
                small_fenwick_[i] = static_cast<uint32_t>(small_fenwick_[i] + delta);
            } else {
                fenwick_[i] = static_cast<uint32_t>(fenwick_[i] + delta);
            }
        }
    }

    uint32_t fenwick_prefix(uint32_t idx) const {
        uint32_t sum = 0;
        for (uint32_t i = idx + 1; i > 0; i -= (i & -i)) {
            sum += fenwick_value(i);
        }
        return sum;
    }

    void rescale() {
        for (uint32_t i = 0; i < num_symbols_; i++) {
            if (small_mode_) {
                small_freq_[i] = std::max<uint32_t>(alpha_num_, small_freq_[i] / 2);
            } else {
                freq_[i] = std::max<uint32_t>(alpha_num_, freq_[i] / 2);
            }
        }
        rebuild_fenwick();
    }

    void rebuild_fenwick() {
        total = 0;
        if (small_mode_) {
            for (uint32_t i = 0; i < num_symbols_; ++i) {
                total += small_freq_[i];
            }
            return;
        } else {
            std::fill(fenwick_.begin(), fenwick_.end(), 0);
        }
        for (uint32_t i = 0; i < num_symbols_; ++i) {
            uint32_t f = small_mode_ ? small_freq_[i] : freq_[i];
            total += f;
            fenwick_add(i, f);
        }
    }

    uint64_t rescale_threshold() const {
        return static_cast<uint64_t>(alpha_den_) * static_cast<uint64_t>(max_total_) + alpha_num_;
    }
};
