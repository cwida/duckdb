#include "duckdb/main/http/http_client_pool.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {

bool HTTPClientPool::ClientKey::operator==(const ClientKey &other) const {
	return provider_epoch == other.provider_epoch && connection_epoch == other.connection_epoch &&
	       session_id == other.session_id && reuse_domain == other.reuse_domain && origin_hash == other.origin_hash &&
	       transport_config_hash == other.transport_config_hash;
}

hash_t HTTPClientPool::ClientKeyHash::operator()(const ClientKey &key) const {
	auto result = std::hash<idx_t> {}(key.provider_epoch);
	auto combine = [&](hash_t value) {
		result ^= value + 0x9e3779b9U + (result << 6U) + (result >> 2U);
	};
	combine(std::hash<uint64_t> {}(key.connection_epoch));
	combine(std::hash<idx_t> {}(key.session_id));
	combine(std::hash<idx_t> {}(key.reuse_domain));
	combine(key.origin_hash);
	combine(key.transport_config_hash);
	return result;
}

HTTPClientPool::BucketHandle::BucketHandle(ClientBucket &bucket_p) : bucket(bucket_p) {
}

bool HTTPClientPool::BucketHandle::IsValid() const {
	return bool(bucket);
}

const HTTPClientPool::ClientKey &HTTPClientPool::BucketHandle::GetKey() const {
	D_ASSERT(bucket);
	return bucket->key;
}

HTTPClientPool::HTTPClientPool(idx_t capacity_p) : capacity(capacity_p) {
	client_buckets.reserve(capacity);
	non_empty_buckets.reserve(capacity);
}

void HTTPClientPool::SetCapacity(idx_t new_capacity) {
	D_ASSERT(new_capacity > 0);
	if (new_capacity > capacity) {
		client_buckets.reserve(new_capacity);
		non_empty_buckets.reserve(new_capacity);
		for (auto &entry : client_buckets) {
			entry.second.idle_clients.reserve(new_capacity);
		}
	}
	capacity = new_capacity;
	WakeNextAdmission();
}

idx_t HTTPClientPool::GetCapacity() const {
	return capacity;
}

bool HTTPClientPool::HasAdmissionResource() const {
	return reserved_clients < capacity || (reserved_clients == capacity && !non_empty_buckets.empty());
}

void HTTPClientPool::WaitForAdmission(annotated_unique_lock<annotated_mutex> &guard, bool owns_capacity) {
	if (closed) {
		throw InvalidInputException("HTTP transport manager is closed");
	}
	if (admission_waiters.empty() && HasAdmissionResource()) {
		return;
	}
	if (owns_capacity) {
		throw InvalidInputException("Nested HTTP client acquisition would wait for manager capacity");
	}
	AdmissionWaiter waiter;
	admission_waiters.emplace_back(waiter);
	try {
		waiter.availability.wait(
		    guard, [&]() { return closed || (admission_waiters.front().get() == &waiter && HasAdmissionResource()); });
	} catch (...) {
		RemoveAdmissionWaiter(waiter);
		throw;
	}
	if (closed) {
		RemoveAdmissionWaiter(waiter);
		throw InvalidInputException("HTTP transport manager is closed");
	}
	D_ASSERT(admission_waiters.front().get() == &waiter);
	admission_waiters.pop_front();
}

void HTTPClientPool::WakeNextAdmission() {
	if (!admission_waiters.empty() && (closed || HasAdmissionResource())) {
		admission_waiters.front()->availability.notify_one();
	}
}

void HTTPClientPool::RemoveAdmissionWaiter(AdmissionWaiter &waiter) {
	for (auto entry = admission_waiters.begin(); entry != admission_waiters.end(); ++entry) {
		if (entry->get() != &waiter) {
			continue;
		}
		const bool was_front = entry == admission_waiters.begin();
		admission_waiters.erase(entry);
		if (was_front) {
			WakeNextAdmission();
		}
		return;
	}
	D_ASSERT(false);
}

void HTTPClientPool::Close() noexcept {
	closed = true;
	for (auto waiter : admission_waiters) {
		waiter->availability.notify_one();
	}
}

bool HTTPClientPool::IsClosed() const {
	return closed;
}

