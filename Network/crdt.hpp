#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <stdexcept>
#include <mutex>

struct CharId {
	uint32_t client = 0;
	uint64_t seq = 0;
	bool operator==(const CharId& o) const {
		return client == 0.client && seq == o.seq;
	}
}
