#define DUCKDB_EXTENSION_MAIN

#include "aproql_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

// OpenSSL linked through vcpkg
#include <openssl/opensslv.h>

namespace duckdb {

inline void AproqlScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "Aproql " + name.GetString() + " 🐥");
	});
}

inline void AproqlOpenSSLVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "Aproql " + name.GetString() + ", my linked OpenSSL version is " +
		                                           OPENSSL_VERSION_TEXT);
	});
}

static void LoadInternal(ExtensionLoader &loader) {
	// Register a scalar function
	auto aproql_scalar_function = ScalarFunction("aproql", {LogicalType::VARCHAR}, LogicalType::VARCHAR, AproqlScalarFun);
	loader.RegisterFunction(aproql_scalar_function);

	// Register another scalar function
	auto aproql_openssl_version_scalar_function = ScalarFunction("aproql_openssl_version", {LogicalType::VARCHAR},
	                                                            LogicalType::VARCHAR, AproqlOpenSSLVersionScalarFun);
	loader.RegisterFunction(aproql_openssl_version_scalar_function);
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
