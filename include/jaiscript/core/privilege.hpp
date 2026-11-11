#pragma once

#include <cstdint>

namespace jai {

// Engine-owned grants. Only implemented/enforced capabilities belong in this set.
// A scoped enum leaves room for privilege::serialization without colliding with
// the existing jai::serialization C++ namespace.
enum class privilege : uint64_t {
    none = 0,
    filesystem = 1,
    scoped_template = 2,   // scoped_template and scoped_template_text: untrusted templates read under their own root (detail/template_library.hpp)
};

inline constexpr privilege filesystem = privilege::filesystem;
inline constexpr privilege scoped_template = privilege::scoped_template;

constexpr privilege operator|(privilege left, privilege right) noexcept {
    return static_cast<privilege>(static_cast<uint64_t>(left) | static_cast<uint64_t>(right));
}

constexpr privilege operator&(privilege left, privilege right) noexcept {
    return static_cast<privilege>(static_cast<uint64_t>(left) & static_cast<uint64_t>(right));
}

// The trailing option of add_function, add_variadic_function and a binder's method and static_method: the privilege the
// call requires, whether a script statement may drop the result (nodiscard, as a script declares [[nodiscard]]), and whether
// the call touches state other objects share, so a parallel body may not make it (serial). They combine with |, as in
// jai::nodiscard | jai::serial | jai::filesystem.
struct function_options {
    privilege required = privilege::none;
    bool nodiscard = false;
    bool serial = false;
};

inline constexpr function_options nodiscard{privilege::none, true, false};
inline constexpr function_options serial{privilege::none, false, true};

constexpr function_options operator|(function_options left, function_options right) noexcept {
    return {left.required | right.required, left.nodiscard || right.nodiscard, left.serial || right.serial};
}

constexpr function_options operator|(function_options left, privilege right) noexcept { return {left.required | right, left.nodiscard, left.serial}; }

constexpr function_options operator|(privilege left, function_options right) noexcept { return {left | right.required, right.nodiscard, right.serial}; }

constexpr bool valid_privileges(privilege flags) noexcept {
    return (static_cast<uint64_t>(flags) & ~(static_cast<uint64_t>(privilege::filesystem) | static_cast<uint64_t>(privilege::scoped_template))) == 0;
}

} // namespace jai
