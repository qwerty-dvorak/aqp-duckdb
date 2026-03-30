#define DUCKDB_EXTENSION_MAIN

#include "aproql_extension.hpp"
#include "aproql_hyperloglog.hpp"
#include "aproql_countmin.hpp"
#include "aproql_tdigest.hpp"
#include "algorithms.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb {

// approx_avg_list(DOUBLE[], DOUBLE) → DOUBLE
// Computes the mean of a list of doubles
static void ApproxAvgListFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &list_vec = args.data[0];
	// fraction arg unused for avg computation
	auto count = args.size();

	UnifiedVectorFormat list_data;
	list_vec.ToUnifiedFormat(count, list_data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<double>(result);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto idx = list_data.sel->get_index(i);
		if (!list_data.validity.RowIsValid(idx)) {
			result_validity.SetInvalid(i);
			continue;
		}
		// Get the list entry
		auto list_entries = UnifiedVectorFormat::GetData<list_entry_t>(list_data);
		auto &entry = list_entries[idx];
		auto &child = ListVector::GetEntry(list_vec);

		if (entry.length == 0) {
			result_validity.SetInvalid(i);
			continue;
		}

		double sum = 0.0;
		idx_t valid_count = 0;
		for (idx_t j = 0; j < entry.length; j++) {
			auto child_val = child.GetValue(entry.offset + j);
			if (!child_val.IsNull()) {
				sum += child_val.GetValue<double>();
				valid_count++;
			}
		}

		if (valid_count == 0) {
			result_validity.SetInvalid(i);
		} else {
			result_data[i] = sum / static_cast<double>(valid_count);
		}
	}
}

// approx_sum_list(DOUBLE[], DOUBLE) → DOUBLE
// Computes sum of list, divides by fraction to scale back to population estimate
static void ApproxSumListFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &list_vec = args.data[0];
	auto &frac_vec = args.data[1];
	auto count = args.size();

	UnifiedVectorFormat list_data;
	list_vec.ToUnifiedFormat(count, list_data);

	UnifiedVectorFormat frac_data;
	frac_vec.ToUnifiedFormat(count, frac_data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<double>(result);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto list_idx = list_data.sel->get_index(i);
		auto frac_idx = frac_data.sel->get_index(i);

		if (!list_data.validity.RowIsValid(list_idx) || !frac_data.validity.RowIsValid(frac_idx)) {
			result_validity.SetInvalid(i);
			continue;
		}

		auto list_entries = UnifiedVectorFormat::GetData<list_entry_t>(list_data);
		auto &entry = list_entries[list_idx];
		auto &child = ListVector::GetEntry(list_vec);
		double fraction = UnifiedVectorFormat::GetData<double>(frac_data)[frac_idx];

		double sum = 0.0;
		for (idx_t j = 0; j < entry.length; j++) {
			auto child_val = child.GetValue(entry.offset + j);
			if (!child_val.IsNull()) {
				sum += child_val.GetValue<double>();
			}
		}

		if (fraction <= 0.0) {
			result_validity.SetInvalid(i);
		} else {
			result_data[i] = sum / fraction;
		}
	}
}

// approx_count_list(DOUBLE[], DOUBLE) → BIGINT
// Returns (int64_t)(list.size() / fraction) as population count estimate
static void ApproxCountListFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &list_vec = args.data[0];
	auto &frac_vec = args.data[1];
	auto count = args.size();

	UnifiedVectorFormat list_data;
	list_vec.ToUnifiedFormat(count, list_data);

	UnifiedVectorFormat frac_data;
	frac_vec.ToUnifiedFormat(count, frac_data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<int64_t>(result);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto list_idx = list_data.sel->get_index(i);
		auto frac_idx = frac_data.sel->get_index(i);

		if (!list_data.validity.RowIsValid(list_idx) || !frac_data.validity.RowIsValid(frac_idx)) {
			result_validity.SetInvalid(i);
			continue;
		}

		auto list_entries = UnifiedVectorFormat::GetData<list_entry_t>(list_data);
		auto &entry = list_entries[list_idx];
		double fraction = UnifiedVectorFormat::GetData<double>(frac_data)[frac_idx];

		if (fraction <= 0.0) {
			result_validity.SetInvalid(i);
		} else {
			result_data[i] = static_cast<int64_t>(static_cast<double>(entry.length) / fraction);
		}
	}
}

