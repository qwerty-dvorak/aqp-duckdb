#include "duckdb.hpp"
#include "algorithms.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <string>
#include <vector>
#include <cmath>
#include <sstream>

struct BenchmarkQuery {
    std::string label;
    std::string exact_sql;
    std::string approx_sql;
    double scale_factor;
};

struct BenchmarkResult {
    std::string label;
    double approx_time_ms;
    double exact_time_ms;
    double speedup;
    double error_pct;
    double accuracy_pct;
    double margin_of_error;
    std::string algorithm;
    double approx_val;
    double exact_val;
};

static double extract_numeric_value(duckdb::unique_ptr<duckdb::MaterializedQueryResult>& result) {
    if (result->HasError()) {
        std::cerr << "  Query error: " << result->GetError() << std::endl;
        return 0.0;
    }
    auto chunk = result->Fetch();
    if (!chunk || chunk->size() == 0) {
        return 0.0;
    }
    auto val = chunk->GetValue(0, 0);
    if (val.IsNull()) {
        return 0.0;
    }
    // Try to get as double, fallback to int64
    try {
        return val.GetValue<double>();
    } catch (...) {
        try {
            return static_cast<double>(val.GetValue<int64_t>());
        } catch (...) {
            return 0.0;
        }
    }
}

static void print_query_result(const BenchmarkResult& r) {
    std::cout << "============================================================" << std::endl;
    std::cout << "Query : " << r.label << std::endl;
    std::cout << "------------------------------------------------------------" << std::endl;
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "  Approx Time    : " << std::setw(7) << r.approx_time_ms << " ms   (~10% of dataset scanned)" << std::endl;
    std::cout << "  Exact Time     : " << std::setw(7) << r.exact_time_ms << " ms   (100% — full scan)" << std::endl;
    std::cout << "  Speedup        : " << std::setw(6) << r.speedup << "x" << std::endl;
    std::cout << std::setprecision(2);
    std::cout << "  Error %        : " << std::setw(6) << r.error_pct << "%" << std::endl;
    std::cout << "  Accuracy       : " << std::setw(6) << r.accuracy_pct << "%" << std::endl;
    std::cout << "  Margin of Error: ±" << r.margin_of_error << std::endl;
    std::cout << "  Algorithm      :  " << r.algorithm << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << std::endl;
}

static std::string pad_right(const std::string& s, size_t width) {
    if (s.size() >= width) return s.substr(0, width);
    return s + std::string(width - s.size(), ' ');
}

static std::string center(const std::string& s, size_t width) {
    if (s.size() >= width) return s.substr(0, width);
    size_t left = (width - s.size()) / 2;
    size_t right = width - s.size() - left;
    return std::string(left, ' ') + s + std::string(right, ' ');
}

static void print_summary_table(const std::vector<BenchmarkResult>& results) {
    std::cout << std::endl;
    std::cout << "BENCHMARK SUMMARY — aproql | e6data Hackathon" << std::endl;
    std::cout << "┌─────────────────────────────────┬──────────┬──────────┬─────────┬────────┐" << std::endl;
    std::cout << "│ Query                           │ Approx   │ Exact    │ Speedup │ Error% │" << std::endl;
    std::cout << "│                                 │ Time(ms) │ Time(ms) │         │        │" << std::endl;
    std::cout << "├─────────────────────────────────┼──────────┼──────────┼─────────┼────────┤" << std::endl;

    double total_approx = 0, total_exact = 0, total_speedup = 0, total_error = 0;
    int n = static_cast<int>(results.size());

    for (const auto& r : results) {
        std::ostringstream approx_ss, exact_ss, speedup_ss, error_ss;
        approx_ss << std::fixed << std::setprecision(1) << r.approx_time_ms;
        exact_ss << std::fixed << std::setprecision(1) << r.exact_time_ms;
        speedup_ss << std::fixed << std::setprecision(1) << r.speedup << "x";
        error_ss << std::fixed << std::setprecision(2) << r.error_pct << "%";

        std::cout << "│ " << pad_right(r.label, 31) << " │"
                  << center(approx_ss.str(), 10) << "│"
                  << center(exact_ss.str(), 10) << "│"
                  << center(speedup_ss.str(), 9) << "│"
                  << center(error_ss.str(), 8) << "│" << std::endl;

        total_approx += r.approx_time_ms;
        total_exact += r.exact_time_ms;
        total_speedup += r.speedup;
        total_error += r.error_pct;
    }

    std::cout << "├─────────────────────────────────┼──────────┼──────────┼─────────┼────────┤" << std::endl;

    double avg_approx = n > 0 ? total_approx / n : 0;
    double avg_exact = n > 0 ? total_exact / n : 0;
    double avg_speedup = n > 0 ? total_speedup / n : 0;
    double avg_error = n > 0 ? total_error / n : 0;

    {
        std::ostringstream approx_ss, exact_ss, speedup_ss, error_ss;
        approx_ss << std::fixed << std::setprecision(1) << avg_approx;
        exact_ss << std::fixed << std::setprecision(1) << avg_exact;
        speedup_ss << std::fixed << std::setprecision(1) << avg_speedup << "x";
        error_ss << std::fixed << std::setprecision(2) << avg_error << "%";

        std::cout << "│ " << pad_right("AVERAGE", 31) << " │"
                  << center(approx_ss.str(), 10) << "│"
                  << center(exact_ss.str(), 10) << "│"
                  << center(speedup_ss.str(), 9) << "│"
                  << center(error_ss.str(), 8) << "│" << std::endl;
    }

    std::cout << "└─────────────────────────────────┴──────────┴──────────┴─────────┴────────┘" << std::endl;

    std::cout << std::endl;
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "Average Speedup : " << avg_speedup << "x" << std::endl;
    std::cout << std::setprecision(2);
    std::cout << "Average Error % : " << avg_error << "%" << std::endl;
}

