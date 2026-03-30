#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>

// Lightweight Count-Min Sketch for approximate frequency estimation.
// Uses depth independent hash functions over a 2D counter array.
// Space: depth × width × 8 bytes.  Error ≤ ε·N with probability ≥ 1−δ
// where ε = e/width, δ = (1/2)^depth.
class AproqlCountMinSketch {
public:
	static constexpr int DEFAULT_DEPTH = 4;
	static constexpr int DEFAULT_WIDTH = 2048;

	explicit AproqlCountMinSketch(int depth = DEFAULT_DEPTH, int width = DEFAULT_WIDTH)
	    : depth_(depth), width_(width), table_(depth * width, 0) {
		// Seed each hash row differently using golden-ratio-derived constants
		seeds_.resize(depth);
		uint64_t s = 0x9e3779b97f4a7c15ULL; // golden ratio
		for (int d = 0; d < depth; d++) {
			seeds_[d] = s;
			s = s * 6364136223846793005ULL + 1442695040888963407ULL;
		}
	}

	void add(const std::string &item) {
		for (int d = 0; d < depth_; d++) {
			uint32_t bucket = hash_for_row(d, item);
			table_[d * width_ + bucket]++;
		}
	}

	int64_t estimate(const std::string &item) const {
		int64_t min_count = INT64_MAX;
		for (int d = 0; d < depth_; d++) {
			uint32_t bucket = hash_for_row(d, item);
			int64_t count = table_[d * width_ + bucket];
			min_count = std::min(min_count, count);
		}
		return min_count;
	}

	std::string target_val;
	const std::vector<int64_t>& get_table() const { return table_; }
	
	void merge(const AproqlCountMinSketch& other) {
		for (size_t i = 0; i < table_.size(); i++) {
			table_[i] += other.get_table()[i];
		}
		if (target_val.empty()) {
			target_val = other.target_val;
		}
	}

private:
	int depth_;
	int width_;
	std::vector<int64_t> table_;
	std::vector<uint64_t> seeds_;

	// MurmurHash3-style 64-bit hash with a per-row seed, then mod width.
	uint32_t hash_for_row(int row, const std::string &s) const {
		uint64_t h = seeds_[row];
		const uint64_t c1 = 0x87c37b91114253d5ULL;
		const uint64_t c2 = 0x4cf5ad432745937fULL;

		const auto *data = reinterpret_cast<const uint8_t *>(s.data());
		size_t len = s.size();
		size_t nblocks = len / 8;

		for (size_t i = 0; i < nblocks; i++) {
			uint64_t k = 0;
			std::memcpy(&k, data + i * 8, 8);
			k *= c1;
			k = (k << 31) | (k >> 33);
			k *= c2;
			h ^= k;
			h = (h << 27) | (h >> 37);
			h = h * 5 + 0x52dce729;
		}

		uint64_t k = 0;
		const uint8_t *tail = data + nblocks * 8;
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

		h ^= static_cast<uint64_t>(len);
		h ^= h >> 33;
		h *= 0xff51afd7ed558ccdULL;
		h ^= h >> 33;
		h *= 0xc4ceb9fe1a85ec53ULL;
		h ^= h >> 33;

		return static_cast<uint32_t>(h % static_cast<uint64_t>(width_));
	}
};
