#pragma once

#include "adaptive_model.hpp"
#include <vector>
#include <cstdint>
#include <algorithm>
#include <unordered_map>
#include <stdexcept>
#include <array>
#include <limits>
#include <atomic>

struct ContextOffset {
    int dx;
    int dy;
};


// atomic counter definitions are provided after the class declaration

// atomic counter definitions are provided after the class declaration


inline uint32_t calc_num_contexts(int n_ctx, int symbol_bits) {
    if (n_ctx < 0 || symbol_bits <= 0) {
        return 0;
    }
    int total_bits = symbol_bits * n_ctx;
    if (total_bits > 32) {
        return 0;
    }
    if (total_bits == 32) {
        return UINT32_MAX;
    }
    return 1u << total_bits;
    }



inline void get_neighbors(const uint16_t* pixels,
                          uint32_t row, uint32_t col,
                          uint32_t width, uint32_t height,
                          const std::vector<ContextOffset>& offsets,
                          uint16_t* neighbors,
                          uint32_t row_min = 0,
                          uint32_t row_max = std::numeric_limits<uint32_t>::max()) {
    int n_ctx = static_cast<int>(offsets.size());

    for (int i = 0; i < n_ctx; i++) neighbors[i] = 0;

    for (int i = 0; i < n_ctx; i++) {
        int64_t nr = static_cast<int64_t>(row) + offsets[i].dy;
        int64_t nc = static_cast<int64_t>(col) + offsets[i].dx;
        if (nr < static_cast<int64_t>(row_min) || nc < 0 || nr >= static_cast<int64_t>(std::min(height, row_max)) || nc >= static_cast<int64_t>(width)) {
            continue;
        }

        size_t nidx = static_cast<size_t>(nr) * width + static_cast<size_t>(nc);
        neighbors[i] = pixels[nidx];
    }
}

inline uint32_t compute_context(const uint16_t* neighbors, int n_ctx,
                                int symbol_pos, int symbol_bits,
                                int symbols_per_sample) {
    uint32_t ctx = 0;
    const uint32_t levels = 1u << symbol_bits;
    const uint32_t symbol_mask = (1u << symbol_bits) - 1u;
    const int shift = symbol_bits * (symbols_per_sample - 1 - symbol_pos);
    const uint16_t* p = neighbors;
    for (int i = 0; i < n_ctx; ++i) {
        uint32_t symbol_val = ((*p++) >> shift) & symbol_mask;
        ctx = (ctx << symbol_bits) | symbol_val;
    }
    return ctx;
}

inline uint32_t compute_context_shared(const uint16_t* pixels,
                                       uint32_t row, uint32_t col,
                                       uint32_t width, uint32_t height,
                                       int symbol_pos, int symbols_per_sample,
                                       const std::vector<ContextOffset>& offsets,
                                       int symbol_bits,
                                       uint32_t row_min = 0,
                                       uint32_t row_max = std::numeric_limits<uint32_t>::max()) {
    uint32_t ctx = 0;
    const uint32_t levels = 1u << symbol_bits;
    const uint32_t symbol_mask = levels - 1u;
    const bool symbols_power_of_two = symbols_per_sample > 0 &&
                                      (symbols_per_sample & (symbols_per_sample - 1)) == 0;
    const int symbols_shift = symbols_power_of_two ? __builtin_ctz(static_cast<unsigned int>(symbols_per_sample)) : 0;

    const int64_t cur_sx = symbols_power_of_two
        ? ((static_cast<int64_t>(col) << symbols_shift) + symbol_pos)
        : (static_cast<int64_t>(col) * symbols_per_sample + symbol_pos);
    const int64_t max_sx = symbols_power_of_two
        ? (static_cast<int64_t>(width) << symbols_shift)
        : (static_cast<int64_t>(width) * symbols_per_sample);
    const int64_t row_base = static_cast<int64_t>(row) * width;
    const int64_t prev_row_base = row_base - width;
    const uint16_t* pix = pixels;
    const ContextOffset* off = offsets.data();
    const size_t n_off = offsets.size();
    const int64_t r_min = static_cast<int64_t>(row_min);
    const int64_t r_max = static_cast<int64_t>(std::min(height, row_max));

    if (symbols_power_of_two) {
        const int phase_mask = symbols_per_sample - 1;
        for (size_t i = 0; i < n_off; ++i) {
            uint32_t symbol_val = 0;
            int64_t nr = static_cast<int64_t>(row) + off[i].dy;
            int64_t nsx = cur_sx + off[i].dx;
            if (nr >= r_min && nr < r_max && nsx >= 0 && nsx < max_sx) {
                int64_t ncol = nsx >> symbols_shift;
                int nphase = static_cast<int>(nsx & phase_mask);
                size_t nidx;
                if (off[i].dy == 0) {
                    nidx = static_cast<size_t>(row_base + ncol);
                } else if (off[i].dy == -1) {
                    nidx = static_cast<size_t>(prev_row_base + ncol);
                } else {
                    nidx = static_cast<size_t>(nr) * width + static_cast<size_t>(ncol);
                }
                const int shift = symbol_bits * (symbols_per_sample - 1 - nphase);
                symbol_val = (pix[nidx] >> shift) & symbol_mask;
            }
            ctx = (ctx << symbol_bits) | symbol_val;
        }
    } else {
        for (size_t i = 0; i < n_off; ++i) {
            uint32_t symbol_val = 0;
            int64_t nr = static_cast<int64_t>(row) + off[i].dy;
            int64_t nsx = cur_sx + off[i].dx;
            if (nr >= r_min && nr < r_max && nsx >= 0 && nsx < max_sx) {
                int64_t ncol = nsx / symbols_per_sample;
                int nphase = static_cast<int>(nsx % symbols_per_sample);
                size_t nidx;
                if (off[i].dy == 0) {
                    nidx = static_cast<size_t>(row_base + ncol);
                } else if (off[i].dy == -1) {
                    nidx = static_cast<size_t>(prev_row_base + ncol);
                } else {
                    nidx = static_cast<size_t>(nr) * width + static_cast<size_t>(ncol);
                }
                const int shift = symbol_bits * (symbols_per_sample - 1 - nphase);
                symbol_val = (pix[nidx] >> shift) & symbol_mask;
            }
            ctx = (ctx << symbol_bits) | symbol_val;
        }
    }
    return ctx;
}