// approx_count_distinct_hll(VARCHAR[]) → BIGINT
// Feeds each string into a HyperLogLog instance and returns the estimate
static void ApproxCountDistinctHLLFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &list_vec = args.data[0];
	auto count = args.size();

	UnifiedVectorFormat list_data;
	list_vec.ToUnifiedFormat(count, list_data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<int64_t>(result);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto idx = list_data.sel->get_index(i);
		if (!list_data.validity.RowIsValid(idx)) {
			result_validity.SetInvalid(i);
			continue;
		}

		auto list_entries = UnifiedVectorFormat::GetData<list_entry_t>(list_data);
		auto &entry = list_entries[idx];
		auto &child = ListVector::GetEntry(list_vec);

		AproqlHyperLogLog hll;
		for (idx_t j = 0; j < entry.length; j++) {
			auto child_val = child.GetValue(entry.offset + j);
			if (!child_val.IsNull()) {
				hll.add(child_val.ToString());
			}
		}

		result_data[i] = static_cast<int64_t>(hll.estimate());
	}
}

// ---------------------------------------------------------------------------
// approx_freq_cms(VARCHAR[], VARCHAR) → BIGINT
// Builds a Count-Min Sketch from the first argument (list of strings) and
// returns the estimated frequency of the second argument (target string).
// ---------------------------------------------------------------------------
static void ApproxFreqCMSFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &list_vec   = args.data[0];
	auto &target_vec = args.data[1];
	auto count       = args.size();

	UnifiedVectorFormat list_data;
	list_vec.ToUnifiedFormat(count, list_data);

	UnifiedVectorFormat target_data;
	target_vec.ToUnifiedFormat(count, target_data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data     = FlatVector::GetData<int64_t>(result);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto list_idx   = list_data.sel->get_index(i);
		auto target_idx = target_data.sel->get_index(i);

		// NULL guard: list or target is NULL → result is NULL
		if (!list_data.validity.RowIsValid(list_idx) ||
		    !target_data.validity.RowIsValid(target_idx)) {
			result_validity.SetInvalid(i);
			continue;
		}

		auto list_entries = UnifiedVectorFormat::GetData<list_entry_t>(list_data);
		auto &entry = list_entries[list_idx];
		auto &child = ListVector::GetEntry(list_vec);

		// Empty list → NULL
		if (entry.length == 0) {
			result_validity.SetInvalid(i);
			continue;
		}

		// Get the target string
		auto target_val = target_vec.GetValue(i);
		std::string target_str = target_val.ToString();

		// Build sketch from the list entries
		AproqlCountMinSketch cms;
		for (idx_t j = 0; j < entry.length; j++) {
			auto child_val = child.GetValue(entry.offset + j);
			if (!child_val.IsNull()) {
				cms.add(child_val.ToString());
			}
		}

		result_data[i] = cms.estimate(target_str);
	}
}

