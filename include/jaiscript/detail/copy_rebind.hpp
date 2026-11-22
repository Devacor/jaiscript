#pragma once

// What a copy does with the closures and coroutines its source holds ("Copy and
// restore agree"): copying a value rebinds, inside the copy, every lambda, bound method and method coroutine that was bound
// to an instance inside the original. One kernel for both backends: the outermost watched copy (an instance whose class can
// hold a callable, an array or a map holding more than scalars, strings and handles) ends here when it met a callable bound
// to an instance, and no other copy does.

#include <jaiscript/core/value.hpp>

namespace jai { class class_instance; }

namespace jai::detail {

// The value is a function or a coroutine bound to a class instance: a lambda that captured this or holds a weak_ptr, a
// bound method, a method coroutine
bool binds_class_instance(const script_value& a_callable) noexcept;

// `a_copy` holds copies of `a_source`'s members (a deep copy, or the implicit copy assignment into an object that keeps its
// identity). Each closure and coroutine a_copy holds by value (a field, an element, an entry's value, at any depth of
// by-value nesting) that is bound to a_source or to an instance a_source holds by value takes that instance's copy for its
// receiver: a lambda gets its own captured values, a coroutine its own frame paused where the source's is. A coroutine
// whose state has no copy leaves a handle that is never done and raises the reason when it is resumed
void rebind_copied_callables(const class_instance& a_source, class_instance& a_copy);

// The same for an array or a map and its copy: an element's closure bound to another element is bound to that element's copy
void rebind_copied_callables(const script_value& a_source, script_value& a_copy);

} // namespace jai::detail