inline uint32_t compute_context_shared_row(const uint16_t* row_pixels,
                                           uint32_t col, uint32_t width,
                                           int symbol_pos, int symbols_per_sample,
                                           const std::vector<ContextOffset>& offsets,
                                           int symbol_bits) {
    uint32_t ctx = 0;
    const uint32_t levels = 1u << symbol_bits;
    const uint32_t symbol_mask = levels - 1u;
    const bool symbols_power_of_two = symbols_per_sample > 0 &&
                                      (symbols_per_sample & (symbols_per_sample - 1)) == 0;
    const int symbols_shift = symbols_power_of_two ? __builtin_ctz(static_cast<unsigned int>(symbols_per_sample)) : 0;

    const int cur_sx = symbols_power_of_two
        ? ((static_cast<int>(col) << symbols_shift) + symbol_pos)
        : (static_cast<int>(col) * symbols_per_sample + symbol_pos);
    const int max_sx = symbols_power_of_two
        ? (static_cast<int>(width) << symbols_shift)
        : (static_cast<int>(width) * symbols_per_sample);
    const ContextOffset* off = offsets.data();
    const size_t n_off = offsets.size();

    if (symbols_power_of_two) {
        const int phase_mask = symbols_per_sample - 1;
        for (size_t i = 0; i < n_off; ++i) {
            uint32_t symbol_val = 0;
            int nsx = cur_sx + off[i].dx;
            if (nsx >= 0 && nsx < max_sx) {
                int ncol = nsx >> symbols_shift;
                int nphase = nsx & phase_mask;
                const int shift = symbol_bits * (symbols_per_sample - 1 - nphase);
                symbol_val = (row_pixels[ncol] >> shift) & symbol_mask;
            }
            ctx = (ctx << symbol_bits) | symbol_val;
        }
    } else {
        for (size_t i = 0; i < n_off; ++i) {
            uint32_t symbol_val = 0;
            int nsx = cur_sx + off[i].dx;
            if (nsx >= 0 && nsx < max_sx) {
                int ncol = nsx / symbols_per_sample;
                int nphase = nsx % symbols_per_sample;
                const int shift = symbol_bits * (symbols_per_sample - 1 - nphase);
                symbol_val = (row_pixels[ncol] >> shift) & symbol_mask;
            }
            ctx = (ctx << symbol_bits) | symbol_val;
        }
    }
    return ctx;
}