HTTPClientPool::Reservation HTTPClientPool::Reserve(const ClientKey &key, const string &origin,
                                                    const HTTPTransportConfig &transport_config, bool cacheable) {
	D_ASSERT(!closed);
	D_ASSERT(HasAdmissionResource());
	auto exact = cacheable ? FindBucket(key, origin, transport_config) : client_buckets.end();
	Reservation result;
	result.key = key;
	result.cacheable = cacheable;
	if (exact != client_buckets.end() && !exact->second.idle_clients.empty()) {
		result.bucket = BucketHandle(exact->second);
		result.client = TakeIdleClient(exact->second);
		result.kind = ReservationKind::REUSE;
		WakeNextAdmission();
		return result;
	}

	D_ASSERT(reserved_clients <= capacity);
	result.bucket_capacity = capacity;
	if (reserved_clients < capacity) {
		reserved_clients++;
	} else {
		auto replacement_bucket = non_empty_buckets.back();
		result.client = TakeIdleClient(*replacement_bucket);
		D_ASSERT(replacement_bucket->reserved_clients > 0);
		replacement_bucket->reserved_clients--;
		if (replacement_bucket->reserved_clients == 0) {
			result.bucket_node.node = ExtractBucket(*replacement_bucket);
		}
	}
	if (exact != client_buckets.end()) {
		exact->second.reserved_clients++;
		result.bucket = BucketHandle(exact->second);
	}
	WakeNextAdmission();
	return result;
}

bool HTTPClientPool::Reservation::PrepareBucket(const string &origin, const HTTPTransportConfig &transport_config) {
	if (!cacheable || bucket.IsValid()) {
		return false;
	}
	if (bucket_node.node) {
		auto &new_bucket = bucket_node.node.mapped();
		D_ASSERT(new_bucket.reserved_clients == 0);
		D_ASSERT(new_bucket.idle_clients.empty());
		D_ASSERT(!new_bucket.non_empty_index.IsValid());
		D_ASSERT(new_bucket.idle_clients.capacity() >= bucket_capacity);
		bucket_node.node.key() = key;
		new_bucket.key = key;
		new_bucket.origin = origin;
		new_bucket.transport_config = transport_config;
		new_bucket.reserved_clients = 1;
	} else {
		ClientBucket new_bucket;
		new_bucket.key = key;
		new_bucket.origin = origin;
		new_bucket.transport_config = transport_config;
		new_bucket.idle_clients.reserve(bucket_capacity);
		new_bucket.reserved_clients = 1;
		ClientBucketMap pending;
		pending.emplace(key, std::move(new_bucket));
		bucket_node.node = pending.extract(pending.begin());
	}
	return true;
}

void HTTPClientPool::AdoptPreparedBucket(Reservation &reservation) {
	D_ASSERT(!reservation.bucket.IsValid());
	D_ASSERT(reservation.bucket_node.node);
	auto &prepared = reservation.bucket_node.node.mapped();
	auto exact = FindBucket(reservation.key, prepared.origin, prepared.transport_config);
	if (exact != client_buckets.end()) {
		exact->second.reserved_clients++;
		reservation.bucket = BucketHandle(exact->second);
		return;
	}
	prepared.idle_clients.reserve(capacity);
	auto inserted = client_buckets.insert(std::move(reservation.bucket_node.node));
	reservation.bucket = BucketHandle(inserted->second);
}

void HTTPClientPool::Return(BucketHandle handle, unique_ptr<HTTPClient> client) {
	D_ASSERT(handle.IsValid());
	D_ASSERT(client);
	auto &bucket = *handle.bucket;
	D_ASSERT(bucket.idle_clients.size() < bucket.idle_clients.capacity());
	const bool was_empty = bucket.idle_clients.empty();
	bucket.idle_clients.push_back(std::move(client));
	if (was_empty) {
		AddNonEmptyBucket(bucket);
	}
	WakeNextAdmission();
}

HTTPClientPool::Reservation HTTPClientPool::TakeIdleForDisposal(IdleFilter filter, idx_t first, uint64_t second) {
	Reservation result;
	if (filter == IdleFilter::EXCESS && reserved_clients <= capacity) {
		return result;
	}
	for (auto &entry : client_buckets) {
		if (!entry.second.idle_clients.empty() && MatchesFilter(entry.first, filter, first, second)) {
			result.bucket = BucketHandle(entry.second);
			result.client = TakeIdleClient(entry.second);
			break;
		}
	}
	return result;
}

