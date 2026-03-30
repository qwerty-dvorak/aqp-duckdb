#include "algorithms.hpp"
#include <numeric>
#include <cmath>

double compute_stddev(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double mean = std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
    double variance = 0.0;
    for (const auto& val : v) {
        double diff = val - mean;
        variance += diff * diff;
    }
    variance /= static_cast<double>(v.size());
    return std::sqrt(variance);
}

double compute_confidence_interval(const std::vector<double>& sample, size_t /*population_n*/) {
    if (sample.empty()) return 0.0;
    double sd = compute_stddev(sample);
    if (sd == 0.0) return 0.0;
    return 1.96 * sd / std::sqrt(static_cast<double>(sample.size()));
}

double get_error_percentage(double approx_val, double exact_val) {
    if (exact_val == 0.0) return 0.0;
    return std::abs((approx_val - exact_val) / exact_val) * 100.0;
}

double round2(double val) {
    return std::round(val * 100.0) / 100.0;
}
