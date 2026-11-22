#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

namespace jai {

// Every permission is an int32_t id: an enum passed where a permission goes is one, its underlying value its id. The engine's
// own are jai::permissions; a host declares its own as an enum over std::int32_t starting at jai::permissions::count:
//   enum class GamePermission : std::int32_t { cheats = jai::permissions::count, editor };
// Ids are the host's to keep unique across its enums; two enums that share an id name one permission.
enum class permission : std::int32_t {
    filesystem,
    untrusted_template,
    network,
    debug,
    trusted,
    untrusted,
    source_receivers,
};

namespace permissions {
    // enforced by the engine: files and paths, untrusted templates under their own root (detail/template_library.hpp), and
    // jai_spider's requests and the runner's download.start
    inline constexpr permission filesystem = permission::filesystem;
    inline constexpr permission untrusted_template = permission::untrusted_template;
    inline constexpr permission network = permission::network;
    // saved signal receivers that are source text: a script's text connect and connect_source, and a load that arms one
    inline constexpr permission source_receivers = permission::source_receivers;
    // conventions the engine enforces nothing with: what each one guards is the host's to decide
    inline constexpr permission debug = permission::debug;
    inline constexpr permission trusted = permission::trusted;
    inline constexpr permission untrusted = permission::untrusted;
    // the first id of a host's own
    inline constexpr std::int32_t count = 7;
}

namespace detail {
    template<typename E>
    concept permission_enum = std::is_enum_v<E>;

    template<permission_enum E>
    constexpr std::int32_t permission_id(E a_permission) noexcept {
        return static_cast<std::int32_t>(static_cast<std::underlying_type_t<E>>(a_permission));
    }
}

// A set of ids: what a call requires, what an engine grants. Ids 0 to 255 are bits in place, so a check over them is four word
// tests; any other id lives in a sorted spill the check reads only when the requirement has one.
class permission_set {
public:
    permission_set() noexcept = default;

    template<detail::permission_enum E>
    permission_set(E a_permission) { add(detail::permission_id(a_permission)); }

    permission_set& add(std::int32_t a_id) {
        if (a_id >= 0 && a_id < inline_ids) {
            words[static_cast<std::size_t>(a_id) / 64] |= std::uint64_t{1} << (static_cast<std::size_t>(a_id) % 64);
        } else {
            add_spilled(a_id);
        }
        return *this;
    }
    permission_set& merge(const permission_set& a_other);
    permission_set& remove(const permission_set& a_other);

    [[nodiscard]] bool empty() const noexcept {
        return spill.empty() && (words[0] | words[1] | words[2] | words[3]) == 0;
    }
    [[nodiscard]] bool contains(std::int32_t a_id) const noexcept;
    // The lowest id of a_required this set lacks
    [[nodiscard]] std::optional<std::int32_t> first_missing(const permission_set& a_required) const;
    [[nodiscard]] bool covers(const permission_set& a_required) const noexcept {
        const std::uint64_t missing = (a_required.words[0] & ~words[0]) | (a_required.words[1] & ~words[1]) |
                                      (a_required.words[2] & ~words[2]) | (a_required.words[3] & ~words[3]);
        return missing == 0 && (a_required.spill.empty() || covers_spilled(a_required));
    }

    friend bool operator==(const permission_set&, const permission_set&) = default;

private:
    static constexpr std::int32_t inline_ids = 256;

    void add_spilled(std::int32_t a_id);
    [[nodiscard]] bool covers_spilled(const permission_set& a_required) const noexcept;

