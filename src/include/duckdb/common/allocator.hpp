//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/allocator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {
class Allocator;
class ClientContext;
class DatabaseInstance;
class ExecutionContext;
class ThreadContext;

struct PrivateAllocatorData {
	virtual ~PrivateAllocatorData() {
	}
};

typedef data_ptr_t (*allocate_function_ptr_t)(PrivateAllocatorData *private_data, idx_t size);
typedef void (*free_function_ptr_t)(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t size);
typedef data_ptr_t (*reallocate_function_ptr_t)(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t size);

class AllocatedData {
public:
	AllocatedData(Allocator &allocator, data_ptr_t pointer, idx_t allocated_size);
	~AllocatedData();

	data_ptr_t get() {
		return pointer;
	}
	const_data_ptr_t get() const {
		return pointer;
	}
	idx_t GetSize() const {
		return allocated_size;
	}
	void Reset();

private:
	Allocator &allocator;
	data_ptr_t pointer;
	idx_t allocated_size;
};

class Allocator {
public:
	DUCKDB_API Allocator();
	DUCKDB_API Allocator(allocate_function_ptr_t allocate_function_p, free_function_ptr_t free_function_p,
	                     reallocate_function_ptr_t reallocate_function_p,
	                     unique_ptr<PrivateAllocatorData> private_data);

	DUCKDB_API Allocator &operator=(Allocator &&allocator) noexcept = default;

	data_ptr_t AllocateData(idx_t size);
	void FreeData(data_ptr_t pointer, idx_t size);
	data_ptr_t ReallocateData(data_ptr_t pointer, idx_t size);

	unique_ptr<AllocatedData> Allocate(idx_t size) {
		return make_unique<AllocatedData>(*this, AllocateData(size), size);
	}

	static data_ptr_t DefaultAllocate(PrivateAllocatorData *private_data, idx_t size) {
		return (data_ptr_t)malloc(size);
	}
	static void DefaultFree(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t size) {
		free(pointer);
	}
	static data_ptr_t DefaultReallocate(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t size) {
		return (data_ptr_t)realloc(pointer, size);
	}
	static Allocator &Get(ClientContext &context);
	static Allocator &Get(DatabaseInstance &db);

	PrivateAllocatorData *GetPrivateData() {
		return private_data.get();
	}

	static Allocator &DefaultAllocator();

private:
	allocate_function_ptr_t allocate_function;
	free_function_ptr_t free_function;
	reallocate_function_ptr_t reallocate_function;

	unique_ptr<PrivateAllocatorData> private_data;
};

//! The ArenaAllocator is a special thread-local allocator that should be used to allocate thread-local state
//! Note that the ArenaAllocator has a unique behavior: anything allocated through the allocator CANNOT be freed
//! Instead, everything allocated will be freed at the end - when the thread is finished executing the current pipeline
//! This is problematic when the allocator is used in a loop, since the allocated memory will only grow
//! Care must be taken when using the ArenaAllocator for this reason
struct ArenaAllocator {
	static Allocator &Get(ExecutionContext &context);
	static Allocator &Get(ThreadContext &tcontext);
};

//! The BufferAllocator is a wrapper around the global allocator class that sends any allocations made through the
//! buffer manager. This makes the buffer manager aware of the memory usage, allowing it to potentially free
//! other blocks to make space in memory.
//! Note that there is a cost to doing so (several atomic operations will be performed on allocation/free).
//! As such this class should be used primarily for larger allocations.
struct BufferAllocator {
	static Allocator &Get(ClientContext &context);
};

} // namespace duckdb
