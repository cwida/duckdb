#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/bit_operations.hpp"
#include <algorithm>
#include <ctgmath>
#include <cstring>

namespace duckdb {

ART::ART(const vector<column_t> &column_ids, const vector<unique_ptr<Expression>> &unbound_expressions, bool is_unique,
         bool is_primary)
    : Index(IndexType::ART, column_ids, unbound_expressions, is_unique, is_primary) {
	tree = nullptr;
	expression_result.Initialize(logical_types);
	is_little_endian = IsLittleEndian();
	for (idx_t i = 0; i < types.size(); i++) {
		switch (types[i]) {
		case PhysicalType::BOOL:
		case PhysicalType::INT8:
		case PhysicalType::INT16:
		case PhysicalType::INT32:
		case PhysicalType::INT64:
		case PhysicalType::UINT8:
		case PhysicalType::UINT16:
		case PhysicalType::UINT32:
		case PhysicalType::UINT64:
		case PhysicalType::FLOAT:
		case PhysicalType::DOUBLE:
		case PhysicalType::VARCHAR:
			break;
		default:
			throw InvalidTypeException(logical_types[i], "Invalid type for index");
		}
	}
}

ART::~ART() {
}

bool ART::LeafMatches(Node *node, Key &key, unsigned depth) {
	auto leaf = static_cast<Leaf *>(node);
	Key &leaf_key = *leaf->value;
	for (idx_t i = depth; i < leaf_key.len; i++) {
		if (leaf_key[i] != key[i]) {
			return false;
		}
	}

	return true;
}

unique_ptr<IndexScanState> ART::InitializeScanSinglePredicate(Transaction &transaction, Value value,
                                                              ExpressionType expression_type) {
	auto result = make_unique<ARTIndexScanState>();
	result->values[0] = value;
	result->expressions[0] = expression_type;
	return move(result);
}

unique_ptr<IndexScanState> ART::InitializeScanTwoPredicates(Transaction &transaction, Value low_value,
                                                            ExpressionType low_expression_type, Value high_value,
                                                            ExpressionType high_expression_type) {
	auto result = make_unique<ARTIndexScanState>();
	result->values[0] = low_value;
	result->expressions[0] = low_expression_type;
	result->values[1] = high_value;
	result->expressions[1] = high_expression_type;
	return move(result);
}

//===--------------------------------------------------------------------===//
// Insert
//===--------------------------------------------------------------------===//
template <class T>
static void TemplatedGenerateKeys(Vector &input, idx_t count, vector<unique_ptr<Key>> &keys, bool is_little_endian) {
	VectorData idata;
	input.Orrify(count, idata);

	auto input_data = (T *)idata.data;
	for (idx_t i = 0; i < count; i++) {
		auto idx = idata.sel->get_index(i);
		if (idata.validity.RowIsValid(idx)) {
			keys.push_back(Key::CreateKey<T>(input_data[idx], is_little_endian));
		} else {
			keys.push_back(nullptr);
		}
	}
}

template <class T>
static void ConcatenateKeys(Vector &input, idx_t count, vector<unique_ptr<Key>> &keys, bool is_little_endian) {
	VectorData idata;
	input.Orrify(count, idata);

	auto input_data = (T *)idata.data;
	for (idx_t i = 0; i < count; i++) {
		auto idx = idata.sel->get_index(i);
		if (!idata.validity.RowIsValid(idx) || !keys[i]) {
			// either this column is NULL, or the previous column is NULL!
			keys[i] = nullptr;
		} else {
			// concatenate the keys
			auto old_key = move(keys[i]);
			auto new_key = Key::CreateKey<T>(input_data[idx], is_little_endian);
			auto key_len = old_key->len + new_key->len;
			auto compound_data = unique_ptr<data_t[]>(new data_t[key_len]);
			memcpy(compound_data.get(), old_key->data.get(), old_key->len);
			memcpy(compound_data.get() + old_key->len, new_key->data.get(), new_key->len);
			keys[i] = make_unique<Key>(move(compound_data), key_len);
		}
	}
}

void ART::GenerateKeys(DataChunk &input, vector<unique_ptr<Key>> &keys) {
	keys.reserve(STANDARD_VECTOR_SIZE);
	// generate keys for the first input column
	switch (input.data[0].GetType().InternalType()) {
	case PhysicalType::BOOL:
		TemplatedGenerateKeys<bool>(input.data[0], input.size(), keys, is_little_endian);
		break;
	case PhysicalType::INT8:
		TemplatedGenerateKeys<int8_t>(input.data[0], input.size(), keys, is_little_endian);
		break;
	case PhysicalType::INT16:
		TemplatedGenerateKeys<int16_t>(input.data[0], input.size(), keys, is_little_endian);
		break;
	case PhysicalType::INT32:
		TemplatedGenerateKeys<int32_t>(input.data[0], input.size(), keys, is_little_endian);
		break;
	case PhysicalType::INT64:
		TemplatedGenerateKeys<int64_t>(input.data[0], input.size(), keys, is_little_endian);
		break;
	case PhysicalType::UINT8:
		TemplatedGenerateKeys<uint8_t>(input.data[0], input.size(), keys, is_little_endian);
		break;
	case PhysicalType::UINT16:
		TemplatedGenerateKeys<uint16_t>(input.data[0], input.size(), keys, is_little_endian);
		break;
	case PhysicalType::UINT32:
		TemplatedGenerateKeys<uint32_t>(input.data[0], input.size(), keys, is_little_endian);
		break;
	case PhysicalType::UINT64:
		TemplatedGenerateKeys<uint64_t>(input.data[0], input.size(), keys, is_little_endian);
		break;
	case PhysicalType::FLOAT:
		TemplatedGenerateKeys<float>(input.data[0], input.size(), keys, is_little_endian);
		break;
	case PhysicalType::DOUBLE:
		TemplatedGenerateKeys<double>(input.data[0], input.size(), keys, is_little_endian);
		break;
	case PhysicalType::VARCHAR:
		TemplatedGenerateKeys<string_t>(input.data[0], input.size(), keys, is_little_endian);
		break;
	default:
		throw InternalException("Invalid type for index");
	}

	for (idx_t i = 1; i < input.ColumnCount(); i++) {
		// for each of the remaining columns, concatenate
		switch (input.data[i].GetType().InternalType()) {
		case PhysicalType::BOOL:
			ConcatenateKeys<bool>(input.data[i], input.size(), keys, is_little_endian);
			break;
		case PhysicalType::INT8:
			ConcatenateKeys<int8_t>(input.data[i], input.size(), keys, is_little_endian);
			break;
		case PhysicalType::INT16:
			ConcatenateKeys<int16_t>(input.data[i], input.size(), keys, is_little_endian);
			break;
		case PhysicalType::INT32:
			ConcatenateKeys<int32_t>(input.data[i], input.size(), keys, is_little_endian);
			break;
		case PhysicalType::INT64:
			ConcatenateKeys<int64_t>(input.data[i], input.size(), keys, is_little_endian);
			break;
		case PhysicalType::UINT8:
			ConcatenateKeys<uint8_t>(input.data[i], input.size(), keys, is_little_endian);
			break;
		case PhysicalType::UINT16:
			ConcatenateKeys<uint16_t>(input.data[i], input.size(), keys, is_little_endian);
			break;
		case PhysicalType::UINT32:
			ConcatenateKeys<uint32_t>(input.data[i], input.size(), keys, is_little_endian);
			break;
		case PhysicalType::UINT64:
			ConcatenateKeys<uint64_t>(input.data[i], input.size(), keys, is_little_endian);
			break;
		case PhysicalType::FLOAT:
			ConcatenateKeys<float>(input.data[i], input.size(), keys, is_little_endian);
			break;
		case PhysicalType::DOUBLE:
			ConcatenateKeys<double>(input.data[i], input.size(), keys, is_little_endian);
			break;
		case PhysicalType::VARCHAR:
			ConcatenateKeys<string_t>(input.data[i], input.size(), keys, is_little_endian);
			break;
		default:
			throw InternalException("Invalid type for index");
		}
	}
}

bool ART::Insert(IndexLock &lock, DataChunk &input, Vector &row_ids) {
	D_ASSERT(row_ids.GetType().InternalType() == ROW_TYPE);
	D_ASSERT(logical_types[0] == input.data[0].GetType());

	// generate the keys for the given input
	vector<unique_ptr<Key>> keys;
	GenerateKeys(input, keys);

	// now insert the elements into the index
	row_ids.Normalify(input.size());
	auto row_identifiers = FlatVector::GetData<row_t>(row_ids);
	idx_t failed_index = INVALID_INDEX;
	for (idx_t i = 0; i < input.size(); i++) {
		if (!keys[i]) {
			continue;
		}

		row_t row_id = row_identifiers[i];
		if (!Insert(tree, move(keys[i]), 0, row_id)) {
			// failed to insert because of constraint violation
			failed_index = i;
			break;
		}
	}
	if (failed_index != INVALID_INDEX) {
		// failed to insert because of constraint violation: remove previously inserted entries
		// generate keys again
		keys.clear();
		GenerateKeys(input, keys);
		unique_ptr<Key> key;

		// now erase the entries
		for (idx_t i = 0; i < failed_index; i++) {
			if (!keys[i]) {
				continue;
			}
			row_t row_id = row_identifiers[i];
			Erase(tree, *keys[i], 0, row_id);
		}
		return false;
	}
	return true;
}

bool ART::Append(IndexLock &lock, DataChunk &appended_data, Vector &row_identifiers) {
	DataChunk expression_result;
	expression_result.Initialize(logical_types);

	// first resolve the expressions for the index
	ExecuteExpressions(appended_data, expression_result);

	// now insert into the index
	return Insert(lock, expression_result, row_identifiers);
}

void ART::VerifyAppend(DataChunk &chunk) {
	if (!is_unique) {
		return;
	}

	DataChunk expression_result;
	expression_result.Initialize(logical_types);

	// unique index, check
	lock_guard<mutex> l(lock);
	// first resolve the expressions for the index
	ExecuteExpressions(chunk, expression_result);

	// generate the keys for the given input
	vector<unique_ptr<Key>> keys;
	GenerateKeys(expression_result, keys);

	for (idx_t i = 0; i < chunk.size(); i++) {
		if (!keys[i]) {
			continue;
		}
		if (Lookup(tree, *keys[i], 0) != nullptr) {
			string key_name;
			for (idx_t k = 0; k < expression_result.ColumnCount(); k++) {
				if (k > 0) {
					key_name += ", ";
				}
				key_name += unbound_expressions[k]->GetName() + ": " + expression_result.data[k].GetValue(i).ToString();
			}
			// node already exists in tree
			throw ConstraintException("duplicate key \"%s\" violates %s constraint", key_name,
			                          is_primary ? "primary key" : "unique");
		}
	}
}

bool ART::InsertToLeaf(Leaf &leaf, row_t row_id) {
	if (is_unique && leaf.num_elements != 0) {
		return false;
	}
	leaf.Insert(row_id);
	return true;
}

bool ART::Insert(unique_ptr<Node> &node, unique_ptr<Key> value, unsigned depth, row_t row_id) {
	Key &key = *value;
	if (!node) {
		// node is currently empty, create a leaf here with the key
		node = make_unique<Leaf>(*this, move(value), row_id);
		return true;
	}

	if (node->type == NodeType::NLeaf) {
		// Replace leaf with Node4 and store both leaves in it
		auto leaf = static_cast<Leaf *>(node.get());

		Key &existing_key = *leaf->value;
		uint32_t new_prefix_length = 0;
		// Leaf node is already there, update row_id vector
		if (depth + new_prefix_length == existing_key.len && existing_key.len == key.len) {
			return InsertToLeaf(*leaf, row_id);
		}
		while (existing_key[depth + new_prefix_length] == key[depth + new_prefix_length]) {
			new_prefix_length++;
			// Leaf node is already there, update row_id vector
			if (depth + new_prefix_length == existing_key.len && existing_key.len == key.len) {
				return InsertToLeaf(*leaf, row_id);
			}
		}

		unique_ptr<Node> new_node = make_unique<Node4>(*this, new_prefix_length);
		new_node->prefix_length = new_prefix_length;
		memcpy(new_node->prefix.get(), &key[depth], new_prefix_length);
		Node4::Insert(*this, new_node, existing_key[depth + new_prefix_length], node);
		unique_ptr<Node> leaf_node = make_unique<Leaf>(*this, move(value), row_id);
		Node4::Insert(*this, new_node, key[depth + new_prefix_length], leaf_node);
		node = move(new_node);
		return true;
	}

	// Handle prefix of inner node
	if (node->prefix_length) {
		uint32_t mismatch_pos = Node::PrefixMismatch(*this, node.get(), key, depth);
		if (mismatch_pos != node->prefix_length) {
			// Prefix differs, create new node
			unique_ptr<Node> new_node = make_unique<Node4>(*this, mismatch_pos);
			new_node->prefix_length = mismatch_pos;
			memcpy(new_node->prefix.get(), node->prefix.get(), mismatch_pos);
			// Break up prefix
			auto node_ptr = node.get();
			Node4::Insert(*this, new_node, node->prefix[mismatch_pos], node);
			node_ptr->prefix_length -= (mismatch_pos + 1);
			memmove(node_ptr->prefix.get(), node_ptr->prefix.get() + mismatch_pos + 1, node_ptr->prefix_length);
			unique_ptr<Node> leaf_node = make_unique<Leaf>(*this, move(value), row_id);
			Node4::Insert(*this, new_node, key[depth + mismatch_pos], leaf_node);
			node = move(new_node);
			return true;
		}
		depth += node->prefix_length;
	}

	// Recurse
	idx_t pos = node->GetChildPos(key[depth]);
	if (pos != INVALID_INDEX) {
		auto child = node->GetChild(pos);
		return Insert(*child, move(value), depth + 1, row_id);
	}
	unique_ptr<Node> new_node = make_unique<Leaf>(*this, move(value), row_id);
	Node::InsertLeaf(*this, node, key[depth], new_node);
	return true;
}

//===--------------------------------------------------------------------===//
// Delete
//===--------------------------------------------------------------------===//
void ART::Delete(IndexLock &state, DataChunk &input, Vector &row_ids) {
	DataChunk expression_result;
	expression_result.Initialize(logical_types);

	// first resolve the expressions
	ExecuteExpressions(input, expression_result);

	// then generate the keys for the given input
	vector<unique_ptr<Key>> keys;
	GenerateKeys(expression_result, keys);

	// now erase the elements from the database
	row_ids.Normalify(input.size());
	auto row_identifiers = FlatVector::GetData<row_t>(row_ids);

	for (idx_t i = 0; i < input.size(); i++) {
		if (!keys[i]) {
			continue;
		}
		Erase(tree, *keys[i], 0, row_identifiers[i]);
#ifdef DEBUG
		auto node = Lookup(tree, *keys[i], 0);
		if (node) {
			auto leaf = static_cast<Leaf *>(node);
			for (idx_t k = 0; k < leaf->num_elements; k++) {
				D_ASSERT(leaf->GetRowId(k) != row_identifiers[i]);
			}
		}
#endif
	}
}

void ART::Erase(unique_ptr<Node> &node, Key &key, unsigned depth, row_t row_id) {
	if (!node) {
		return;
	}
	// Delete a leaf from a tree
	if (node->type == NodeType::NLeaf) {
		// Make sure we have the right leaf
		if (ART::LeafMatches(node.get(), key, depth)) {
			auto leaf = static_cast<Leaf *>(node.get());
			leaf->Remove(row_id);
			if (leaf->num_elements == 0) {
				node.reset();
			}
		}
		return;
	}

	// Handle prefix
	if (node->prefix_length) {
		if (Node::PrefixMismatch(*this, node.get(), key, depth) != node->prefix_length) {
			return;
		}
		depth += node->prefix_length;
	}
	idx_t pos = node->GetChildPos(key[depth]);
	if (pos != INVALID_INDEX) {
		auto child = node->GetChild(pos);
		D_ASSERT(child);

		unique_ptr<Node> &child_ref = *child;
		if (child_ref->type == NodeType::NLeaf && LeafMatches(child_ref.get(), key, depth)) {
			// Leaf found, remove entry
			auto leaf = static_cast<Leaf *>(child_ref.get());
			leaf->Remove(row_id);
			if (leaf->num_elements == 0) {
				// Leaf is empty, delete leaf, decrement node counter and maybe shrink node
				Node::Erase(*this, node, pos);
			}
		} else {
			// Recurse
			Erase(*child, key, depth + 1, row_id);
		}
	}
}

//===--------------------------------------------------------------------===//
// Point Query
//===--------------------------------------------------------------------===//
static unique_ptr<Key> CreateKey(ART &art, PhysicalType type, Value &value) {
	D_ASSERT(type == value.type().InternalType());
	switch (type) {
	case PhysicalType::BOOL:
		return Key::CreateKey<bool>(value.value_.boolean, art.is_little_endian);
	case PhysicalType::INT8:
		return Key::CreateKey<int8_t>(value.value_.tinyint, art.is_little_endian);
	case PhysicalType::INT16:
		return Key::CreateKey<int16_t>(value.value_.smallint, art.is_little_endian);
	case PhysicalType::INT32:
		return Key::CreateKey<int32_t>(value.value_.integer, art.is_little_endian);
	case PhysicalType::INT64:
		return Key::CreateKey<int64_t>(value.value_.bigint, art.is_little_endian);
	case PhysicalType::UINT8:
		return Key::CreateKey<uint8_t>(value.value_.utinyint, art.is_little_endian);
	case PhysicalType::UINT16:
		return Key::CreateKey<uint16_t>(value.value_.usmallint, art.is_little_endian);
	case PhysicalType::UINT32:
		return Key::CreateKey<uint32_t>(value.value_.uinteger, art.is_little_endian);
	case PhysicalType::UINT64:
		return Key::CreateKey<uint64_t>(value.value_.ubigint, art.is_little_endian);
	case PhysicalType::INT128:
		return Key::CreateKey<hugeint_t>(value.value_.hugeint, art.is_little_endian);
	case PhysicalType::FLOAT:
		return Key::CreateKey<float>(value.value_.float_, art.is_little_endian);
	case PhysicalType::DOUBLE:
		return Key::CreateKey<double>(value.value_.double_, art.is_little_endian);
	case PhysicalType::VARCHAR:
		return Key::CreateKey<string_t>(string_t(value.str_value.c_str(), value.str_value.size()),
		                                art.is_little_endian);
	default:
		throw InternalException("Invalid type for index");
	}
}

bool ART::SearchEqual(ARTIndexScanState *state, idx_t max_count, vector<row_t> &result_ids) {
	auto key = CreateKey(*this, types[0], state->values[0]);
	auto leaf = static_cast<Leaf *>(Lookup(tree, *key, 0));
	if (!leaf) {
		return true;
	}
	if (leaf->num_elements > max_count) {
		return false;
	}
	for (idx_t i = 0; i < leaf->num_elements; i++) {
		row_t row_id = leaf->GetRowId(i);
		result_ids.push_back(row_id);
	}
	return true;
}

void ART::SearchEqualJoinNoFetch(Value &equal_value, idx_t &result_size) {
	//! We need to look for a leaf
	auto key = CreateKey(*this, types[0], equal_value);
	auto leaf = static_cast<Leaf *>(Lookup(tree, *key, 0));
	if (!leaf) {
		return;
	}
	result_size = leaf->num_elements;
}

Node *ART::Lookup(unique_ptr<Node> &node, Key &key, unsigned depth) {
	auto node_val = node.get();

	while (node_val) {
		if (node_val->type == NodeType::NLeaf) {
			auto leaf = static_cast<Leaf *>(node_val);
			Key &leaf_key = *leaf->value;
			//! Check leaf
			for (idx_t i = depth; i < leaf_key.len; i++) {
				if (leaf_key[i] != key[i]) {
					return nullptr;
				}
			}
			return node_val;
		}
		if (node_val->prefix_length) {
			for (idx_t pos = 0; pos < node_val->prefix_length; pos++) {
				if (key[depth + pos] != node_val->prefix[pos]) {
					return nullptr;
				}
			}
			depth += node_val->prefix_length;
		}
		idx_t pos = node_val->GetChildPos(key[depth]);
		if (pos == INVALID_INDEX) {
			return nullptr;
		}
		node_val = node_val->GetChild(pos)->get();
		D_ASSERT(node_val);

		depth++;
	}

	return nullptr;
}

//===--------------------------------------------------------------------===//
// Iterator scans
//===--------------------------------------------------------------------===//
template <bool HAS_BOUND, bool INCLUSIVE>
bool ART::IteratorScan(ARTIndexScanState *state, Iterator *it, Key *bound, idx_t max_count, vector<row_t> &result_ids) {
	bool has_next;
	do {
		if (HAS_BOUND) {
			D_ASSERT(bound);
			if (INCLUSIVE) {
				if (*it->node->value > *bound) {
					break;
				}
			} else {
				if (*it->node->value >= *bound) {
					break;
				}
			}
		}
		if (result_ids.size() + it->node->num_elements > max_count) {
			// adding these elements would exceed the max count
			return false;
		}
		for (idx_t i = 0; i < it->node->num_elements; i++) {
			row_t row_id = it->node->GetRowId(i);
			result_ids.push_back(row_id);
		}
		has_next = ART::IteratorNext(*it);
	} while (has_next);
	return true;
}

void Iterator::SetEntry(idx_t entry_depth, IteratorEntry entry) {
	if (stack.size() < entry_depth + 1) {
		stack.resize(MaxValue<idx_t>(8, MaxValue<idx_t>(entry_depth + 1, stack.size() * 2)));
	}
	stack[entry_depth] = entry;
}

bool ART::IteratorNext(Iterator &it) {
	// Skip leaf
	if ((it.depth) && ((it.stack[it.depth - 1].node)->type == NodeType::NLeaf)) {
		it.depth--;
	}

	// Look for the next leaf
	while (it.depth > 0) {
		auto &top = it.stack[it.depth - 1];
		Node *node = top.node;

		if (node->type == NodeType::NLeaf) {
			// found a leaf: move to next node
			it.node = (Leaf *)node;
			return true;
		}

		// Find next node
		top.pos = node->GetNextPos(top.pos);
		if (top.pos != INVALID_INDEX) {
			// next node found: go there
			it.SetEntry(it.depth, IteratorEntry(node->GetChild(top.pos)->get(), INVALID_INDEX));
			it.depth++;
		} else {
			// no node found: move up the tree
			it.depth--;
		}
	}
	return false;
}

//===--------------------------------------------------------------------===//
// Greater Than
// Returns: True (If found leaf >= key)
//          False (Otherwise)
//===--------------------------------------------------------------------===//
bool ART::Bound(unique_ptr<Node> &n, Key &key, Iterator &it, bool inclusive) {
	it.depth = 0;
	bool equal = false;
	if (!n) {
		return false;
	}
	Node *node = n.get();

	idx_t depth = 0;
	while (true) {
		it.SetEntry(it.depth, IteratorEntry(node, 0));
		auto &top = it.stack[it.depth];
		it.depth++;
		if (!equal) {
			while (node->type != NodeType::NLeaf) {
				node = node->GetChild(node->GetMin())->get();
				auto &c_top = it.stack[it.depth];
				c_top.node = node;
				it.depth++;
			}
		}
		if (node->type == NodeType::NLeaf) {
			// found a leaf node: check if it is bigger or equal than the current key
			auto leaf = static_cast<Leaf *>(node);
			it.node = leaf;
			// if the search is not inclusive the leaf node could still be equal to the current value
			// check if leaf is equal to the current key
			if (*leaf->value == key) {
				// if its not inclusive check if there is a next leaf
				if (!inclusive && !IteratorNext(it)) {
					return false;
				} else {
					return true;
				}
			}

			if (*leaf->value > key) {
				return true;
			}
			// Leaf is lower than key
			// Check if next leaf is still lower than key
			while (IteratorNext(it)) {
				if (*it.node->value == key) {
					// if its not inclusive check if there is a next leaf
					if (!inclusive && !IteratorNext(it)) {
						return false;
					} else {
						return true;
					}
				} else if (*it.node->value > key) {
					// if its not inclusive check if there is a next leaf
					return true;
				}
			}
			return false;
		}
		uint32_t mismatch_pos = Node::PrefixMismatch(*this, node, key, depth);
		if (mismatch_pos != node->prefix_length) {
			if (node->prefix[mismatch_pos] < key[depth + mismatch_pos]) {
				// Less
				it.depth--;
				return IteratorNext(it);
			} else {
				// Greater
				top.pos = INVALID_INDEX;
				return IteratorNext(it);
			}
		}
		// prefix matches, search inside the child for the key
		depth += node->prefix_length;

		top.pos = node->GetChildGreaterEqual(key[depth], equal);
		if (top.pos == INVALID_INDEX) {
			// Find min leaf
			top.pos = node->GetMin();
		}
		node = node->GetChild(top.pos)->get();
		//! This means all children of this node qualify as geq

		depth++;
	}
}

bool ART::SearchGreater(ARTIndexScanState *state, bool inclusive, idx_t max_count, vector<row_t> &result_ids) {
	Iterator *it = &state->iterator;
	auto key = CreateKey(*this, types[0], state->values[0]);

	// greater than scan: first set the iterator to the node at which we will start our scan by finding the lowest node
	// that satisfies our requirement
	if (!it->start) {
		bool found = ART::Bound(tree, *key, *it, inclusive);
		if (!found) {
			return true;
		}
		it->start = true;
	}
	// after that we continue the scan; we don't need to check the bounds as any value following this value is
	// automatically bigger and hence satisfies our predicate
	return IteratorScan<false, false>(state, it, nullptr, max_count, result_ids);
}

//===--------------------------------------------------------------------===//
// Less Than
//===--------------------------------------------------------------------===//
static Leaf &FindMinimum(Iterator &it, Node &node) {
	Node *next = nullptr;
	idx_t pos = 0;
	switch (node.type) {
	case NodeType::NLeaf:
		it.node = (Leaf *)&node;
		return (Leaf &)node;
	case NodeType::N4:
		next = ((Node4 &)node).child[0].get();
		break;
	case NodeType::N16:
		next = ((Node16 &)node).child[0].get();
		break;
	case NodeType::N48: {
		auto &n48 = (Node48 &)node;
		while (n48.child_index[pos] == Node::EMPTY_MARKER) {
			pos++;
		}
		next = n48.child[n48.child_index[pos]].get();
		break;
	}
	case NodeType::N256: {
		auto &n256 = (Node256 &)node;
		while (!n256.child[pos]) {
			pos++;
		}
		next = n256.child[pos].get();
		break;
	}
	}
	it.SetEntry(it.depth, IteratorEntry(&node, pos));
	it.depth++;
	return FindMinimum(it, *next);
}

bool ART::SearchLess(ARTIndexScanState *state, bool inclusive, idx_t max_count, vector<row_t> &result_ids) {
	if (!tree) {
		return true;
	}

	Iterator *it = &state->iterator;
	auto upper_bound = CreateKey(*this, types[0], state->values[0]);

	if (!it->start) {
		// first find the minimum value in the ART: we start scanning from this value
		auto &minimum = FindMinimum(state->iterator, *tree);
		// early out min value higher than upper bound query
		if (*minimum.value > *upper_bound) {
			return true;
		}
		it->start = true;
	}
	// now continue the scan until we reach the upper bound
	if (inclusive) {
		return IteratorScan<true, true>(state, it, upper_bound.get(), max_count, result_ids);
	} else {
		return IteratorScan<true, false>(state, it, upper_bound.get(), max_count, result_ids);
	}
}

//===--------------------------------------------------------------------===//
// Closed Range Query
//===--------------------------------------------------------------------===//
bool ART::SearchCloseRange(ARTIndexScanState *state, bool left_inclusive, bool right_inclusive, idx_t max_count,
                           vector<row_t> &result_ids) {
	auto lower_bound = CreateKey(*this, types[0], state->values[0]);
	auto upper_bound = CreateKey(*this, types[0], state->values[1]);
	Iterator *it = &state->iterator;
	// first find the first node that satisfies the left predicate
	if (!it->start) {
		bool found = ART::Bound(tree, *lower_bound, *it, left_inclusive);
		if (!found) {
			return true;
		}
		it->start = true;
	}
	// now continue the scan until we reach the upper bound
	if (right_inclusive) {
		return IteratorScan<true, true>(state, it, upper_bound.get(), max_count, result_ids);
	} else {
		return IteratorScan<true, false>(state, it, upper_bound.get(), max_count, result_ids);
	}
}

bool ART::Scan(Transaction &transaction, DataTable &table, IndexScanState &table_state, idx_t max_count,
               vector<row_t> &result_ids) {
	auto state = (ARTIndexScanState *)&table_state;

	D_ASSERT(state->values[0].type().InternalType() == types[0]);

	vector<row_t> row_ids;
	bool success = true;
	if (state->values[1].is_null) {
		lock_guard<mutex> l(lock);
		// single predicate
		switch (state->expressions[0]) {
		case ExpressionType::COMPARE_EQUAL:
			success = SearchEqual(state, max_count, row_ids);
			break;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			success = SearchGreater(state, true, max_count, row_ids);
			break;
		case ExpressionType::COMPARE_GREATERTHAN:
			success = SearchGreater(state, false, max_count, row_ids);
			break;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			success = SearchLess(state, true, max_count, row_ids);
			break;
		case ExpressionType::COMPARE_LESSTHAN:
			success = SearchLess(state, false, max_count, row_ids);
			break;
		default:
			throw InternalException("Operation not implemented");
		}
	} else {
		lock_guard<mutex> l(lock);
		// two predicates
		D_ASSERT(state->values[1].type().InternalType() == types[0]);
		bool left_inclusive = state->expressions[0] == ExpressionType ::COMPARE_GREATERTHANOREQUALTO;
		bool right_inclusive = state->expressions[1] == ExpressionType ::COMPARE_LESSTHANOREQUALTO;
		success = SearchCloseRange(state, left_inclusive, right_inclusive, max_count, row_ids);
	}
	if (!success) {
		return false;
	}
	if (row_ids.empty()) {
		return true;
	}
	// sort the row ids
	sort(row_ids.begin(), row_ids.end());
	// duplicate eliminate the row ids and append them to the row ids of the state
	result_ids.reserve(row_ids.size());

	result_ids.push_back(row_ids[0]);
	for (idx_t i = 1; i < row_ids.size(); i++) {
		if (row_ids[i] != row_ids[i - 1]) {
			result_ids.push_back(row_ids[i]);
		}
	}
	return true;
}

} // namespace duckdb
