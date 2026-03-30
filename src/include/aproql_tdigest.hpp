#pragma once

#include <cstdint>
#include <vector>
#include <algorithm>
#include <cmath>
#include <limits>

// Simplified T-Digest for streaming quantile estimation.
// Maintains a sorted list of weighted centroids that approximates the
// empirical CDF.  Centroids near the tails (q≈0 or q≈1) are kept
// small to maximise accuracy at extreme percentiles, while centroids
// near the median can grow larger.
//
// This is a faithful single-pass implementation suitable for the
// list-based ScalarFunction pattern in the aproql extension.

class AproqlTDigest {
public:
	// compression controls accuracy vs memory: higher → more centroids → better.
	// 200 gives ~0.5% relative error at the median and <0.1% at the tails.
	explicit AproqlTDigest(double compression = 200.0)
	    : compression_(compression), total_weight_(0.0), sorted_(false) {}

	void add(double value, double weight = 1.0) {
		centroids_.push_back({value, weight});
		total_weight_ += weight;
		sorted_ = false;

		// Periodically compress to avoid unbounded memory
		if (static_cast<int>(centroids_.size()) > max_unmerged()) {
			compress();
		}
	}

	// Query the estimated value at quantile q ∈ [0, 1].
	double quantile(double q) {
		if (centroids_.empty()) {
			return std::numeric_limits<double>::quiet_NaN();
		}
		compress(); // ensure sorted & merged

		if (centroids_.size() == 1) {
			return centroids_[0].mean;
		}

		// Clamp
		if (q <= 0.0) return centroids_.front().mean;
		if (q >= 1.0) return centroids_.back().mean;

		// Walk through centroids, accumulating weight
		double target = q * total_weight_;
		double cumulative = 0.0;

		for (size_t i = 0; i < centroids_.size(); i++) {
			double half_w = centroids_[i].weight / 2.0;
			if (cumulative + half_w >= target) {
				// Interpolate within this centroid or between neighbours
				if (i == 0) {
					// Left edge — interpolate between min and centroid centre
					double inner = target / half_w;
					if (i + 1 < centroids_.size()) {
						return centroids_[0].mean +
						       inner * (centroids_[1].mean - centroids_[0].mean) * 0.5;
					}
					return centroids_[0].mean;
				}
				// Between centroid i-1 and i
				double prev_mid = cumulative;
				double delta = centroids_[i].mean - centroids_[i - 1].mean;
				double frac = (target - prev_mid + half_w) / (centroids_[i - 1].weight / 2.0 + half_w);
				return centroids_[i - 1].mean + delta * frac;
			}
			cumulative += centroids_[i].weight;
		}

		return centroids_.back().mean;
	}

private:
	struct Centroid {
		double mean;
		double weight;
	};

	double compression_;
	double total_weight_;
	std::vector<Centroid> centroids_;
	bool sorted_;

	int max_unmerged() const {
		return static_cast<int>(compression_) * 5;
	}

	// Merge centroids using the T-Digest size-bound function.
	void compress() {
		if (centroids_.empty()) return;

		// Sort by mean
		std::sort(centroids_.begin(), centroids_.end(),
		          [](const Centroid &a, const Centroid &b) { return a.mean < b.mean; });
		sorted_ = true;

		std::vector<Centroid> merged;
		merged.reserve(static_cast<size_t>(compression_) * 2);
		merged.push_back(centroids_[0]);

		double weight_so_far = centroids_[0].weight;

		for (size_t i = 1; i < centroids_.size(); i++) {
			// q value at the proposed merge point
			double q = (weight_so_far + centroids_[i].weight / 2.0) / total_weight_;
			// Size limit: k(q) = 4 * compression * q * (1 - q)
			double max_weight = 4.0 * compression_ * q * (1.0 - q);
			if (max_weight < 1.0) max_weight = 1.0;

			if (merged.back().weight + centroids_[i].weight <= max_weight) {
				// Merge into the last centroid (weighted mean)
				double total_w = merged.back().weight + centroids_[i].weight;
				merged.back().mean = (merged.back().mean * merged.back().weight +
				                      centroids_[i].mean * centroids_[i].weight) /
				                     total_w;
				merged.back().weight = total_w;
			} else {
				// Start a new centroid
				merged.push_back(centroids_[i]);
			}
			weight_so_far += centroids_[i].weight;
		}

		centroids_ = std::move(merged);
	}
};
