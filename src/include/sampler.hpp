#pragma once

#include <string>
#include <vector>
#include <map>
#include "duckdb.hpp"

struct SampleResult {
    std::vector<double> values;
    size_t population_size;
    double sample_fraction;
};

SampleResult reservoir_sample(duckdb::Connection& conn,
                               const std::string& view_name,
                               const std::string& column,
                               double fraction);

std::map<std::string, SampleResult> stratified_sample(duckdb::Connection& conn,
                                                        const std::string& view_name,
                                                        const std::string& value_col,
                                                        const std::string& group_col,
                                                        double fraction);
