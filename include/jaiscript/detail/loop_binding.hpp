#pragma once

#include <jaiscript/detail/environment.hpp>

#include <optional>

namespace jai::detail {

	// A for loop's variable whose cell a lambda took by reference this iteration: the cell the next iteration's variable lives
	// in, holding a copy of the value. Empty when nothing else holds the cell, and the variable carries on in it.
	inline std::optional<script_value> next_iteration_cell(const script_value& storage, engine* eng) {
		if (!script_value::is_shared_cell(storage)) { return std::nullopt; }
		script_value held(storage.deref());   // the copy may run a constructor that moves the storage
		return script_value::make_cell_reference(clone_for_assignment(held), eng);
	}

}
