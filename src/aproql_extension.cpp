#define DUCKDB_EXTENSION_MAIN

#include "aproql_extension.hpp"
#include "aproql_hyperloglog.hpp"
#include "aproql_countmin.hpp"
#include "aproql_tdigest.hpp"
#include "algorithms.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/string_type.hpp"

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
// approx_freq_cms(VARCHAR, VARCHAR) → BIGINT
// AggregateFunction using Count-Min Sketch.
// ---------------------------------------------------------------------------
struct CMSAggState {
	AproqlCountMinSketch *sketch;
};

struct CMSAggOP {
	template <class STATE>
	static void Initialize(STATE &state) {
		state.sketch = nullptr;
	}

	template <class A_TYPE, class B_TYPE, class STATE, class OP>
	static void Operation(STATE &state, A_TYPE val, B_TYPE target, AggregateBinaryInput &) {
		if (!state.sketch) {
			state.sketch = new AproqlCountMinSketch();
			state.sketch->target_val = target.GetString();
		}
		state.sketch->add(val.GetString());
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		if (!source.sketch) return;
		if (!target.sketch) {
			target.sketch = new AproqlCountMinSketch(*source.sketch);
			return;
		}
		target.sketch->merge(*source.sketch);
	}

	template <class RESULT_TYPE, class STATE>
	static void Finalize(STATE &state, RESULT_TYPE &target, AggregateFinalizeData &finalize_data) {
		if (!state.sketch) {
			finalize_data.ReturnNull();
			return;
		}
		target = state.sketch->estimate(state.sketch->target_val);
	}

	template <class STATE>
	static void Destroy(STATE &state, AggregateInputData &) {
		if (state.sketch) {
			delete state.sketch;
		}
	}

	static bool IgnoreNull() { return true; }
};

// ---------------------------------------------------------------------------
// approx_quantile_sketch(DOUBLE, DOUBLE) → DOUBLE
// AggregateFunction using T-Digest
// ---------------------------------------------------------------------------
struct TDigestAggState {
	AproqlTDigest *digest;
	double quantile;
};

struct TDigestAggOP {
	template <class STATE>
	static void Initialize(STATE &state) {
		state.digest = nullptr;
		state.quantile = 0.5;
	}

	template <class A_TYPE, class B_TYPE, class STATE, class OP>
	static void Operation(STATE &state, A_TYPE val, B_TYPE q, AggregateBinaryInput &) {
		if (!state.digest) {
			state.digest = new AproqlTDigest();
			state.quantile = q;
		}
		state.digest->add(val);
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		if (!source.digest) return;
		if (!target.digest) {
			target.digest = new AproqlTDigest(*source.digest);
			target.quantile = source.quantile;
			return;
		}
		target.digest->merge(*source.digest);
	}

	template <class RESULT_TYPE, class STATE>
	static void Finalize(STATE &state, RESULT_TYPE &target, AggregateFinalizeData &finalize_data) {
		if (!state.digest) {
			finalize_data.ReturnNull();
			return;
		}
		target = state.digest->quantile(state.quantile);
	}

	template <class STATE>
	static void Destroy(STATE &state, AggregateInputData &) {
		if (state.digest) {
			delete state.digest;
		}
	}

	static bool IgnoreNull() { return true; }
};

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

	// approx_freq_cms(VARCHAR, VARCHAR) → BIGINT
	auto approx_freq_cms = AggregateFunction::BinaryAggregate<CMSAggState, string_t, string_t, int64_t, CMSAggOP>(
	    LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT);
	approx_freq_cms.name = "approx_freq_cms";
	approx_freq_cms.destructor = AggregateFunction::StateDestroy<CMSAggState, CMSAggOP>;
	loader.RegisterFunction(approx_freq_cms);

	// approx_quantile_sketch(DOUBLE, DOUBLE) → DOUBLE
	auto approx_quantile_sketch = AggregateFunction::BinaryAggregate<TDigestAggState, double, double, double, TDigestAggOP>(
	    LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE);
	approx_quantile_sketch.name = "approx_quantile_sketch";
	approx_quantile_sketch.destructor = AggregateFunction::StateDestroy<TDigestAggState, TDigestAggOP>;
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
