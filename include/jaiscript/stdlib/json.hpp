#pragma once

#include <jaiscript/core/engine.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace jai {
namespace stdlib {

	// The script handles one load resolves: {"$id": N, "$type": T, "$val": fields} at an object's first appearance in write order,
	// {"$id": N} after it, and the weak {"$ref": N} twins alias one object per id across the archive. A load visits keys in map
	// order, not write order, so an id met before its object finds the object's archived form by index
	struct archive_handles {
		const script_value* root = nullptr;
		std::unordered_map<script_int, script_value> loaded;
		std::unordered_map<script_int, script_value> payloads;
		std::unordered_set<script_int> loading;
		bool indexed = false;
	};

	// handle_id: the archive id a handle's object is loading under, registered as soon as it is constructed so a cycle back to it resolves
	script_value reconstruct_archive_value(engine* eng, const script_value& value, bool json_string_keys, archive_handles& handles,
	                                       script_int handle_id = 0);

	// One archive's script value back to live values: classes by name, handles by id
	script_value load_archive_value(engine* eng, const script_value& value, bool json_string_keys);

	// A constructed object's saved values, in the class's property order (base first; unknown keys ignored, a missing
	// key keeps the constructed value): a script field writes its storage, a C++ property its binding, and a value that
	// does not fit converts as hot reload's retype does or keeps the constructed value (record_field_migration)
	void load_saved_fields(engine* eng, const script_value& instance, const script_map& saved,
	                       const std::function<script_value(const script_value&)>& transform);

    // json:: and the archive functions (source/implementation/stdlib/json.cpp)
    void register_json_functions(engine& eng_ref);

} // namespace stdlib
} // namespace jai
