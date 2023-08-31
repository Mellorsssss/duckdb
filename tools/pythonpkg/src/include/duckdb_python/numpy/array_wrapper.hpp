//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb_python/array_wrapper.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb_python/pybind11/pybind_wrapper.hpp"
#include "duckdb_python/numpy/raw_array_wrapper.hpp"
#include "duckdb.hpp"

namespace duckdb {

struct ArrayWrapper {
	explicit ArrayWrapper(const LogicalType &type, const ClientProperties &client_properties, bool pandas = false);

	unique_ptr<RawArrayWrapper> data;
	unique_ptr<RawArrayWrapper> mask;
	bool requires_mask;
	const ClientProperties client_properties;
	bool pandas;

public:
	void Initialize(idx_t capacity);
	void Resize(idx_t new_capacity);
	void Append(idx_t current_offset, Vector &input, idx_t count);
	py::object ToArray(idx_t count) const;
};

} // namespace duckdb
