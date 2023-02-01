#include "json_functions.hpp"
#include "json_scan.hpp"
#include "json_structure.hpp"
#include "json_transform.hpp"

namespace duckdb {

void AutoDetect(ClientContext &context, JSONScanData &bind_data, vector<LogicalType> &return_types,
                vector<string> &names) {
	auto original_scan_type = bind_data.type;
	bind_data.type = JSONScanType::SAMPLE; // Set scan type to sample for the auto-detect, we restore it later
	JSONScanGlobalState gstate(context, bind_data);
	JSONScanLocalState lstate(context, gstate);
	ArenaAllocator allocator(BufferAllocator::Get(context));

	// Read for the specified sample size
	JSONStructureNode node;
	Vector string_vector(LogicalType::VARCHAR);
	idx_t read = 0;
	while (read < bind_data.sample_size) {
		allocator.Reset();
		auto count = lstate.ReadNext(gstate);
		if (count == 0) {
			break;
		}
		idx_t i;
		for (i = 0; i < count; i++) {
			if (lstate.objects[i]) {
				JSONStructure::ExtractStructure(lstate.objects[i], node);
			}
			if (++read == bind_data.sample_size) {
				break;
			}
		}
		if (!node.ContainsVarchar()) { // Can't refine non-VARCHAR types
			continue;
		}
		node.InitializeCandidateTypes(bind_data.max_depth);
		node.RefineCandidateTypes(lstate.objects, i, string_vector, allocator);
	}
	bind_data.type = original_scan_type;

	const auto type = JSONStructure::StructureToType(context, node, bind_data.max_depth);
	if (type.id() != LogicalTypeId::STRUCT) {
		return_types.emplace_back(type);
		names.emplace_back("json");
		return;
	}

	const auto &child_types = StructType::GetChildTypes(type);
	return_types.reserve(child_types.size());
	names.reserve(child_types.size());
	for (auto &child_type : child_types) {
		return_types.emplace_back(child_type.second);
		names.emplace_back(child_type.first);
	}

	for (auto &reader : gstate.json_readers) {
		if (reader->IsOpen()) {
			reader->Reset();
		}
	}
	bind_data.stored_readers = std::move(gstate.json_readers);
}

unique_ptr<FunctionData> ReadJSONBind(ClientContext &context, TableFunctionBindInput &input,
                                      vector<LogicalType> &return_types, vector<string> &names) {
	// First bind default params
	auto result = JSONScanData::Bind(context, input);
	auto &bind_data = (JSONScanData &)*result;

	for (auto &kv : input.named_parameters) {
		auto loption = StringUtil::Lower(kv.first);
		if (loption == "columns") {
			auto &child_type = kv.second.type();
			if (child_type.id() != LogicalTypeId::STRUCT) {
				throw BinderException("read_json \"columns\" parameter requires a struct as input");
			}
			auto &struct_children = StructValue::GetChildren(kv.second);
			D_ASSERT(StructType::GetChildCount(child_type) == struct_children.size());
			for (idx_t i = 0; i < struct_children.size(); i++) {
				auto &name = StructType::GetChildName(child_type, i);
				auto &val = struct_children[i];
				names.push_back(name);
				if (val.type().id() != LogicalTypeId::VARCHAR) {
					throw BinderException("read_json \"columns\" parameter type specification must be VARCHAR");
				}
				return_types.emplace_back(TransformStringToLogicalType(StringValue::Get(val), context));
			}
			D_ASSERT(names.size() == return_types.size());
			if (names.empty()) {
				throw BinderException("read_json \"columns\" parameter needs at least one column");
			}
			bind_data.names = names;
		} else if (loption == "auto_detect") {
			bind_data.auto_detect = BooleanValue::Get(kv.second);
		} else if (loption == "sample_size") {
			auto arg = BigIntValue::Get(kv.second);
			if (arg == -1) {
				bind_data.sample_size = NumericLimits<idx_t>::Maximum();
			} else if (arg > 0) {
				bind_data.sample_size = arg;
			} else {
				throw BinderException(
				    "read_json \"sample_size\" parameter must be positive, or -1 to sample the entire file");
			}
		} else if (loption == "maximum_depth") {
			auto arg = BigIntValue::Get(kv.second);
			if (arg == -1) {
				bind_data.max_depth = NumericLimits<idx_t>::Maximum();
			} else {
				bind_data.max_depth = arg;
			}
		}
	}

	if (!bind_data.names.empty()) {
		bind_data.auto_detect = false; // override auto-detect when columns are specified
	} else if (!bind_data.auto_detect) {
		throw BinderException("read_json \"columns\" parameter is required when auto_detect is false");
	}

	if (bind_data.auto_detect) {
		AutoDetect(context, bind_data, return_types, names);
		bind_data.names = names;
	}

	auto &transform_options = bind_data.transform_options;
	transform_options.strict_cast = !bind_data.ignore_errors;
	transform_options.error_duplicate_key = !bind_data.ignore_errors;
	transform_options.error_missing_key = false;
	transform_options.error_unknown_key = bind_data.auto_detect && !bind_data.ignore_errors;

	return result;
}

static void ReadJSONFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &gstate = ((JSONGlobalTableFunctionState &)*data_p.global_state).state;
	auto &lstate = ((JSONLocalTableFunctionState &)*data_p.local_state).state;
	D_ASSERT(output.ColumnCount() == gstate.bind_data.names.size());