    std::array<std::uint64_t, inline_ids / 64> words{};
    std::vector<std::int32_t> spill;
};

// Trailing options beside the permissions: a script statement may not drop the result (as a script declares [[nodiscard]]),
// the call touches state other objects share, so a parallel body may not make it, and the time the call takes is the host's
// (a load, a wait on a service), which the execution budget does not count (detail/host_time.hpp)
struct nodiscard_option {};
struct serial_option {};
struct exclude_execution_time_option {};
inline constexpr nodiscard_option nodiscard{};
inline constexpr serial_option serial{};
inline constexpr exclude_execution_time_option exclude_execution_time{};

// What a call with type arguments answers, in terms of its type arguments (docs/site/embedding/17-host-api.html "Type
// arguments"): jai::returns<jai::type_arg> is the first type argument itself, jai::returns<jai::array_of<jai::type_arg>> an array of it,
// jai::returns<jai::shared_ptr_of<jai::type_arg>> a shared_ptr to it. The checker types the call so (auto rows = db.query<User>(...)
// is an array<User>); the host returns a value of that type.
struct type_arg {};   // the call's first type argument
template<typename Element>
struct array_of {};
template<typename Pointee>
struct shared_ptr_of {};

struct type_result_option {
    std::uint8_t shape = 0;
};

namespace detail {
    // A result shape the checker knows how to build: a new shape (a map_of, a second placeholder) is one more specialization
    // and its case in the checker's type_argument_result
    template<typename Shape>
    struct type_result_shape {
        static constexpr std::uint8_t value = 0;
    };
    template<>
    struct type_result_shape<type_arg> {
        static constexpr std::uint8_t value = 1;
    };
    template<>
    struct type_result_shape<array_of<type_arg>> {
        static constexpr std::uint8_t value = 2;
    };
    template<>
    struct type_result_shape<shared_ptr_of<type_arg>> {
        static constexpr std::uint8_t value = 3;
    };

    template<typename Shape>
    constexpr type_result_option make_type_result() {
        static_assert(type_result_shape<Shape>::value != 0,
            "jai::returns<Shape>: Shape is jai::type_arg, jai::array_of<jai::type_arg> or jai::shared_ptr_of<jai::type_arg>");
        return type_result_option{type_result_shape<Shape>::value};
    }
}

template<typename Shape>
inline constexpr type_result_option returns = detail::make_type_result<Shape>();

// What the trailing options of add_function, add_variadic_function and a binder's method, static_method and constructor add up
// to: any number, in any order, as in add_function("save", fn, jai::nodiscard, jai::serial, GamePermission::cheats)
struct function_options {
    permission_set required;
    bool nodiscard = false;
    bool serial = false;
    std::uint8_t type_result = 0;   // type_result_option::shape
};

namespace detail {
    template<typename O>
    concept function_option = permission_enum<std::remove_cvref_t<O>> || std::is_same_v<std::remove_cvref_t<O>, nodiscard_option> ||
                              std::is_same_v<std::remove_cvref_t<O>, serial_option> || std::is_same_v<std::remove_cvref_t<O>, exclude_execution_time_option> ||
                              std::is_same_v<std::remove_cvref_t<O>, type_result_option>;

    // Whether a binding's options carry jai::exclude_execution_time: read where the binding is made, so an unmarked binding
    // compiles what it always did and a marked one stores the timed callable (engine::time_excluded_function)
    template<typename... Options>
    inline constexpr bool excludes_execution_time = (std::is_same_v<std::remove_cvref_t<Options>, exclude_execution_time_option> || ...);

    template<typename O>
    void add_function_option(function_options& a_options, const O& a_option) {
        if constexpr (std::is_same_v<O, nodiscard_option>) {
            a_options.nodiscard = true;
        } else if constexpr (std::is_same_v<O, serial_option>) {
            a_options.serial = true;
        } else if constexpr (std::is_same_v<O, type_result_option>) {
            a_options.type_result = a_option.shape;
        } else if constexpr (std::is_same_v<O, exclude_execution_time_option>) {
        } else {
            a_options.required.add(permission_id(a_option));
        }
    }

    template<function_option... Options>
    function_options fold_function_options(const Options&... a_options) {
        function_options result;
        (add_function_option(result, a_options), ...);
        return result;
    }

    template<permission_enum... Permissions>
    permission_set permission_set_of(Permissions... a_permissions) {
        permission_set result;
        (result.add(permission_id(a_permissions)), ...);
        return result;
    }
}

} // namespace jai
