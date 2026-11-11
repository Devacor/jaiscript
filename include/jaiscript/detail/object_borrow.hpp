#pragma once

#include <jaiscript/core/value.hpp>
#include <memory>
#include <unordered_map>
#include <vector>

namespace jai::detail {

// What an object borrow names (shared objects): the shared instance a worker reached through a host
// call, and the worker's own view of its C++ object (null when it holds none). parallel_object_borrow mints one per instance; the
// worker's slot keeps it until the join swaps every escaped borrow for a handle.
struct alignas(8) object_borrow_record {
    const class_instance* source = nullptr;
    engine* eng = nullptr;
    script_value view{std::monostate{}, nullptr};
};

// A worker's borrows: one record per shared instance (records never move: borrows point at them), so every wrap of one object
// is one identity, as on the serial path
struct object_borrows {
    std::vector<std::unique_ptr<object_borrow_record>> records;
    std::unordered_map<const class_instance*, const object_borrow_record*> by_source;
    void clear() noexcept { by_source.clear(); records.clear(); }
};

// The borrow a worker reads `source` through, its handle tag `tag`. Raises when the instance is an element of the region (another
// worker's own object)
script_value parallel_object_borrow(const class_instance& source, type_info_ptr tag, engine& eng);
// An object a shared object holds, as a worker reads it: a class instance as its own borrow, a bound C++ object as a holder of the
// worker's own over the same object
script_value parallel_shared_object(const script_value& object, engine& eng);
// A worker's copy of an object borrow (clone, a by-value store): the borrow again for a shared_ptr handle, else a new instance of
// the source's class holding worker-local copies of its fields
script_value parallel_object_copy(const script_value& borrow, const object_borrow_record& record);
// The value a by-value use reads (==, a conversion, a condition, its text): an object borrow, a handle's too, as the worker's own inert copy
// of the shared object, so its class's conversions run on the copy through their pins; any other value as itself
script_value parallel_by_value(const script_value& value);

}