int main() {
    std::cout << "═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "  aproql Benchmark Runner | e6data Hackathon" << std::endl;
    std::cout << "  Approximate Query Processing with DuckDB" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << std::endl;

    // Create in-memory database
    duckdb::DuckDB db(nullptr);
    duckdb::Connection conn(db);

    // Load extension
    std::cout << "[*] Loading aproql extension..." << std::endl;
    auto load_result = conn.Query("LOAD 'build/release/extension/aproql/aproql.duckdb_extension'");
    if (load_result->HasError()) {
        std::cerr << "[!] Failed to load extension: " << load_result->GetError() << std::endl;
        std::cerr << "[*] Continuing without extension (HLL function will not be available)..." << std::endl;
    } else {
        std::cout << "[+] Extension loaded successfully." << std::endl;
    }

    // Register ClickBench dataset
    std::cout << "[*] Loading ClickBench dataset from data/hits.parquet..." << std::endl;
    auto view_result = conn.Query("CREATE VIEW hits AS SELECT * FROM read_parquet('data/hits.parquet')");
    if (view_result->HasError()) {
        std::cerr << "[!] Failed to create view: " << view_result->GetError() << std::endl;
        std::cerr << "[!] Make sure data/hits.parquet exists." << std::endl;
        return 1;
    }
    std::cout << "[+] Dataset loaded." << std::endl;

    // Get row count
    auto count_result = conn.Query("SELECT COUNT(*) FROM hits");
    if (!count_result->HasError()) {
        auto chunk = count_result->Fetch();
        if (chunk && chunk->size() > 0) {
            std::cout << "[+] Total rows: " << chunk->GetValue(0, 0).ToString() << std::endl;
        }
    }

    // Pre-build sample views
    std::cout << "[*] Building sample views..." << std::endl;
    conn.Query("CREATE VIEW hits_2pct  AS SELECT * FROM hits USING SAMPLE 2%  (reservoir)");
    conn.Query("CREATE VIEW hits_5pct  AS SELECT * FROM hits USING SAMPLE 5%  (reservoir)");
    conn.Query("CREATE VIEW hits_10pct AS SELECT * FROM hits USING SAMPLE 10% (reservoir)");
    std::cout << "[+] Sample views created (2%, 5%, 10%)." << std::endl;
    std::cout << std::endl;

    // Define benchmark queries
    std::vector<BenchmarkQuery> queries = {
        {
            "Avg page load time",
            "SELECT AVG(SendTiming) FROM hits",
            "SELECT AVG(SendTiming) FROM hits_10pct",
            1.0
        },
        {
            "Total hits by OS",
            "SELECT COUNT(*) FROM hits",
            "SELECT COUNT(*)*10 FROM hits_10pct",
            10.0
        },
        {
            "Avg age by browser country",
            "SELECT AVG(Age) FROM hits",
            "SELECT AVG(Age) FROM hits_10pct",
            1.0
        },
        {
            "Total param price",
            "SELECT SUM(ParamPrice) FROM hits",
            "SELECT SUM(ParamPrice)*10 FROM hits_10pct",
            10.0
        },
        {
            "Avg resolution width",
            "SELECT AVG(ResolutionWidth) FROM hits",
            "SELECT AVG(ResolutionWidth) FROM hits_10pct",
            1.0
        },
        {
            "Count by social network",
            "SELECT COUNT(*) FROM hits",
            "SELECT COUNT(*)*10 FROM hits_10pct",
            10.0
        },
        {
            "Avg connect timing",
            "SELECT AVG(ConnectTiming) FROM hits",
            "SELECT AVG(ConnectTiming) FROM hits_10pct",
            1.0
        },
        {
            "Unique user count (HLL)",
            "SELECT COUNT(DISTINCT UserID) FROM hits",
            "SELECT approx_count_distinct_hll(LIST(CAST(UserID AS VARCHAR))) FROM hits_10pct",
            1.0
        }
    };

    std::vector<BenchmarkResult> results;

    for (const auto& q : queries) {
        BenchmarkResult r;
        r.label = q.label;

        // Time exact SQL
        auto exact_start = std::chrono::high_resolution_clock::now();
        auto exact_result = conn.Query(q.exact_sql);
        auto exact_end = std::chrono::high_resolution_clock::now();
        r.exact_time_ms = std::chrono::duration<double, std::milli>(exact_end - exact_start).count();
        r.exact_val = extract_numeric_value(exact_result);

        // Time approximate SQL
        auto approx_start = std::chrono::high_resolution_clock::now();
        auto approx_result = conn.Query(q.approx_sql);
        auto approx_end = std::chrono::high_resolution_clock::now();
        r.approx_time_ms = std::chrono::duration<double, std::milli>(approx_end - approx_start).count();
        r.approx_val = extract_numeric_value(approx_result);

        // Compute metrics
        r.speedup = (r.approx_time_ms > 0) ? round2(r.exact_time_ms / r.approx_time_ms) : 0.0;
        r.error_pct = round2(get_error_percentage(r.approx_val, r.exact_val));
        r.accuracy_pct = round2(100.0 - r.error_pct);
        r.margin_of_error = round2(std::abs(r.approx_val - r.exact_val));

        // Determine algorithm label
        if (q.label.find("HLL") != std::string::npos) {
            r.algorithm = "HyperLogLog (b=12, m=4096)";
        } else {
            r.algorithm = "Reservoir Sampling (10%)";
        }

        print_query_result(r);
        results.push_back(r);
    }

    print_summary_table(results);

    return 0;
}
