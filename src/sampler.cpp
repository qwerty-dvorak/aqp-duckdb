#include "sampler.hpp"
#include "duckdb.hpp"
#include <sstream>
#include <cmath>

SampleResult reservoir_sample(duckdb::Connection& conn,
                               const std::string& view_name,
                               const std::string& column,
                               double fraction) {
    SampleResult result;
    result.sample_fraction = fraction;

    // Get population size
    {
        std::string count_sql = "SELECT COUNT(*) FROM " + view_name;
        auto count_res = conn.Query(count_sql);
        if (count_res->HasError()) {
            result.population_size = 0;
            return result;
        }
        auto chunk = count_res->Fetch();
        if (chunk && chunk->size() > 0) {
            result.population_size = static_cast<size_t>(chunk->GetValue(0, 0).GetValue<int64_t>());
        } else {
            result.population_size = 0;
        }
    }

    // Sample data using DuckDB's built-in reservoir sampling
    {
        int pct = static_cast<int>(std::round(fraction * 100.0));
        if (pct < 1) pct = 1;
        std::ostringstream oss;
        oss << "SELECT CAST(" << column << " AS DOUBLE) FROM " << view_name
            << " USING SAMPLE " << pct << "% (reservoir)";
        auto sample_res = conn.Query(oss.str());
        if (sample_res->HasError()) {
            return result;
        }
        while (true) {
            auto chunk = sample_res->Fetch();
            if (!chunk || chunk->size() == 0) break;
            for (duckdb::idx_t i = 0; i < chunk->size(); i++) {
                auto val = chunk->GetValue(0, i);
                if (!val.IsNull()) {
                    result.values.push_back(val.GetValue<double>());
                }
            }
        }
    }

    return result;
}

std::map<std::string, SampleResult> stratified_sample(duckdb::Connection& conn,
                                                        const std::string& view_name,
                                                        const std::string& value_col,
                                                        const std::string& group_col,
                                                        double fraction) {
    std::map<std::string, SampleResult> results;

    int pct = static_cast<int>(std::round(fraction * 100.0));
    if (pct < 1) pct = 1;

    std::ostringstream oss;
    oss << "SELECT CAST(" << group_col << " AS VARCHAR), CAST(" << value_col << " AS DOUBLE) FROM " << view_name
        << " USING SAMPLE " << pct << "% (reservoir)";

    auto sample_res = conn.Query(oss.str());
    if (sample_res->HasError()) {
        return results;
    }

    // Collect into groups
    std::map<std::string, std::vector<double>> grouped;
    while (true) {
        auto chunk = sample_res->Fetch();
        if (!chunk || chunk->size() == 0) break;
        for (duckdb::idx_t i = 0; i < chunk->size(); i++) {
            auto key_val = chunk->GetValue(0, i);
            auto num_val = chunk->GetValue(1, i);
            std::string key = key_val.IsNull() ? "NULL" : key_val.ToString();
            if (!num_val.IsNull()) {
                grouped[key].push_back(num_val.GetValue<double>());
            }
        }
    }

    // Get population size for scaling
    std::string count_sql = "SELECT COUNT(*) FROM " + view_name;
    auto count_res = conn.Query(count_sql);
    size_t pop_size = 0;
    if (!count_res->HasError()) {
        auto chunk = count_res->Fetch();
        if (chunk && chunk->size() > 0) {
            pop_size = static_cast<size_t>(chunk->GetValue(0, 0).GetValue<int64_t>());
        }
    }

    // Convert to SampleResult map
    for (auto& pair : grouped) {
        SampleResult sr;
        sr.values = std::move(pair.second);
        sr.population_size = pop_size;
        sr.sample_fraction = fraction;
        results[pair.first] = std::move(sr);
    }

    return results;
}
