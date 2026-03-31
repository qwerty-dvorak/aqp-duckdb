#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

class AproqlHyperLogLog {
public:
    AproqlHyperLogLog() : registers(m, 0) {}

    void add(const std::string& value) {
        uint64_t h = hash64(value);
        // Use the first b bits to determine the register index
        uint32_t idx = static_cast<uint32_t>(h >> (64 - b));
        // Use the remaining bits to count leading zeros
        uint64_t remaining = (h << b) | ((uint64_t)1 << (b - 1)); // ensure at least 1 bit set
        uint8_t rank = leading_zeros(remaining, 64 - b) + 1;
        if (rank > registers[idx]) {
            registers[idx] = rank;
        }
    }

    uint64_t estimate() const {
        // Compute the harmonic mean of 2^(-register[i])
        double sum = 0.0;
        for (int i = 0; i < m; i++) {
            sum += std::pow(2.0, -static_cast<double>(registers[i]));
        }

        // alpha_m constant
        double alpha_m;
        if (m == 16) {
            alpha_m = 0.673;
        } else if (m == 32) {
            alpha_m = 0.697;
        } else if (m == 64) {
            alpha_m = 0.709;
        } else {
            alpha_m = 0.7213 / (1.0 + 1.079 / static_cast<double>(m));
        }

        double raw_estimate = alpha_m * static_cast<double>(m) * static_cast<double>(m) / sum;

        // Small range correction (linear counting)
        if (raw_estimate <= 2.5 * static_cast<double>(m)) {
            int zeros = 0;
            for (int i = 0; i < m; i++) {
                if (registers[i] == 0) {
                    zeros++;
                }
            }
            if (zeros != 0) {
                raw_estimate = static_cast<double>(m) * std::log(static_cast<double>(m) / static_cast<double>(zeros));
            }
        }

        // Large range correction
        double two_pow_32 = 4294967296.0; // 2^32
        if (raw_estimate > two_pow_32 / 30.0) {
            raw_estimate = -two_pow_32 * std::log(1.0 - raw_estimate / two_pow_32);
        }

        return static_cast<uint64_t>(raw_estimate + 0.5);
    }

    double error_rate() const {
        return 1.04 / std::sqrt(static_cast<double>(m));
    }

private:
    static const int b = 10;
    static const int m = 1024;
    std::vector<uint8_t> registers;

    uint64_t hash64(const std::string& s) const {
        // MurmurHash3-style 64-bit hash
        uint64_t h = 0xcbf29ce484222325ULL; // FNV offset basis as seed
        const uint64_t c1 = 0x87c37b91114253d5ULL;
        const uint64_t c2 = 0x4cf5ad432745937fULL;

        const uint8_t* data = reinterpret_cast<const uint8_t*>(s.data());
        size_t len = s.size();
        size_t nblocks = len / 8;

        // Body - process 8-byte blocks
        for (size_t i = 0; i < nblocks; i++) {
            uint64_t k = 0;
            for (int j = 0; j < 8; j++) {
                k |= static_cast<uint64_t>(data[i * 8 + j]) << (j * 8);
            }
            k *= c1;
            k = (k << 31) | (k >> 33);
            k *= c2;
            h ^= k;
            h = (h << 27) | (h >> 37);
            h = h * 5 + 0x52dce729;
        }

        // Tail
        uint64_t k = 0;
        const uint8_t* tail = data + nblocks * 8;
        size_t remaining = len & 7;
        for (size_t i = 0; i < remaining; i++) {
            k |= static_cast<uint64_t>(tail[i]) << (i * 8);
        }
        if (remaining > 0) {
            k *= c1;
            k = (k << 31) | (k >> 33);
            k *= c2;
            h ^= k;
        }

        // Finalization
        h ^= static_cast<uint64_t>(len);
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;

        return h;
    }

    uint8_t leading_zeros(uint64_t val, int max_bits) const {
        if (val == 0) return static_cast<uint8_t>(max_bits);
        uint8_t count = 0;
        for (int i = max_bits - 1; i >= 0; i--) {
            if ((val >> i) & 1) break;
            count++;
        }
        return count;
    }
};