class ContextModel {
public:
    ContextModel()
        : num_symbols_(AdaptiveModel::DEFAULT_NSYM),
          max_total_(16384),
          alpha_num_(1),
          alpha_den_(1),
          use_dense_array_(true),
          dense_array_models_(),
                    sparse_models_(),
                    cache_keys_(),
                    cache_values_() {
                clear_cache();
        }

    void init(uint32_t num_ctx, uint32_t num_symbols, uint32_t max_total = 16384,
              uint16_t alpha_num = 1, uint16_t alpha_den = 1) {
        static const uint32_t DENSE_ARRAY_LIMIT = 4096;

        num_symbols_ = num_symbols;
        max_total_ = max_total;
        alpha_num_ = alpha_num;
        alpha_den_ = alpha_den;
        use_dense_array_ = (num_ctx <= DENSE_ARRAY_LIMIT);

        if (use_dense_array_) {
            sparse_models_.clear();
            dense_array_models_.clear();
            dense_array_models_.reserve(num_ctx);
            for (uint32_t i = 0; i < num_ctx; ++i) {
                dense_array_models_.emplace_back(num_symbols_, max_total_, alpha_num_, alpha_den_);
            }
        } else {
            dense_array_models_.clear();
            sparse_models_.clear();
            sparse_models_.max_load_factor(0.7f);
            sparse_models_.reserve(static_cast<size_t>(std::min<uint32_t>(num_ctx, 131072u)));
        }
        clear_cache();
    }

    AdaptiveModel& operator[](uint64_t context) {
        if (use_dense_array_) {
            return dense_array_models_[static_cast<size_t>(context)];
        }

        return lookup_sparse(context, max_total_, alpha_num_, alpha_den_);
    }

    AdaptiveModel& get_or_create(uint64_t context, uint32_t current_max_total, uint16_t current_alpha_num, uint16_t current_alpha_den) {
        if (use_dense_array_) {
            return dense_array_models_[static_cast<size_t>(context)];
        }

        return lookup_sparse(context, current_max_total, current_alpha_num, current_alpha_den);
    }

    const AdaptiveModel& operator[](uint64_t context) const {
        if (use_dense_array_) {
            return dense_array_models_[static_cast<size_t>(context)];
        }

        auto it = sparse_models_.find(context);
        if (it == sparse_models_.end()) {
            throw std::out_of_range("context model not initialized");
        }
        return it->second;
    }

    void reset() {
        if (use_dense_array_) {
            for (auto& model : dense_array_models_) {
                model.reset();
            }
            clear_cache();
            return;
        }

        for (auto& kv : sparse_models_) {
            kv.second.reset();
        }
        clear_cache();
    }


    size_t size() const {
        if (use_dense_array_) {
            return dense_array_models_.size();
        }
        return sparse_models_.size();
    }

private:
    uint32_t num_symbols_;
    uint32_t max_total_;
    uint16_t alpha_num_;
    uint16_t alpha_den_;
    bool use_dense_array_;
    std::vector<AdaptiveModel> dense_array_models_;
    std::unordered_map<uint64_t, AdaptiveModel> sparse_models_;
    static constexpr size_t CONTEXT_CACHE_SIZE = 8192;
    static constexpr uint64_t CONTEXT_CACHE_EMPTY = std::numeric_limits<uint64_t>::max();
    std::array<uint64_t, CONTEXT_CACHE_SIZE> cache_keys_;
    std::array<AdaptiveModel*, CONTEXT_CACHE_SIZE> cache_values_;

    static size_t cache_slot(uint64_t context) {
        context ^= context >> 33;
        context *= 0xff51afd7ed558ccdULL;
        context ^= context >> 33;
        context *= 0xc4ceb9fe1a85ec53ULL;
        context ^= context >> 33;
        return static_cast<size_t>(context) & (CONTEXT_CACHE_SIZE - 1);
    }

    void clear_cache() {
        cache_keys_.fill(CONTEXT_CACHE_EMPTY);
        cache_values_.fill(nullptr);
    }

    AdaptiveModel& lookup_sparse(uint64_t context, uint32_t current_max_total, uint16_t current_alpha_num, uint16_t current_alpha_den) {
        size_t slot = cache_slot(context);
        if (cache_keys_[slot] == context && cache_values_[slot] != nullptr) {
            return *cache_values_[slot];
        }

        auto [it, inserted] = sparse_models_.try_emplace(
            context,
            num_symbols_,
            current_max_total,
            current_alpha_num,
            current_alpha_den);

        cache_keys_[slot] = context;
        cache_values_[slot] = &it->second;
        return it->second;
    }
};