// ---------------------------------------------------------------------------
// approx_quantile_sketch(DOUBLE[], DOUBLE) → DOUBLE
// Builds a T-Digest from the first argument (list of doubles) and returns
// the estimated value at the quantile specified by the second argument.
// ---------------------------------------------------------------------------
static void ApproxQuantileSketchFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &list_vec = args.data[0];
	auto &q_vec    = args.data[1];
	auto count     = args.size();

	UnifiedVectorFormat list_data;
	list_vec.ToUnifiedFormat(count, list_data);

	UnifiedVectorFormat q_data;
	q_vec.ToUnifiedFormat(count, q_data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data     = FlatVector::GetData<double>(result);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto list_idx = list_data.sel->get_index(i);
		auto q_idx    = q_data.sel->get_index(i);

		// NULL guard
		if (!list_data.validity.RowIsValid(list_idx) ||
		    !q_data.validity.RowIsValid(q_idx)) {
			result_validity.SetInvalid(i);
			continue;
		}

		auto list_entries = UnifiedVectorFormat::GetData<list_entry_t>(list_data);
		auto &entry = list_entries[list_idx];
		auto &child = ListVector::GetEntry(list_vec);

		// Empty list → NULL
		if (entry.length == 0) {
			result_validity.SetInvalid(i);
			continue;
		}

		double q = UnifiedVectorFormat::GetData<double>(q_data)[q_idx];

		// Quantile must be in [0, 1]
		if (q < 0.0 || q > 1.0) {
			result_validity.SetInvalid(i);
			continue;
		}

		// Build T-Digest from the list entries
		AproqlTDigest td;
		for (idx_t j = 0; j < entry.length; j++) {
			auto child_val = child.GetValue(entry.offset + j);
			if (!child_val.IsNull()) {
				td.add(child_val.GetValue<double>());
			}
		}

		result_data[i] = td.quantile(q);
	}
}

static void LoadInternal(ExtensionLoader &loader) {
	// approx_avg_list(DOUBLE[], DOUBLE) → DOUBLE
	auto approx_avg_list = ScalarFunction(
	    "approx_avg_list",
	    {LogicalType::LIST(LogicalType::DOUBLE), LogicalType::DOUBLE},
	    LogicalType::DOUBLE,
	    ApproxAvgListFun);
	loader.RegisterFunction(approx_avg_list);

	// approx_sum_list(DOUBLE[], DOUBLE) → DOUBLE
	auto approx_sum_list = ScalarFunction(
	    "approx_sum_list",
	    {LogicalType::LIST(LogicalType::DOUBLE), LogicalType::DOUBLE},
	    LogicalType::DOUBLE,
	    ApproxSumListFun);
	loader.RegisterFunction(approx_sum_list);

	// approx_count_list(DOUBLE[], DOUBLE) → BIGINT
	auto approx_count_list = ScalarFunction(
	    "approx_count_list",
	    {LogicalType::LIST(LogicalType::DOUBLE), LogicalType::DOUBLE},
	    LogicalType::BIGINT,
	    ApproxCountListFun);
	loader.RegisterFunction(approx_count_list);

	// approx_count_distinct_hll(VARCHAR[]) → BIGINT
	auto approx_count_distinct_hll = ScalarFunction(
	    "approx_count_distinct_hll",
	    {LogicalType::LIST(LogicalType::VARCHAR)},
	    LogicalType::BIGINT,
	    ApproxCountDistinctHLLFun);
	loader.RegisterFunction(approx_count_distinct_hll);

	// approx_freq_cms(VARCHAR[], VARCHAR) → BIGINT
	auto approx_freq_cms = ScalarFunction(
	    "approx_freq_cms",
	    {LogicalType::LIST(LogicalType::VARCHAR), LogicalType::VARCHAR},
	    LogicalType::BIGINT,
	    ApproxFreqCMSFun);
	loader.RegisterFunction(approx_freq_cms);

	// approx_quantile_sketch(DOUBLE[], DOUBLE) → DOUBLE
	auto approx_quantile_sketch = ScalarFunction(
	    "approx_quantile_sketch",
	    {LogicalType::LIST(LogicalType::DOUBLE), LogicalType::DOUBLE},
	    LogicalType::DOUBLE,
	    ApproxQuantileSketchFun);
	loader.RegisterFunction(approx_quantile_sketch);
}

void AproqlExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string AproqlExtension::Name() {
	return "aproql";
}

std::string AproqlExtension::Version() const {
#ifdef EXT_VERSION_APROQL
	return EXT_VERSION_APROQL;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(aproql, loader) {
	duckdb::LoadInternal(loader);
}
}
