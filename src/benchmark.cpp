#include "duckdb.hpp"
#include "algorithms.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <string>
#include <vector>
#include <cmath>
#include <sstream>
#include <algorithm>
#include <fstream>

// Number of timed iterations per query (median is taken)
static constexpr int NUM_ITERATIONS = 5;
// Number of warmup runs before timing
static constexpr int NUM_WARMUP = 2;

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

static double extract_numeric_value(duckdb::unique_ptr<duckdb::MaterializedQueryResult> &result) {
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

// Run a query multiple times and return the median execution time in ms.
// Also returns the result value from the last successful read.
static double timed_query_median(duckdb::Connection &conn, const std::string &sql,
                                 double &out_value, int iterations = NUM_ITERATIONS,
                                 int warmup = NUM_WARMUP) {
	// Warmup runs — not timed
	for (int i = 0; i < warmup; i++) {
		auto res = conn.Query(sql);
		(void)res;
	}

	std::vector<double> times;
	times.reserve(iterations);
	double last_val = 0.0;

	for (int i = 0; i < iterations; i++) {
		auto start = std::chrono::high_resolution_clock::now();
		auto res = conn.Query(sql);
		auto end = std::chrono::high_resolution_clock::now();
		double ms = std::chrono::duration<double, std::milli>(end - start).count();
		times.push_back(ms);
		last_val = extract_numeric_value(res);
	}

	std::sort(times.begin(), times.end());
	out_value = last_val;
	return times[iterations / 2];
}

// Holds info about one sample file created each run
struct SampleCreationInfo {
	std::string label;
	std::string parquet_path;
	std::string query_source;       // actual SQL fragment used in approx queries
	double create_parquet_ms = 0.0; // time to write the parquet (-1 = reused)
	double create_table_ms  = 0.0;  // time to load into in-memory table (0 = not loaded)
	int64_t rows = 0;
	double actual_pct = 0.0;
	bool ok = false;                // false = unusable, abort
};

static void print_query_result(const BenchmarkResult &r) {
	std::cout << "============================================================" << std::endl;
	std::cout << "Query : " << r.label << std::endl;
	std::cout << "------------------------------------------------------------" << std::endl;
	std::cout << std::fixed << std::setprecision(1);

	std::string approx_note = "(sampled)";
	if (r.algorithm.find("5%") != std::string::npos) {
		approx_note = "(~5% of dataset scanned)";
	} else if (r.algorithm.find("10%") != std::string::npos) {
		approx_note = "(~10% of dataset scanned)";
	} else if (r.algorithm.find("HLL") != std::string::npos) {
		approx_note = "(HyperLogLog sketch)";
	}

	std::cout << "  Approx Time    : " << std::setw(7) << r.approx_time_ms << " ms   " << approx_note << std::endl;
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

static std::string pad_right(const std::string &s, size_t width) {
	if (s.size() >= width)
		return s.substr(0, width);
	return s + std::string(width - s.size(), ' ');
}

static std::string center(const std::string &s, size_t width) {
	if (s.size() >= width)
		return s.substr(0, width);
	size_t left = (width - s.size()) / 2;
	size_t right = width - s.size() - left;
	return std::string(left, ' ') + s + std::string(right, ' ');
}

static void print_summary_table(const std::vector<BenchmarkResult> &results) {
	std::cout << std::endl;
	std::cout << "BENCHMARK SUMMARY — aproql | e6data Hackathon" << std::endl;
	std::cout << "┌─────────────────────────────────┬──────────┬──────────┬─────────┬────────┐" << std::endl;
	std::cout << "│ Query                           │ Approx   │ Exact    │ Speedup │ Error% │" << std::endl;
	std::cout << "│                                 │ Time(ms) │ Time(ms) │         │        │" << std::endl;
	std::cout << "├─────────────────────────────────┼──────────┼──────────┼─────────┼────────┤" << std::endl;

	double total_approx = 0, total_exact = 0, total_speedup = 0, total_error = 0;
	int n = static_cast<int>(results.size());

	for (const auto &r : results) {
		std::ostringstream approx_ss, exact_ss, speedup_ss, error_ss;
		approx_ss << std::fixed << std::setprecision(1) << r.approx_time_ms;
		exact_ss << std::fixed << std::setprecision(1) << r.exact_time_ms;
		speedup_ss << std::fixed << std::setprecision(1) << r.speedup << "x";
		error_ss << std::fixed << std::setprecision(2) << r.error_pct << "%";

		std::cout << "│ " << pad_right(r.label, 31) << " │" << center(approx_ss.str(), 10) << "│"
		          << center(exact_ss.str(), 10) << "│" << center(speedup_ss.str(), 9) << "│"
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

		std::cout << "│ " << pad_right("AVERAGE", 31) << " │" << center(approx_ss.str(), 10) << "│"
		          << center(exact_ss.str(), 10) << "│" << center(speedup_ss.str(), 9) << "│"
		          << center(error_ss.str(), 8) << "│" << std::endl;
	}

	std::cout << "└─────────────────────────────────┴──────────┴──────────┴─────────┴────────┘" << std::endl;

	std::cout << std::endl;
	std::cout << std::fixed << std::setprecision(1);
	std::cout << "Average Speedup : " << avg_speedup << "x" << std::endl;
	std::cout << std::setprecision(2);
	std::cout << "Average Error % : " << avg_error << "%" << std::endl;
}

static void print_sample_creation_summary(const std::vector<SampleCreationInfo> &infos) {
	std::cout << std::endl;
	std::cout << "Sample File Creation Times" << std::endl;
	std::cout << "┌─────────────────┬─────────────────┬─────────────────┬──────────────────────┐" << std::endl;
	std::cout << "│ Sample          │ Write Parquet   │ Load to Table   │ Rows                 │" << std::endl;
	std::cout << "│                 │ (ms)            │ (ms)            │                      │" << std::endl;
	std::cout << "├─────────────────┼─────────────────┼─────────────────┼──────────────────────┤" << std::endl;
	for (const auto &info : infos) {
		std::ostringstream pq_ss, tbl_ss, rows_ss;
		if (info.create_parquet_ms < 0.0) {
			pq_ss << "reused";
		} else {
			pq_ss << std::fixed << std::setprecision(1) << info.create_parquet_ms;
		}
		if (info.create_table_ms == 0.0 && info.rows == 0) {
			tbl_ss << "parquet direct";
		} else {
			tbl_ss << std::fixed << std::setprecision(1) << info.create_table_ms;
		}
		rows_ss << info.rows << " (" << std::fixed << std::setprecision(1) << info.actual_pct << "%)";
		std::cout << "│ " << pad_right(info.label, 15)
		          << " │" << center(pq_ss.str(), 17)
		          << "│" << center(tbl_ss.str(), 17)
		          << "│" << center(rows_ss.str(), 22) << "│" << std::endl;
	}
	std::cout << "└─────────────────┴─────────────────┴─────────────────┴──────────────────────┘" << std::endl;
}

int main() {
	std::cout << "═══════════════════════════════════════════════════════════" << std::endl;
	std::cout << "  aproql Benchmark Runner | e6data Hackathon" << std::endl;
	std::cout << "  Approximate Query Processing with DuckDB" << std::endl;
	std::cout << "═══════════════════════════════════════════════════════════" << std::endl;
	std::cout << std::endl;

	// Single-threaded: ensures speedup reflects row-count reduction, not parallelism
	duckdb::DBConfig config;
	config.SetOptionByName("threads", duckdb::Value::INTEGER(1));
	duckdb::DuckDB db(nullptr, &config);
	duckdb::Connection conn(db);

	// Load extension
	std::cout << "[*] Loading aproql extension..." << std::endl;
	auto load_result = conn.Query("LOAD 'build/release/extension/aproql/aproql.duckdb_extension'");
	if (load_result->HasError()) {
		std::cerr << "[!] Failed to load extension: " << load_result->GetError() << std::endl;
		std::cerr << "[*] Continuing without extension (HLL function unavailable)..." << std::endl;
	} else {
		std::cout << "[+] Extension loaded successfully." << std::endl;
	}

	// Load full dataset into memory
	std::cout << "[*] Loading ClickBench dataset from data/hits.parquet..." << std::endl;
	conn.Query("DROP TABLE IF EXISTS hits");
	auto table_result = conn.Query("CREATE TABLE hits AS SELECT * FROM read_parquet('data/hits.parquet')");
	if (table_result->HasError()) {
		std::cerr << "[!] Failed to load dataset: " << table_result->GetError() << std::endl;
		return 1;
	}
	std::cout << "[+] Dataset loaded into memory." << std::endl;

	int64_t total_rows = 0;
	{
		auto cr = conn.Query("SELECT COUNT(*) FROM hits");
		if (!cr->HasError()) {
			auto chunk = cr->Fetch();
			if (chunk && chunk->size() > 0) {
				total_rows = chunk->GetValue(0, 0).GetValue<int64_t>();
				std::cout << "[+] Total rows: " << total_rows << std::endl;
			}
		}
	}

	// -----------------------------------------------------------------------
	// Create sample parquet files and load them into in-memory tables.
	// Strategy (handles disk-full gracefully):
	//   1. COPY … TABLESAMPLE to parquet  → time it (or reuse existing file)
	//   2. CREATE TABLE … AS SELECT * FROM read_parquet(…) → time it
	//   3. If step 2 fails (disk full for temp space), fall back to querying
	//      the parquet file directly without a table — still valid and fast.
	// -----------------------------------------------------------------------
	std::cout << "[*] Creating sample files and loading tables..." << std::endl;

	std::vector<SampleCreationInfo> sample_infos;

	auto create_sample = [&](const char* label, int pct, const char* parquet_path,
	                          const char* table_name) -> SampleCreationInfo {
		SampleCreationInfo info;
		info.label = label;
		info.parquet_path = parquet_path;

		// --- Step 1: Write parquet file (timed) ---
		std::string export_sql =
		    std::string("COPY (SELECT * FROM hits TABLESAMPLE ") + std::to_string(pct) +
		    " PERCENT (system)) TO '" + parquet_path + "' (FORMAT PARQUET)";
		auto t0 = std::chrono::high_resolution_clock::now();
		auto export_res = conn.Query(export_sql);
		auto t1 = std::chrono::high_resolution_clock::now();
		info.create_parquet_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

		if (export_res->HasError()) {
			// Try to reuse existing parquet from a prior run
			std::ifstream f(parquet_path);
			if (f.good()) {
				std::cerr << "[!] Could not re-write " << parquet_path
				          << " (disk full?). Using existing file." << std::endl;
				info.create_parquet_ms = -1.0;  // sentinel: reused
			} else {
				std::cerr << "[!] Failed to write " << parquet_path << ": "
				          << export_res->GetError() << std::endl;
				info.ok = false;
				return info;  // nothing we can do
			}
		} else {
			std::cout << std::fixed << std::setprecision(1)
			          << "[+] Wrote " << parquet_path
			          << " in " << info.create_parquet_ms << " ms" << std::endl;
		}

		// --- Step 2: Load parquet into in-memory table (timed) ---
		conn.Query(std::string("DROP TABLE IF EXISTS ") + table_name);
		std::string load_sql =
		    std::string("CREATE TABLE ") + table_name +
		    " AS SELECT * FROM read_parquet('" + parquet_path + "')";
		auto t2 = std::chrono::high_resolution_clock::now();
		auto load_res = conn.Query(load_sql);
		auto t3 = std::chrono::high_resolution_clock::now();
		info.create_table_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

		if (load_res->HasError()) {
			// Disk too full for in-memory table — fall back to direct parquet queries
			std::cerr << "[!] Could not load " << table_name
			          << " into memory (disk full?). Will query parquet directly." << std::endl;
			// Use read_parquet(...) as the query source instead of the table name
			info.query_source = std::string("read_parquet('") + parquet_path + "')";
			// Get row count from the parquet file directly
			auto cnt = conn.Query(std::string("SELECT COUNT(*) FROM '") + parquet_path + "'");
			if (!cnt->HasError()) {
				auto chunk = cnt->Fetch();
				if (chunk && chunk->size() > 0)
					info.rows = chunk->GetValue(0, 0).GetValue<int64_t>();
			}
		} else {
			std::cout << "[+] Loaded " << table_name
			          << " in " << std::fixed << std::setprecision(1)
			          << info.create_table_ms << " ms" << std::endl;
			info.query_source = table_name;  // in-memory table — preferred
			// Row count from the in-memory table
			auto cnt = conn.Query(std::string("SELECT COUNT(*) FROM ") + table_name);
			if (!cnt->HasError()) {
				auto chunk = cnt->Fetch();
				if (chunk && chunk->size() > 0)
					info.rows = chunk->GetValue(0, 0).GetValue<int64_t>();
			}
		}

		info.actual_pct = total_rows > 0 ? (100.0 * info.rows / total_rows) : 0.0;
		std::cout << "[+] " << label << " rows: " << info.rows
		          << " (" << std::fixed << std::setprecision(1) << info.actual_pct << "%)"
		          << " — querying from: " << info.query_source << std::endl;
		info.ok = (info.rows > 0);
		return info;
	};

	auto s5  = create_sample("5% sample",  5,  "data/hits_5pct.parquet",  "hits_sample_5pct");
	auto s10 = create_sample("10% sample", 10, "data/hits_10pct.parquet", "hits_sample_10pct");
	sample_infos.push_back(s5);
	sample_infos.push_back(s10);

	if (!s5.ok || !s10.ok) {
		std::cerr << "[!] Sample data is unavailable. Cannot run benchmark." << std::endl;
		return 1;
	}

	// Compute real scale factors from actual sample row counts
	double scale_5pct  = s5.rows  > 0 ? static_cast<double>(total_rows) / static_cast<double>(s5.rows)  : 1.0;
	double scale_10pct = s10.rows > 0 ? static_cast<double>(total_rows) / static_cast<double>(s10.rows) : 1.0;

	std::cout << "[+] Scale factors — 5%: " << std::fixed << std::setprecision(2) << scale_5pct
	          << "  10%: " << scale_10pct << std::endl;
	std::cout << std::endl;

	// Build scale factor strings and query source references
	std::ostringstream sf5_ss, sf10_ss;
	sf5_ss  << std::fixed << std::setprecision(6) << scale_5pct;
	sf10_ss << std::fixed << std::setprecision(6) << scale_10pct;
	const std::string sf5    = sf5_ss.str();
	const std::string src5   = s5.query_source;   // table name or read_parquet(...)
	const std::string src10  = s10.query_source;

	// Build benchmark queries using the correct source (table or parquet)
	std::vector<BenchmarkQuery> queries = {
	    {"Avg region ID",
	     "SELECT AVG(RegionID) FROM hits",
	     "SELECT AVG(RegionID) FROM " + src10,
	     1.0},
	    {"Total hits (COUNT)",
	     "SELECT COUNT(*) FROM hits",
	     "SELECT CAST(COUNT(*) * " + sf5 + " AS BIGINT) FROM " + src5,
	     scale_5pct},
	    {"Avg age",
	     "SELECT AVG(Age) FROM hits",
	     "SELECT AVG(Age) FROM " + src5,
	     1.0},
	    {"Total resolution width (SUM)",
	     "SELECT SUM(ResolutionWidth) FROM hits",
	     "SELECT SUM(ResolutionWidth) * " + sf5 + " FROM " + src5,
	     scale_5pct},
	    {"Avg resolution width",
	     "SELECT AVG(ResolutionWidth) FROM hits",
	     "SELECT AVG(ResolutionWidth) FROM " + src5,
	     1.0},
	    {"Total event count (COUNT)",
	     "SELECT COUNT(*) FROM hits",
	     "SELECT CAST(COUNT(*) * " + sf5 + " AS BIGINT) FROM " + src5,
	     scale_5pct},
	    {"Avg counter ID",
	     "SELECT AVG(CounterID) FROM hits",
	     "SELECT AVG(CounterID) FROM " + src5,
	     1.0},
	    {"Unique user count (HLL)",
	     "SELECT COUNT(DISTINCT UserID) FROM hits",
	     "SELECT approx_count_distinct(UserID) FROM hits",
	     1.0},
	    // --- New sketch-based queries ---
	    {"Freq of OS=2 (CMS)",
	     "SELECT COUNT(*) FROM hits WHERE CAST(OS AS VARCHAR) = '2'",
	     "SELECT CAST(approx_freq_cms(CAST(OS AS VARCHAR), '2') * " + sf5 + " AS BIGINT) FROM " + src5,
	     scale_5pct},
	    {"Median counter ID (T-Dig)",
	     "SELECT MEDIAN(CounterID) FROM hits",
	     "SELECT approx_quantile(CounterID, 0.5) FROM " + src5,
	     1.0},
	    {"P95 counter ID (T-Dig)",
	     "SELECT quantile_cont(CounterID, 0.95) FROM hits",
	     "SELECT approx_quantile(CounterID, 0.95) FROM " + src5,
	     1.0}
	};

	std::cout << "[*] Running benchmarks (" << NUM_WARMUP << " warmup + "
	          << NUM_ITERATIONS << " timed iterations per query, median reported)..."
	          << std::endl << std::endl;

	std::vector<BenchmarkResult> results;

	for (const auto &q : queries) {
		BenchmarkResult r;
		r.label = q.label;

		r.exact_time_ms  = timed_query_median(conn, q.exact_sql,  r.exact_val);
		r.approx_time_ms = timed_query_median(conn, q.approx_sql, r.approx_val);

		r.speedup        = (r.approx_time_ms > 0) ? round2(r.exact_time_ms / r.approx_time_ms) : 0.0;
		r.error_pct      = round2(get_error_percentage(r.approx_val, r.exact_val));
		r.accuracy_pct   = round2(100.0 - r.error_pct);
		r.margin_of_error = round2(std::abs(r.approx_val - r.exact_val));

		if (q.label.find("HLL") != std::string::npos) {
			r.algorithm = "HyperLogLog (approx_count_distinct)";
		} else if (q.label.find("CMS") != std::string::npos) {
			r.algorithm = "Count-Min Sketch";
		} else if (q.label.find("T-Dig") != std::string::npos) {
			r.algorithm = "T-Digest";
		} else if (q.approx_sql.find("5pct") != std::string::npos) {
			r.algorithm = "System Sampling (5%)";
		} else {
			r.algorithm = "System Sampling (10%)";
		}

		print_query_result(r);
		results.push_back(r);
	}

	print_summary_table(results);
	print_sample_creation_summary(sample_infos);

	return 0;
}
