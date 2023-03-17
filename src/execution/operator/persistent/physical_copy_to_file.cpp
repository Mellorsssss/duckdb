#include "duckdb/execution/operator/persistent/physical_copy_to_file.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/hive_partitioning.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/string_util.hpp"

#include <algorithm>

namespace duckdb {

class CopyToFunctionGlobalState : public GlobalSinkState {
public:
	explicit CopyToFunctionGlobalState(unique_ptr<GlobalFunctionData> global_state)
	    : rows_copied(0), last_file_offset(0), global_state(std::move(global_state)) {
	}
	mutex lock;
	idx_t rows_copied;
	idx_t last_file_offset;
	unique_ptr<GlobalFunctionData> global_state;

	//! shared state for HivePartitionedColumnData
	shared_ptr<GlobalHivePartitionState> partition_state;
};

class CopyToFunctionLocalState : public LocalSinkState {
public:
	explicit CopyToFunctionLocalState(unique_ptr<LocalFunctionData> local_state)
	    : local_state(std::move(local_state)), writer_offset(0) {
	}
	unique_ptr<GlobalFunctionData> global_state;
	unique_ptr<LocalFunctionData> local_state;

	//! Buffers the tuples in partitions before writing
	unique_ptr<HivePartitionedColumnData> part_buffer;
	unique_ptr<PartitionedColumnDataAppendState> part_buffer_append_state;

	idx_t writer_offset;
};

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//

string PhysicalCopyToFile::CreateFilename(const FileSystem &fs, const string &path, const string &extension,
                                          idx_t writer_offset) const {
	string result(fileformat);
	string replacement;
	if (use_uuid_format) {
		replacement = UUID::ToString(UUID::GenerateRandomUUID());
	} else {
		replacement = to_string(writer_offset);
	}
	// assert(format_position != string::npos);
	result.insert(format_position, replacement);
	return fs.JoinPath(path, result + "." + extension);
}

void MoveTmpFile(ClientContext &context, const string &tmp_file_path) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto file_path = tmp_file_path.substr(0, tmp_file_path.length() - 4);
	if (fs.FileExists(file_path)) {
		fs.RemoveFile(file_path);
	}
	fs.MoveFile(tmp_file_path, file_path);
}

PhysicalCopyToFile::PhysicalCopyToFile(vector<LogicalType> types, CopyFunction function_p,
                                       unique_ptr<FunctionData> bind_data, idx_t estimated_cardinality)
    : PhysicalOperator(PhysicalOperatorType::COPY_TO_FILE, std::move(types), estimated_cardinality),
      function(std::move(function_p)), bind_data(std::move(bind_data)), use_uuid_format(false), parallel(false) {
}

SinkResultType PhysicalCopyToFile::Sink(ExecutionContext &context, GlobalSinkState &gstate, LocalSinkState &lstate,
                                        DataChunk &input) const {
	auto &g = (CopyToFunctionGlobalState &)gstate;
	auto &l = (CopyToFunctionLocalState &)lstate;

	if (partition_output) {
		l.part_buffer->Append(*l.part_buffer_append_state, input);
		return SinkResultType::NEED_MORE_INPUT;
	}

	{
		lock_guard<mutex> glock(g.lock);
		g.rows_copied += input.size();
	}
	function.copy_to_sink(context, *bind_data, per_thread_output ? *l.global_state : *g.global_state, *l.local_state,
	                      input);
	return SinkResultType::NEED_MORE_INPUT;
}

static void CreateDir(const string &dir_path, FileSystem &fs) {
	if (!fs.DirectoryExists(dir_path)) {
		fs.CreateDirectory(dir_path);
	}
}

static string CreateDirRecursive(const vector<idx_t> &cols, const vector<string> &names, const vector<Value> &values,
                                 string path, FileSystem &fs) {
	CreateDir(path, fs);

	for (idx_t i = 0; i < cols.size(); i++) {
		const auto &partition_col_name = names[cols[i]];
		const auto &partition_value = values[i];
		string p_dir = partition_col_name + "=" + partition_value.ToString();
		path = fs.JoinPath(path, p_dir);
		CreateDir(path, fs);
	}

	return path;
}

void PhysicalCopyToFile::Combine(ExecutionContext &context, GlobalSinkState &gstate, LocalSinkState &lstate) const {
	auto &g = (CopyToFunctionGlobalState &)gstate;
	auto &l = (CopyToFunctionLocalState &)lstate;

	if (partition_output) {
		auto &fs = FileSystem::GetFileSystem(context.client);
		l.part_buffer->FlushAppendState(*l.part_buffer_append_state);
		auto &partitions = l.part_buffer->GetPartitions();
		auto partition_key_map = l.part_buffer->GetReverseMap();

		string trimmed_path = file_path;
		StringUtil::RTrim(trimmed_path, fs.PathSeparator());

		for (idx_t i = 0; i < partitions.size(); i++) {
			string hive_path =
			    CreateDirRecursive(partition_columns, names, partition_key_map[i]->values, trimmed_path, fs);
			string full_path(CreateFilename(fs, hive_path, function.extension, l.writer_offset));
			if (fs.FileExists(full_path) && !overwrite_or_ignore) {
				throw IOException("failed to create " + full_path +
				                  ", file exists! Enable OVERWRITE_OR_IGNORE option to force writing");
			}
			// Create a writer for the current file
			auto fun_data_global = function.copy_to_initialize_global(context.client, *bind_data, full_path);
			auto fun_data_local = function.copy_to_initialize_local(context, *bind_data);

			for (auto &chunk : partitions[i]->Chunks()) {
				function.copy_to_sink(context, *bind_data, *fun_data_global, *fun_data_local, chunk);
			}

			function.copy_to_combine(context, *bind_data, *fun_data_global, *fun_data_local);
			function.copy_to_finalize(context.client, *bind_data, *fun_data_global);
		}

		return;
	}

	if (function.copy_to_combine) {
		function.copy_to_combine(context, *bind_data, per_thread_output ? *l.global_state : *g.global_state,
		                         *l.local_state);

		if (per_thread_output) {
			function.copy_to_finalize(context.client, *bind_data, *l.global_state);
		}
	}
}

