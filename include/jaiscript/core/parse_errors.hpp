#pragma once

#ifndef __JAISCRIPT_CORE_PARSE_ERRORS_HPP__
#define __JAISCRIPT_CORE_PARSE_ERRORS_HPP__

#include <system_error>
#include <string>

namespace jai {

    // A parse fails with one code: its listing (jai::parse_error's what()) says where and why
    enum class parse_error_code {
        success = 0,
        syntax_error = 1,
    };

    class parse_error_category_impl : public std::error_category {
    public:
        const char* name() const noexcept override {
            return "jaiscript.parse";
        }

        std::string message(int ev) const override;
    };

    const std::error_category& parse_error_category() noexcept;

    inline std::error_code make_error_code(parse_error_code e) noexcept {
        return {static_cast<int>(e), parse_error_category()};
    }

} // namespace jai

namespace std {
    template <>
    struct is_error_code_enum<jai::parse_error_code> : true_type {};
}

#endif // __JAISCRIPT_CORE_PARSE_ERRORS_HPP__
