#pragma once
#ifndef __JAISCRIPT_DETAIL_HOST_MEMBER_HPP__
#define __JAISCRIPT_DETAIL_HOST_MEMBER_HPP__

#include <cstddef>
#include <cstdint>

namespace jai {
class script_value;
class engine;
}

namespace jai::detail {

// A bound C++ data member read and written in place: registered once per bound field, it reaches the member through
// its address instead of the _get_/_set_ call. read produces exactly what the bound getter returns; write takes only
// values the setter would store unchanged and answers false otherwise, so the setter keeps every conversion and text.
struct host_member_lane {
	script_value (*read)(const void* address, engine* eng) = nullptr;
	bool (*write)(void* address, const script_value& value) = nullptr;   // null: the member is read-only
	// the member's address in the receiver's C++ object, or null where the lane cannot reach it (never throws)
	void* (*locate)(const host_member_lane& lane, const script_value& receiver) = nullptr;
	// whether read serves the member's current value (null: always); a declined read takes the getter
	bool (*serves)(const void* address) = nullptr;
};

inline bool host_member_serves(const host_member_lane& lane, const void* address) {
	return !lane.serves || lane.serves(address);
}

inline constexpr int32_t k_no_slot_check = INT32_MIN;

// One receiver layout: the receiver's C++ object held under the same registered type and, for a polymorphic one, with the
// same dynamic type (vptr). A plain wrapper of a script-extensible type also requires the object's slot to hold no layer
// (slot_offset from the stored pointer): an object that gained one answers through its layer.
struct host_receiver_layout {
	uint64_t stored_type = UINT64_MAX;
	const void* vptr = nullptr;
	int32_t slot_offset = k_no_slot_check;
};

// A located member for one receiver layout: the member sits at stored pointer + delta
struct host_member_hit {
	const host_member_lane* lane = nullptr;
	std::ptrdiff_t delta = 0;
	host_receiver_layout layout;
};

// A bound method's direct lane (a class_definition::native_entry) for one receiver layout: the object the method was
// bound on sits at stored pointer + delta
struct host_method_hit {
	const void* entry = nullptr;
	std::ptrdiff_t delta = 0;
	host_receiver_layout layout;
};

} // namespace jai::detail

#endif
