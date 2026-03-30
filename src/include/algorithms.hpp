#pragma once

#include <vector>
#include <cstddef>

double compute_stddev(const std::vector<double>& v);
double compute_confidence_interval(const std::vector<double>& sample, size_t population_n);
double get_error_percentage(double approx_val, double exact_val);
double round2(double val);