HTTPClientPool::DetachedBucket HTTPClientPool::FinishDestruction(BucketHandle handle) {
	D_ASSERT(reserved_clients > 0);
	reserved_clients--;
	DetachedBucket result;
	if (handle.IsValid()) {
		auto &bucket = *handle.bucket;
		D_ASSERT(bucket.reserved_clients > 0);
		bucket.reserved_clients--;
		if (bucket.reserved_clients == 0) {
			result.node = ExtractBucket(bucket);
		}
	}
	WakeNextAdmission();
	return result;
}

idx_t HTTPClientPool::ReservedClients() const {
	return reserved_clients;
}

idx_t HTTPClientPool::IdleClients() const {
	idx_t result = 0;
	for (auto &entry : client_buckets) {
		result += entry.second.idle_clients.size();
	}
	return result;
}

idx_t HTTPClientPool::BucketCount() const {
	return client_buckets.size();
}

idx_t HTTPClientPool::AdmissionWaiters() const {
	return admission_waiters.size();
}

bool HTTPClientPool::IsEmpty() const {
	return reserved_clients == 0 && client_buckets.empty() && non_empty_buckets.empty() && admission_waiters.empty();
}

HTTPClientPool::ClientBucketMap::iterator HTTPClientPool::FindBucket(const ClientKey &key, const string &origin,
                                                                     const HTTPTransportConfig &transport_config) {
	auto entry = client_buckets.find(key);
	if (entry == client_buckets.end() ||
	    (entry->second.origin == origin && entry->second.transport_config == transport_config)) {
		return entry;
	}
	auto range = client_buckets.equal_range(key);
	for (entry = range.first; entry != range.second; ++entry) {
		if (entry->second.origin == origin && entry->second.transport_config == transport_config) {
			return entry;
		}
	}
	return client_buckets.end();
}

void HTTPClientPool::AddNonEmptyBucket(ClientBucket &bucket) {
	D_ASSERT(!bucket.idle_clients.empty());
	D_ASSERT(!bucket.non_empty_index.IsValid());
	D_ASSERT(non_empty_buckets.size() < capacity);
	bucket.non_empty_index = non_empty_buckets.size();
	non_empty_buckets.push_back(bucket);
}

void HTTPClientPool::RemoveNonEmptyBucket(ClientBucket &bucket) {
	D_ASSERT(bucket.non_empty_index.IsValid());
	auto index = bucket.non_empty_index.GetIndex();
	D_ASSERT(index < non_empty_buckets.size());
	auto last_bucket = non_empty_buckets.back();
	non_empty_buckets[index] = last_bucket;
	last_bucket->non_empty_index = index;
	non_empty_buckets.pop_back();
	bucket.non_empty_index.SetInvalid();
}

unique_ptr<HTTPClient> HTTPClientPool::TakeIdleClient(ClientBucket &bucket) {
	D_ASSERT(!bucket.idle_clients.empty());
	auto result = std::move(bucket.idle_clients.back());
	bucket.idle_clients.pop_back();
	if (bucket.idle_clients.empty()) {
		RemoveNonEmptyBucket(bucket);
	}
	return result;
}

HTTPClientPool::ClientBucketMap::node_type HTTPClientPool::ExtractBucket(ClientBucket &bucket) {
	D_ASSERT(bucket.reserved_clients == 0);
	D_ASSERT(bucket.idle_clients.empty());
	D_ASSERT(!bucket.non_empty_index.IsValid());
	auto entry = FindBucket(bucket.key, bucket.origin, bucket.transport_config);
	D_ASSERT(entry != client_buckets.end());
	D_ASSERT(&entry->second == &bucket);
	return client_buckets.extract(entry);
}

bool HTTPClientPool::MatchesFilter(const ClientKey &key, IdleFilter filter, idx_t first, uint64_t second) {
	switch (filter) {
	case IdleFilter::PROVIDER:
		return key.provider_epoch == first;
	case IdleFilter::CONNECTION:
		return key.provider_epoch == first && key.connection_epoch < second;
	case IdleFilter::SESSION:
		return key.session_id == first;
	case IdleFilter::EXCESS:
	case IdleFilter::ALL:
		return true;
	}
	return false;
}

} // namespace duckdb