	// Fetch next lines
	const auto count = lstate.ReadNext(gstate);
	const auto objects = lstate.objects;

	vector<Vector *> result_vectors;
	result_vectors.reserve(output.ColumnCount());
	for (idx_t col_idx = 0; col_idx < output.ColumnCount(); col_idx++) {
		result_vectors.push_back(&output.data[col_idx]);
	}

	// TODO: if errors occur during transformation, we don't have line number information
	JSONTransform::TransformObject(objects, lstate.GetAllocator(), count, gstate.bind_data.names, result_vectors,
	                               gstate.bind_data.transform_options);
	output.SetCardinality(count);
}

TableFunction GetReadJSONTableFunction(bool list_parameter, shared_ptr<JSONScanInfo> function_info) {
	auto parameter = list_parameter ? LogicalType::LIST(LogicalType::VARCHAR) : LogicalType::VARCHAR;
	TableFunction table_function({parameter}, ReadJSONFunction, ReadJSONBind, JSONGlobalTableFunctionState::Init,
	                             JSONLocalTableFunctionState::Init);

	JSONScan::TableFunctionDefaults(table_function);
	table_function.named_parameters["columns"] = LogicalType::ANY;
	table_function.named_parameters["auto_detect"] = LogicalType::BOOLEAN;
	table_function.named_parameters["sample_size"] = LogicalType::BIGINT;

	table_function.projection_pushdown = true;

	table_function.function_info = std::move(function_info);

	return table_function;
}

TableFunction GetReadJSONAutoTableFunction(bool list_parameter, shared_ptr<JSONScanInfo> function_info) {
	auto table_function = GetReadJSONTableFunction(list_parameter, std::move(function_info));
	table_function.named_parameters["maximum_depth"] = LogicalType::BIGINT;
	return table_function;
}

CreateTableFunctionInfo JSONFunctions::GetReadJSONFunction() {
	TableFunctionSet function_set("read_json");
	auto function_info = make_shared<JSONScanInfo>(JSONScanType::READ_JSON, JSONFormat::UNSTRUCTURED, false);
	function_set.AddFunction(GetReadJSONTableFunction(false, function_info));
	function_set.AddFunction(GetReadJSONTableFunction(true, function_info));
	return CreateTableFunctionInfo(function_set);
}

CreateTableFunctionInfo JSONFunctions::GetReadNDJSONFunction() {
	TableFunctionSet function_set("read_ndjson");
	auto function_info = make_shared<JSONScanInfo>(JSONScanType::READ_JSON, JSONFormat::NEWLINE_DELIMITED, false);
	function_set.AddFunction(GetReadJSONTableFunction(false, function_info));
	function_set.AddFunction(GetReadJSONTableFunction(true, function_info));
	return CreateTableFunctionInfo(function_set);
}

CreateTableFunctionInfo JSONFunctions::GetReadJSONAutoFunction() {
	TableFunctionSet function_set("read_json_auto");
	auto function_info = make_shared<JSONScanInfo>(JSONScanType::READ_JSON, JSONFormat::AUTO_DETECT, true);
	function_set.AddFunction(GetReadJSONAutoTableFunction(false, function_info));
	function_set.AddFunction(GetReadJSONAutoTableFunction(true, function_info));
	return CreateTableFunctionInfo(function_set);
}

CreateTableFunctionInfo JSONFunctions::GetReadNDJSONAutoFunction() {
	TableFunctionSet function_set("read_ndjson_auto");
	auto function_info = make_shared<JSONScanInfo>(JSONScanType::READ_JSON, JSONFormat::NEWLINE_DELIMITED, true);
	function_set.AddFunction(GetReadJSONAutoTableFunction(false, function_info));
	function_set.AddFunction(GetReadJSONAutoTableFunction(true, function_info));
	return CreateTableFunctionInfo(function_set);
}

} // namespace duckdb
