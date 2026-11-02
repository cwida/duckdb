#include "duckdb/storage/metadata/metadata_writer.hpp"

#include "duckdb/common/numeric_utils.hpp"

namespace duckdb {

MetadataWriter::MetadataWriter(MetadataManager &manager, optional_ptr<vector<MetaBlockPointer>> written_pointers_p)
    : manager(manager), written_pointers(written_pointers_p), capacity(0), offset(0) {
	D_ASSERT(!written_pointers || written_pointers->empty());
}

MetadataWriter::~MetadataWriter() {
	// If there's an exception during checkpoint, this can get destroyed without
	// flushing the data...which is fine, because none of the unwritten data
	// will be referenced.
	//
	// Otherwise, we should have explicitly flushed (and thereby nulled the block).
	D_ASSERT(!block.handle.IsValid() || Exception::UncaughtException());
}

BlockPointer MetadataWriter::GetBlockPointer() {
	return MetadataManager::ToBlockPointer(GetMetaBlockPointer(), GetManager().GetMetadataBlockSize());
}

MetaBlockPointer MetadataWriter::GetMetaBlockPointer() {
	if (offset >= capacity) {
		// at the end of the block - fetch the next block
		NextBlock();
		D_ASSERT(capacity > 0);
	}
	return manager.GetDiskPointer(block.pointer, UnsafeNumericCast<uint32_t>(offset));
}

void MetadataWriter::SetWrittenPointers(optional_ptr<vector<MetaBlockPointer>> written_pointers_p) {
	written_pointers = written_pointers_p;
	if (written_pointers && capacity > 0 && offset < capacity) {
		written_pointers->push_back(manager.GetDiskPointer(current_pointer));
	}
}

MetadataHandle MetadataWriter::NextHandle() {
	return manager.AllocateHandle();
}

void MetadataWriter::NextBlock() {
	// now we need to get a new block id
	auto new_handle = NextHandle();

	// write the block id of the new block to the start of the current block
	if (capacity > 0) {
		auto disk_block = manager.GetDiskPointer(new_handle.pointer);
		Store<idx_t>(disk_block.block_pointer, BasePtr());
	}
	// now update the block id of the block
	block = std::move(new_handle);
	current_pointer = block.pointer;
	offset = sizeof(idx_t);
	capacity = GetManager().GetMetadataBlockSize();
	Store<idx_t>(static_cast<idx_t>(-1), BasePtr());
	if (written_pointers) {
		written_pointers->push_back(manager.GetDiskPointer(current_pointer));
	}
}

void MetadataWriter::WriteData(const_data_ptr_t buffer, idx_t write_size) {
	while (offset + write_size > capacity) {
		// we need to make a new block
		// first copy what we can
		D_ASSERT(offset <= capacity);
		idx_t copy_amount = capacity - offset;
		if (copy_amount > 0) {
			memcpy(Ptr(), buffer, copy_amount);
			buffer += copy_amount;
			offset += copy_amount;
			write_size -= copy_amount;
		}
		// move forward to the next block
		NextBlock();
	}
	memcpy(Ptr(), buffer, write_size);
	offset += write_size;
}

void MetadataWriter::Flush() {
	if (offset < capacity) {
		// clear remaining bytes of block (if any)
		memset(Ptr(), 0, capacity - offset);
	}
	block.handle.Destroy();
}

data_ptr_t MetadataWriter::BasePtr() {
	return block.handle.GetDataMutable() + current_pointer.index * GetManager().GetMetadataBlockSize();
}

data_ptr_t MetadataWriter::Ptr() {
	return BasePtr() + offset;
}

} // namespace duckdb