SinkFinalizeType PhysicalCopyToFile::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                              GlobalSinkState &gstate_p) const {
	auto &gstate = (CopyToFunctionGlobalState &)gstate_p;
	if (per_thread_output || partition_output) {
		// already happened in combine
		return SinkFinalizeType::READY;
	}
	if (function.copy_to_finalize) {
		function.copy_to_finalize(context, *bind_data, *gstate.global_state);

		if (use_tmp_file) {
			D_ASSERT(!per_thread_output); // FIXME
			D_ASSERT(!partition_output);  // FIXME
			MoveTmpFile(context, file_path);
		}
	}
	return SinkFinalizeType::READY;
}

unique_ptr<LocalSinkState> PhysicalCopyToFile::GetLocalSinkState(ExecutionContext &context) const {
	if (partition_output) {
		auto state = make_unique<CopyToFunctionLocalState>(nullptr);
		{
			auto &g = (CopyToFunctionGlobalState &)*sink_state;
			lock_guard<mutex> glock(g.lock);
			state->writer_offset = g.last_file_offset++;

			state->part_buffer = make_unique<HivePartitionedColumnData>(context.client, expected_types,
			                                                            partition_columns, g.partition_state);
			state->part_buffer_append_state = make_unique<PartitionedColumnDataAppendState>();
			state->part_buffer->InitializeAppendState(*state->part_buffer_append_state);
		}
		return std::move(state);
	}
	auto res = make_unique<CopyToFunctionLocalState>(function.copy_to_initialize_local(context, *bind_data));
	if (per_thread_output) {
		idx_t this_file_offset;
		{
			auto &g = (CopyToFunctionGlobalState &)*sink_state;
			lock_guard<mutex> glock(g.lock);
			this_file_offset = g.last_file_offset++;
		}
		auto &fs = FileSystem::GetFileSystem(context.client);
		string output_path(CreateFilename(fs, file_path, function.extension, this_file_offset));
		if (fs.FileExists(output_path) && !overwrite_or_ignore) {
			throw IOException("%s exists! Enable OVERWRITE_OR_IGNORE option to force writing", output_path);
		}
		res->global_state = function.copy_to_initialize_global(context.client, *bind_data, output_path);
	}
	return std::move(res);
}

unique_ptr<GlobalSinkState> PhysicalCopyToFile::GetGlobalSinkState(ClientContext &context) const {

	if (partition_output || per_thread_output) {
		auto &fs = FileSystem::GetFileSystem(context);

		if (fs.FileExists(file_path) && !overwrite_or_ignore) {
			throw IOException("%s exists! Enable OVERWRITE_OR_IGNORE option to force writing", file_path);
		}
		if (!fs.DirectoryExists(file_path)) {
			fs.CreateDirectory(file_path);
		} else if (!overwrite_or_ignore) {
			idx_t n_files = 0;
			fs.ListFiles(
			    file_path, [&n_files](const string &path, bool) { n_files++; }, FileOpener::Get(context));
			if (n_files > 0) {
				throw IOException("Directory %s is not empty! Enable OVERWRITE_OR_IGNORE option to force writing",
				                  file_path);
			}
		}

		auto state = make_unique<CopyToFunctionGlobalState>(nullptr);

		if (partition_output) {
			state->partition_state = make_shared<GlobalHivePartitionState>();
		}

		return std::move(state);
	}

	return make_unique<CopyToFunctionGlobalState>(function.copy_to_initialize_global(context, *bind_data, file_path));
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
class CopyToFileState : public GlobalSourceState {
public:
	CopyToFileState() : finished(false) {
	}

	bool finished;
};

unique_ptr<GlobalSourceState> PhysicalCopyToFile::GetGlobalSourceState(ClientContext &context) const {
	return make_unique<CopyToFileState>();
}

void PhysicalCopyToFile::GetData(ExecutionContext &context, DataChunk &chunk, GlobalSourceState &gstate,
                                 LocalSourceState &lstate) const {
	auto &state = (CopyToFileState &)gstate;
	auto &g = (CopyToFunctionGlobalState &)*sink_state;
	if (state.finished) {
		return;
	}

	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(g.rows_copied));
	state.finished = true;
}

} // namespace duckdb
